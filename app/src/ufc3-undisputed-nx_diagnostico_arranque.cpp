// ufc3-undisputed-nx - writes a report when the process dies before it can say anything.
//
// THE FAILURE, AS OBSERVED
// The PC build starts, loads the game, registers its 66,314 functions and then the
// process disappears. Exit code 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN, i.e.
// __fastfail). Nothing anywhere: not in the log, not on stderr, not in the Windows
// event log.
//
// WHAT IDENTIFIES THE CAUSE
// 0xC0000409 is what the C++ runtime raises from abort(), and __fastfail bypasses
// every exception handler, vectored ones included, so the ordinary paths see
// nothing. What it does *not* bypass is the code that decides to abort: an
// unhandled exception, or the CRT's invalid-parameter handler. Hooking those, plus
// a vectored handler for the exceptions that are still visible, is what turns a
// silent death into the actual message. With it in place the real failure is
//
//     tipo lanzado: .?AVfilesystem_error@filesystem@std@@
//
// a std::filesystem::filesystem_error thrown out of rexgpu-xenos.dll and never
// caught, so the process ends with the C++ exception code 0xE06D7363.
//
// WHAT THIS DOES INSTEAD
// Installs the hooks at load time and appends one block per exception to
// arranque_fallido.txt, next to the executable: the code, the address, the
// exception's parameters, the mangled name of the C++ type and the exception's own
// message when it is a throw, and the call stack as module+offset. The address is
// enough to tell rexruntime.dll from the .exe from the recompiled game code, whose
// addresses map straight to sub_XXXXXXXX; out/depurar.py is a debugger that does
// not depend on any of this when the answer has to be exact.
//
// FOUR THINGS THAT MADE THIS FILE WRONG BEFORE IT WORKED
// 1. stdio. A vectored handler runs *inside* whatever the CRT was doing when the
//    exception was raised, so fopen/fprintf from there re-enters the CRT's
//    per-thread state and its FILE table. The handler crashed, and the crash of the
//    handler is what got reported. Everything here is raw Win32, opened once at
//    load time.
// 2. The C++ throw layout. On x64 the ThrowInfo is four 32-bit fields and the last
//    three are *RVAs from the module base*, which arrives in
//    ExceptionInformation[3] - not pointers. Reading them as pointers made the
//    handler dereference garbage and die, and because a crash inside a vectored
//    handler is invisible, the report came out empty and the real fault was never
//    seen.
// 3. Writing the block in one go at the end. The header is now written *before* the
//    type name is decoded, so a mistake in the decoding costs one line instead of
//    the whole report.
// 4. Reading the guest's structures with plain pointers. Every read of a
//    structure that belongs to someone else goes through ReadProcessMemory on our
//    own process: it fails cleanly where a pointer dereference would fault inside
//    the handler, which is the one place a fault cannot be reported.
//
// HOW TO TURN IT OFF
// Set UFC3_SIN_DIAGNOSTICO=1. The report is appended to, so delete it between runs.
//
// It is a diagnostic, not part of the port: delete this file and its line in
// app/CMakeLists.txt once the game runs.

#if defined(_WIN32)

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <exception>

namespace {

// Opened once, at load time, before any of this can be needed: a handle is safe to
// write to from an exception handler, opening a file is not.
HANDLE g_informe = INVALID_HANDLE_VALUE;
thread_local bool g_dentro = false;

void Escribir(const char* texto, DWORD largo) {
  if (g_informe != INVALID_HANDLE_VALUE) {
    DWORD escritos = 0;
    WriteFile(g_informe, texto, largo, &escritos, nullptr);
  }
}

// Deliberately hand-rolled: this runs where the CRT may be mid-call, so it must not
// call back into it.
struct Texto {
  char datos[2048];
  DWORD largo = 0;

  void Caracter(char c) {
    if (largo < sizeof(datos)) datos[largo++] = c;
  }
  void Cadena(const char* s, DWORD tope = 512) {
    while (*s && tope--) Caracter(*s++);
  }
  void HexFijo(uint64_t valor, int digitos) {
    static const char* kDigitos = "0123456789ABCDEF";
    for (int i = digitos - 1; i >= 0; --i) {
      Caracter(kDigitos[(valor >> (4 * i)) & 0xF]);
    }
  }
  // Without leading zeros: an offset is read by a person, not by a parser.
  void Hex(uint64_t valor) {
    static const char* kDigitos = "0123456789ABCDEF";
    char invertido[16];
    int n = 0;
    do {
      invertido[n++] = kDigitos[valor & 0xF];
      valor >>= 4;
    } while (valor && n < 16);
    while (n) Caracter(invertido[--n]);
  }
  void Decimal(int64_t valor) {
    if (valor < 0) {
      Caracter('-');
      valor = -valor;
    }
    char invertido[24];
    int n = 0;
    do {
      invertido[n++] = static_cast<char>('0' + valor % 10);
      valor /= 10;
    } while (valor && n < 24);
    while (n) Caracter(invertido[--n]);
  }
  void Enviar() {
    Escribir(datos, largo);
    largo = 0;
  }
};

// Every read of a structure that is not ours goes through here. A fault inside an
// exception handler cannot be reported: it replaces the exception being reported.
bool Leer(uint64_t direccion, void* destino, size_t cuantos) {
  SIZE_T leidos = 0;
  return ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(direccion), destino, cuantos,
                           &leidos) != FALSE &&
         leidos == cuantos;
}

// Basename only: the full path is the same on every line and the offset is what
// matters.
void EscribirModulo(Texto& t, void* direccion) {
  HMODULE modulo = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCSTR>(direccion), &modulo)) {
    t.Cadena("(sin modulo)");
    return;
  }
  char ruta[MAX_PATH] = "";
  GetModuleFileNameA(modulo, ruta, MAX_PATH);
  const char* base = ruta;
  for (const char* p = ruta; *p; ++p) {
    if (*p == '\\' || *p == '/') base = p + 1;
  }
  t.Cadena(base);
  t.Cadena("+0x");
  t.Hex(reinterpret_cast<uintptr_t>(direccion) - reinterpret_cast<uintptr_t>(modulo));
}

void VolcarPila(USHORT saltar) {
  void* pila[48];
  USHORT n = RtlCaptureStackBackTrace(saltar, 48, pila, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    Texto t;
    t.Cadena("  [");
    t.Decimal(i / 10);
    t.Decimal(i % 10);
    t.Cadena("] ");
    EscribirModulo(t, pila[i]);
    t.Cadena("\n");
    t.Enviar();
  }
}

// ---------------------------------------------------------------------------
//  The name and the message of a thrown C++ type (exception code 0xE06D7363)
//
//  On x64 the C++ runtime calls RaiseException with four parameters:
//     [0] 0x19930520 (magic), [1] the object, [2] the ThrowInfo, [3] the module base
//  and *everything the ThrowInfo points at is an RVA from that base*, including the
//  pointers inside the structures it points at. Reading them as absolute addresses
//  is what made this function dereference -0x11 and die.
//
//     struct ThrowInfo          { uint32 atributos, rvaUnwind, rvaCompat, rvaCatchableArray; }
//     struct CatchableTypeArray { uint32 n; uint32 rva[n]; }
//     struct CatchableType      { uint32 propiedades, rvaTipo, ...; }
//     struct TypeDescriptor     { void* vtable; void* spare; char nombre[]; }
// ---------------------------------------------------------------------------
void EscribirTipoLanzado(Texto& t, uint64_t info, uint64_t base) {
  uint32_t cabecera[4];
  if (!Leer(info, cabecera, sizeof(cabecera))) {
    return;
  }
  uint32_t arreglo[2];
  if (!Leer(base + cabecera[3], arreglo, sizeof(arreglo)) || arreglo[0] < 1) {
    return;
  }
  uint32_t entrada[2];
  if (!Leer(base + arreglo[1], entrada, sizeof(entrada)) || entrada[1] == 0) {
    return;
  }
  // TypeDescriptor: the name starts 16 bytes in.
  char nombre[201] = "";
  if (Leer(base + entrada[1] + 16, nombre, 200)) {
    t.Cadena("  tipo lanzado: ");
    t.Cadena(nombre, 200);
    t.Cadena("\n");
  }
}

// The exception object holds what() - the operation, the path and the OS error,
// which is the whole story - inside one or two std::strings whose offset depends on
// the class (system_error, filesystem_error) and on which STL the DLL that threw it
// was built with. Guessing that offset is how the first version of this file came
// out with no message at all. So: walk the object's own words, follow every word
// that looks like a pointer, and print every printable run found there. A path is
// printable and a pointer is not, so this finds the message without knowing where it
// is.
bool Imprimible(unsigned char c) { return c >= 0x20 && c < 0x7F; }

void BuscarCadenas(Texto& t, uint64_t inicio, size_t cuantos, int profundidad) {
  unsigned char datos[512];
  if (cuantos > sizeof(datos)) cuantos = sizeof(datos);
  if (!Leer(inicio, datos, cuantos)) {
    return;
  }
  for (size_t i = 0; i < cuantos; ++i) {
    if (!Imprimible(datos[i])) continue;
    size_t fin = i;
    while (fin < cuantos && Imprimible(datos[fin])) ++fin;
    if (fin - i >= 4) {
      t.Cadena("  ");
      t.Cadena(reinterpret_cast<const char*>(datos + i), static_cast<DWORD>(fin - i));
      t.Cadena("\n");
      t.Enviar();
    }
    i = fin;
  }
  if (profundidad <= 0) {
    return;
  }
  // A std::string's characters live at the pointer in its first word; the length
  // sits two words later. Only follow what is consistent with that.
  for (size_t i = 0; i + 24 <= cuantos; i += 8) {
    uint64_t palabra = 0;
    for (int k = 7; k >= 0; --k) palabra = (palabra << 8) | datos[i + k];
    if (palabra < 0x10000 || palabra > 0x7FFFFFFFFFFF) continue;
    uint64_t largo = 0;
    for (int k = 7; k >= 0; --k) largo = (largo << 8) | datos[i + 16 + k];
    if (largo == 0 || largo > 400) continue;
    Texto u;
    u.Cadena("  [cadena]\n");
    u.Enviar();
    BuscarCadenas(t, palabra, static_cast<size_t>(largo), profundidad - 1);
  }
}

void EscribirInforme(EXCEPTION_POINTERS* info) {
  const DWORD codigo = info->ExceptionRecord->ExceptionCode;
  // Benign exceptions the runtime raises on purpose (thread naming, output debug
  // string). Not worth a line.
  if (codigo == 0x406D1388 || codigo == 0x40010006) {
    return;
  }

  Texto t;
  t.Cadena("\n=== excepcion ===\ncodigo: 0x");
  t.HexFijo(codigo, 8);
  t.Cadena("\ndireccion: 0x");
  t.HexFijo(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress), 16);
  t.Cadena(" (");
  EscribirModulo(t, info->ExceptionRecord->ExceptionAddress);
  t.Cadena(")\n");
  for (DWORD k = 0; k < info->ExceptionRecord->NumberParameters && k < 4; ++k) {
    t.Cadena("  parametro[");
    t.Decimal(k);
    t.Cadena("]: 0x");
    t.HexFijo(info->ExceptionRecord->ExceptionInformation[k], 16);
    t.Cadena("\n");
  }
  t.Enviar();  // before anything that can fail.

  if (codigo == 0xE06D7363 && info->ExceptionRecord->NumberParameters >= 4) {
    Texto u;
    EscribirTipoLanzado(u, info->ExceptionRecord->ExceptionInformation[2],
                        info->ExceptionRecord->ExceptionInformation[3]);
    u.Cadena("  objeto: 0x");
    u.HexFijo(info->ExceptionRecord->ExceptionInformation[1], 16);
    u.Cadena("\n");
    u.Enviar();
    BuscarCadenas(u, info->ExceptionRecord->ExceptionInformation[1], 256, 2);
  }

  VolcarPila(1);
}

// A vectored handler is called for every exception, including the ones the runtime
// raises and handles on purpose, and for the ones raised *inside this handler*.
// Without the guard the second one is what gets written and the first is lost.
LONG CALLBACK VigilanteDeFallos(EXCEPTION_POINTERS* info) {
  if (g_dentro) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_dentro = true;
  EscribirInforme(info);
  g_dentro = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI FiltroDeUltimoRecurso(EXCEPTION_POINTERS* info) {
  if (g_dentro) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_dentro = true;
  EscribirInforme(info);
  g_dentro = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

// __fastfail (0xC0000409) bypasses every handler, vectored ones included, so the
// only place left to look is the thing that calls abort() in the first place.
void AlTerminar() {
  Texto t;
  t.Cadena("\n=== std::terminate (abort inmediato) ===\n");
  t.Enviar();
  VolcarPila(1);
  FlushFileBuffers(g_informe);
  TerminateProcess(GetCurrentProcess(), 3);
}

void ParametroInvalido(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  AlTerminar();
}

struct Instalador {
  Instalador() {
    if (std::getenv("UFC3_SIN_DIAGNOSTICO") != nullptr) {
      return;
    }
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char ruta[MAX_PATH + 32] = "";
    int n = 0;
    for (const char* p = exe; *p && n < MAX_PATH; ++p) ruta[n++] = *p;
    while (n > 0 && ruta[n - 1] != '\\' && ruta[n - 1] != '/') --n;  // strip the file name
    const char* sufijo = "arranque_fallido.txt";
    for (const char* p = sufijo; *p && n < MAX_PATH + 31; ++p) ruta[n++] = *p;
    ruta[n] = '\0';

    g_informe = CreateFileA(ruta, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    AddVectoredExceptionHandler(0, VigilanteDeFallos);
    SetUnhandledExceptionFilter(FiltroDeUltimoRecurso);
    std::set_terminate(AlTerminar);
    _set_invalid_parameter_handler(ParametroInvalido);
  }
} instalador;

}  // namespace

#endif  // _WIN32
