#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Finds the code gaps the game reaches through a pointer, and writes app/huecos.toml.

The problem
-----------
The binary dies with:

    [FATAL] Call to invalid or unregistered function at guest address 0xXXXXXXXX

when something makes an indirect call (through a pointer or a vtable) to an
address the analysis did not mark as a function start. The code generator's
Validate phase does not catch these: it only checks direct branches (b / bl).
They appear one at a time, at run time, each one after a full code generation
and a full build.

Why this works in one pass
--------------------------
An indirect call can only go to an address somebody stored somewhere. So instead
of waiting for the game to fail, the guest image is scanned for 32-bit
big-endian words that:

  * fall inside the code range,
  * are not the start of a function the generator already knows, and
  * fall inside a gap (the space between the end of one function and the start
    of the next, measured from the generated C++).

Those are the addresses the game can call through a pointer. All of them are
declared at once, which also avoids the trap tools/huecos.py warns about: when a
gap holds several entry points, declaring only its first address swallows the
rest.

Two ways an address gets into a pointer, not one
------------------------------------------------
A stored word is only half of it. PowerPC has no 32-bit immediate, so an address
is normally *built* out of two instructions, and a table of function pointers that
is written at run time is built that way too:

    0x8305E63C  addi r9,r11,0xD188     -> 0x8305D188
    0x8305E640  addi r8,r10,0xD1A0     -> 0x8305D1A0
    0x8305E644  stw  r9,0x20(r31)      the vtable the game fills in
    0x8305E64C  stw  r8,0x24(r31)

Neither address appears anywhere in the image as a stored word, so a scan for
stored words cannot see them. The one that cost a run was 0x8305D1A0:

    [FATAL] Call to invalid or unregistered function at guest address 0x8305D1A0
            (called from 0x8305DF78)

The same two instructions also build the address of a *jump table*, which must not
be declared - it is data:

    0x822F1E04  lis   r12,0x822F
    0x822F1E08  slwi  r0,r11,2
    0x822F1E0C  addi  r12,r12,0x1E1C   -> 0x822F1E1C
    0x822F1E10  lwzx  r0,r12,r0        <- used as the base of a load: a table
    0x822F1E14  mtctr r0
    0x822F1E18  bctr

Telling the two apart is one question: after the address is built, is the register
used as the base of a memory access? If it is, the value is an address *of* data
and is skipped; if it is stored or passed as an argument instead, it is an address
*of code* and is a candidate. That is what `construidas()` implements.

Where the image comes from
--------------------------
The runtime decompresses the XEX and maps it into guest memory; the file itself
cannot be read directly. Set UFC3_VOLCAR_IMAGEN to a path and run the PC build:
the app writes the 23,330,816 bytes of the image there before launching the
module. That dump is game data: it lives in out/ and is never committed.

Usage
-----
    # 1. one run of the PC build, with the dump turned on
    $env:UFC3_VOLCAR_IMAGEN = "out\\imagen_guest.bin"
    app\\out\\build\\win-amd64-release\\ufc3-undisputed-nx.exe

    # 2. the scan
    python tools/huecos_punteros.py

    # 3. code generation, which reads app/huecos.toml
    REXGLUE=sdk/out/win-amd64/rexglue.exe tools/codegen.sh app

Addresses in tools/huecos_excluir.txt are left out. Run the generator again
after changing the list.

Addresses the code generator already registers are left out too, and that is not
an optimisation. The XEX import thunks (the stubs the game calls to reach a kernel
function) are ordinary-looking code in the middle of the image, they are stored in
a table and called through a pointer, and the generator already knows every one of
them from the XEX import descriptors. Declaring one as a function replaces its
`__imp__<name>` registration with a `sub_XXXXXXXX` that has no body - the generator
does not emit one for an address it treats as an import - and the build then fails
with a page of

    lld-link: error: undefined symbol: sub_XXXXXXXX

which is a worse place to be than the warning it was meant to fix. The list of
what is already registered is read from the generated
`ufc3_undisputed_nx_register.cpp`, which at that point in the pipeline is the
baseline (generated without huecos.toml).

This is a companion to tools/huecos.py, not a replacement: huecos.py lists every
gap, this one lists only the gaps that are provably reachable.
"""
import argparse
import bisect
import json
import os
import re
import struct
import sys

RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')
RE_HEX = re.compile(r'0x([0-9A-Fa-f]{8})')
# A branch target inside a function, and the only thing that says where the body
# reaches: the label that opens a block, or the goto that jumps to one.
RE_DESTINO = re.compile(r'^(?:loc_([0-9A-Fa-f]{8}):|.*\bgoto loc_([0-9A-Fa-f]{8});)')

# From the manifest and this game's own XEX header (app/overrides.toml).
BASE_IMAGEN = 0x82000000
TAM_IMAGEN = 23330816
CODIGO_MIN = 0x82220000
CODIGO_MAX = 0x8325CDB0


def medir_funciones(gen_dir, inicios_conocidos, silencioso=False):
    """{start: (instruction_count, highest_address_in_body)}.

    The instruction count alone is not enough to know where a function ends. A
    function whose basic blocks are not contiguous emits one comment per
    instruction it has, not per byte it spans, so start + 4*count lands *before*
    its real end and the stretch in between looks like an unclaimed gap. That is
    what breaks things: declaring a function in the tail of another one clips the
    original and its branches to its own tail become unresolvable
    ("Unresolved conditional branch to 0x... from 0x...").

    So the highest address that appears anywhere in the body is measured too, and
    the larger of the two is the end. Overestimating is the safe direction: it
    makes the tool declare less, never more.

    "Anywhere in the body" means *a branch target inside it*, and nothing else. The
    generator writes those in exactly two places - the `loc_XXXXXXXX:` label that
    opens a block, and the `goto loc_XXXXXXXX;` that jumps to one - so those are the
    only two things read. Reading every 8-digit hex value instead is what the first
    version did, and it is wrong, because a body also mentions addresses that are not
    instructions inside it:

      * the entry point of *another* function. The game is full of two-instruction
        trampolines,

            0x83008840  addi r3,r3,-4
            0x83008844  b 0x8300ef78

        which the generator emits as a tail call, `sub_8300EF78(ctx, base);`. Reading
        that as "this function ends at 0x8300EF7C" makes the span swallow the 26,000
        bytes between the two, and every function start inside them is invisible to
        the scan - the game then dies at run time with

            [FATAL] Call to invalid or unregistered function at guest address 0x83008868

        (Skipping addresses that are known function starts, which the previous version
        did, only catches this when the target happens to be registered. In this image
        103 functions still stretch past their own end this way, one of them a
        three-instruction function claiming 16 MB.)

      * the return address of a call, which the generator writes as
        `ctx.lr = 0x82DE5C18;`. That value is the address *after* the calling
        instruction, so it is already the end of the body when the call is the last
        instruction - and then `alto + 4` reaches four bytes past it, into the next
        function. That is what hid the thunk at 0x82DE5C18:

            0x82DE5C14  bl   0x8324bdb4      ctx.lr = 0x82DE5C18   <- read as the end
            0x82DE5C18  addi r5,r3,0x12C     <- the next function, invisible
            0x82DE5C1C  b    0x82DE5910

        and the game died with "invalid or unregistered function at 0x82DE5C18".

      * the immediate of an `lis`/`addi` pair that builds a data address, and any
        other constant that happens to look like a code address.
    """
    tamanos = {}
    ficheros = sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp'))
    for idx, nombre in enumerate(ficheros, 1):
        actual = None
        n = 0
        alto = 0
        with open(os.path.join(gen_dir, nombre), 'r', encoding='utf-8', errors='replace') as fh:
            for linea in fh:
                if linea.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(linea)
                    if m:
                        actual = int(m.group(1), 16)
                        n = 0
                        alto = actual
                elif actual is not None:
                    if linea.startswith('\t// '):
                        n += 1
                    elif linea.startswith('}'):
                        tamanos[actual] = (n, alto)
                        actual = None
                    if actual is not None:
                        for grupos in RE_DESTINO.findall(linea):
                            destino = grupos[0] or grupos[1]
                            direccion = int(destino, 16)
                            if direccion > alto:
                                alto = direccion
        if not silencioso:
            sys.stdout.write("\r  midiendo %d/%d archivos" % (idx, len(ficheros)))
            sys.stdout.flush()
    if not silencioso:
        print()
    return tamanos


def leer_exclusiones(ruta):
    if not os.path.exists(ruta):
        return set()
    fuera = set()
    for linea in open(ruta, encoding='utf-8'):
        linea = linea.split('#')[0].strip()
        if linea.startswith('0x'):
            fuera.add(int(linea, 16))
    return fuera


RE_REGISTRO = re.compile(r'SetFunction\(0x([0-9A-Fa-f]{8}),')


# Loads and stores, by primary opcode. The D-form ones (32-47) all carry their base
# register in bits 16-20, which is all this needs: "is this register being used to
# address memory?".
OPS_CON_RA = set(range(32, 48))

# Opcode 31 is the X-form: every load and store there also carries the base in bits
# 16-20, so the field is read the same way. The three exceptions are the ones that
# put something else in those bits - mfspr/mtspr (an SPR number) and mftb (a TBR
# number) - and they are listed rather than enumerating the ~30 X-form XOs by hand,
# because one wrong magic number there would silently drop a real function address.
XO_31_SIN_RA = {339, 371, 467}


def _usa_como_base(palabras, desde, registro, ventana=8):
    """True if `registro` addresses memory in the instructions after `desde`."""
    for k in range(desde + 1, min(desde + 1 + ventana, len(palabras))):
        w = palabras[k]
        op = w >> 26
        if op in OPS_CON_RA:
            if ((w >> 16) & 0x1F) == registro:
                return True
        elif op == 31 and ((w >> 1) & 0x3FF) not in XO_31_SIN_RA:
            if ((w >> 16) & 0x1F) == registro:
                return True
    return False


def construidas(imagen):
    """Addresses of code the game builds with lis + addi/ori, not by storing them.

    See the module docstring: this is the other half of the pointer search, and the
    filter in `_usa_como_base` is what keeps jump tables out of it.

    Only the FIRST addi/ori that uses the loaded register counts. That is how a
    compiler emits it - `lis` and then immediately the offset - and stopping there is
    what keeps a later, unrelated use of the same register out of the list. The one
    that cost a codegen run was 0x82AD0004, built 16 instructions after its `lis` by

        0x82AD5E64  lis  r11,0x82AD
        0x82AD5E6C  addi r6,r11,0x5448    <- the real use: 0x82AD5448
        0x82AD5E88  mr   r11,r29          <- r11 reused for something else
        0x82AD5EA4  addi r11,r11,4        <- 0x82AD0004, a loop counter increment

    and declaring it made the generator emit "Function 0x82AD0004 has no blocks -
    generating stub". Note that `mr r11,r29` writes bits 16-20, not 21-25, so no
    cheap "does this instruction write the register" test would have caught it.
    """
    n = len(imagen) // 4
    palabras = struct.unpack_from(">%dI" % n, imagen, 0)
    encontradas = set()
    for i in range(n):
        w = palabras[i]
        if (w >> 26) != 15 or ((w >> 16) & 0x1F) != 0:  # addis with RA=0 -> lis
            continue
        registro = (w >> 21) & 0x1F
        alto = w & 0xFFFF
        base = (alto << 16) - 0x100000000 if alto & 0x8000 else (alto << 16)
        for j in range(i + 1, min(i + 17, n)):
            w2 = palabras[j]
            op2 = w2 >> 26
            if op2 not in (14, 24):  # addi / ori
                continue
            if ((w2 >> 16) & 0x1F) != registro:
                continue
            # First use of the register: this is the address, or the register was
            # loaded for something else entirely. Either way, stop looking.
            bajo = w2 & 0xFFFF
            if op2 == 14:
                valor = (base + (bajo - 0x10000 if bajo & 0x8000 else bajo)) & 0xFFFFFFFF
            else:
                valor = (base + bajo) & 0xFFFFFFFF
            if (CODIGO_MIN <= valor < CODIGO_MAX
                    and not _usa_como_base(palabras, j, (w2 >> 21) & 0x1F)):
                encontradas.add(valor)
            break
    return encontradas


def leer_registradas(gen_dir):
    """Las direcciones que el generador ya conoce, tal como salen en register.cpp.

    El fichero se llama <proyecto>_register.cpp y se genera a partir del
    manifiesto, asi que cuando esto se ejecuta (despues del codegen base) contiene
    exactamente las direcciones del juego sin huecos.toml: los cuerpos de funcion
    y los thunks de importacion del XEX. Declarar cualquiera de las dos cosas otra
    vez no aporta nada, y declarar un thunk de importacion rompe la build.
    """
    for nombre in sorted(os.listdir(gen_dir)):
        if not nombre.endswith('_register.cpp'):
            continue
        conocidas = set()
        with open(os.path.join(gen_dir, nombre), encoding='utf-8', errors='replace') as fh:
            for linea in fh:
                m = RE_REGISTRO.search(linea)
                if m:
                    conocidas.add(int(m.group(1), 16))
        return conocidas
    print("aviso: no se encontro *_register.cpp en %s; no se excluye nada" % gen_dir)
    return set()


def main():
    raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = argparse.ArgumentParser(description="Busca huecos de codigo alcanzables por puntero.")
    p.add_argument("--gen", default=os.path.join(raiz, "app", "generated", "default"),
                   help="carpeta con el codigo generado")
    p.add_argument("--imagen", default=os.path.join(raiz, "out", "imagen_guest.bin"),
                   help="volcado de la imagen del invitado (UFC3_VOLCAR_IMAGEN)")
    p.add_argument("--excluir", default=os.path.join(raiz, "tools", "huecos_excluir.txt"),
                   help="direcciones que no se deben declarar aunque sean huecos")
    p.add_argument("--salida", default=os.path.join(raiz, "app", "huecos.toml"),
                   help="fichero TOML que se escribe")
    args = p.parse_args()

    if not os.path.exists(args.imagen):
        raise SystemExit(
            "No existe %s.\nEjecuta la build de PC con UFC3_VOLCAR_IMAGEN apuntando a esa ruta."
            % args.imagen)

    with open(os.path.join(args.gen, "codegen.partition.json")) as fh:
        asignaciones = json.load(fh)["assignments"]
    inicios = sorted(int(k, 16) for k in asignaciones)
    print("funciones declaradas: %d" % len(inicios))

    tamanos = medir_funciones(args.gen, set(inicios))

    # A gap is the space no function claims: between the real end of one function
    # and the start of the next one. See medir_funciones for why the end is not
    # simply start + 4 * instruction_count.
    huecos = []
    for i, a in enumerate(inicios[:-1]):
        medida = tamanos.get(a)
        if medida is None:
            continue
        fin = max(a + 4 * medida[0], medida[1] + 4)
        if fin < inicios[i + 1]:
            huecos.append((fin, inicios[i + 1] - fin))
    huecos.sort()
    inicio_huecos = [h[0] for h in huecos]
    print("huecos: %d, bytes sin dueno: %d" % (len(huecos), sum(h[1] for h in huecos)))

    def hueco_de(direccion):
        i = bisect.bisect_right(inicio_huecos, direccion) - 1
        if i < 0:
            return None
        fin, tam = huecos[i]
        if fin <= direccion < fin + tam:
            return (fin, tam)
        return None

    # The span of every function, to tell a stored pointer from an instruction.
    # Scanning the whole image at 4-byte alignment also reads the game's code, and
    # a PowerPC instruction is 32 bits that can look like any number: `lwz r24,36(r3)`
    # IS 0x83030024, which is a perfectly valid-looking code address. Words that
    # belong to a function are instructions, not data, and must not be read as
    # pointers. This is what keeps the list at hundreds instead of thousands.
    tramos = []
    for a, medida in tamanos.items():
        if medida[0] or medida[1] != a:
            tramos.append((a, max(a + 4 * medida[0], medida[1] + 4)))
    tramos.sort()
    inicio_tramos = [t[0] for t in tramos]

    def es_codigo(direccion):
        i = bisect.bisect_right(inicio_tramos, direccion) - 1
        if i < 0:
            return False
        inicio, fin = tramos[i]
        return inicio <= direccion < fin

    with open(args.imagen, "rb") as fh:
        imagen = fh.read()
    if len(imagen) != TAM_IMAGEN:
        raise SystemExit("La imagen mide %d bytes, se esperaban %d" % (len(imagen), TAM_IMAGEN))

    excluidas = leer_exclusiones(args.excluir)
    conocidas = leer_registradas(args.gen)
    print("ya registradas por el generador: %d" % len(conocidas))
    candidatos = {}
    desalineadas = 0
    en_codigo = 0
    ya_conocidas = 0
    for desplazamiento in range(0, len(imagen) - 3, 4):
        origen = BASE_IMAGEN + desplazamiento
        if es_codigo(origen):
            en_codigo += 1
            continue
        valor = struct.unpack_from(">I", imagen, desplazamiento)[0]
        if not (CODIGO_MIN <= valor < CODIGO_MAX):
            continue
        if valor in tamanos or valor in asignaciones or valor in excluidas:
            continue
        if valor in conocidas:
            ya_conocidas += 1
            continue
        # A pointer is always aligned; an unaligned value is data that happens
        # to look like an address.
        if valor % 4:
            desalineadas += 1
            continue
        if hueco_de(valor) is None:
            continue
        candidatos.setdefault(valor, []).append(origen)

    orden = sorted(candidatos)
    print("punteros a huecos: %d direcciones (%d palabras de codigo ignoradas, "
          "%d punteros a thunks de importacion, "
          "%d valores desalineados descartados)"
          % (len(orden), en_codigo, ya_conocidas, desalineadas))

    # The addresses the game builds instead of storing. Same gap test, same
    # exclusions, and they are merged into the same list so a gap with both kinds
    # is still declared completely.
    armadas = construidas(imagen)
    nuevas = 0
    for valor in sorted(armadas):
        if valor in tamanos or valor in asignaciones or valor in excluidas:
            continue
        if valor in conocidas:
            continue
        if hueco_de(valor) is None:
            continue
        if valor in candidatos:
            continue
        candidatos[valor] = []
        nuevas += 1
    print("construidas con lis+addi/ori: %d direcciones, %d nuevas" % (len(armadas), nuevas))
    orden = sorted(candidatos)

    with open(args.salida, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("""# ============================================================================
#  UFC 3 Undisputed - the code gaps the game reaches through a pointer.
#
#  GENERATED FILE. Do not edit by hand: run
#
#      python tools/huecos_punteros.py
#
#  and it is rewritten from scratch. It is included by
#  ufc3_undisputed_nx_manifest.toml, and every address in it was derived from
#  THIS game's own default.xex (see tools/huecos_punteros.py for the method).
#
#  Why they are needed: an indirect call (through a pointer or a vtable) to an
#  address the analysis did not open as a function stops the game with
#
#      [FATAL] Call to invalid or unregistered function at guest address 0x...
#
#  The code generator's Validate phase cannot find these - it only checks direct
#  branches - so they are found by scanning the guest image for code addresses that
#  land in a gap: either stored as a 32-bit word, or built with lis + addi/ori.
#
#  %d addresses, %d stored pointers.
# ============================================================================
[functions]
""" % (len(orden), sum(len(v) for v in candidatos.values())))
        for valor in orden:
            fin, tam = hueco_de(valor)
            origen = ("%d pointer(s)" % len(candidatos[valor])
                      if candidatos[valor] else "built with lis+addi")
            fh.write('"0x%08X" = {}   # gap of %d bytes from 0x%08X, %s\n'
                     % (valor, tam, fin, origen))

    print("escrito %s (%d entradas)" % (args.salida, len(orden)))


def autocomprobar():
    """Self-check for medir_funciones: only branch targets extend a function.

    Run with `python tools/huecos_punteros.py --autocomprobar`. Each case is one that
    has actually gone wrong, and each one is an address in the body that must NOT be
    read as the body's end:

      a. a tail call to another function (this hid 0x83008868);
      b. the return address of a call (this hid 0x82DE5C18);
      c. a `lis` immediate that happens to look like a code address, which is what made
         103 functions claim spans up to 16 MB;
      d. a forward branch inside the function, which is the one thing that must extend
         it - the reason the measurement exists at all.
    """
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "a.cpp"), "w", encoding="utf-8") as fh:
            fh.write("DEFINE_REX_FUNC(sub_82220000) {\n"
                     "\tREX_FUNC_PROLOGUE();\n"
                     "\t// addi r3,r3,-4\n"
                     "\tctx.r3.s64 = ctx.r3.s64 + -4;\n"
                     "\t// b 0x82230000\n"
                     "\tsub_82230000(ctx, base);\n"
                     "\treturn;\n"
                     "}\n")
        with open(os.path.join(tmp, "b.cpp"), "w", encoding="utf-8") as fh:
            fh.write("DEFINE_REX_FUNC(sub_82220010) {\n"
                     "\tREX_FUNC_PROLOGUE();\n"
                     "\t// addi r3,r3,4\n"
                     "\tctx.r3.s64 = ctx.r3.s64 + 4;\n"
                     "\t// bl 0x82230000\n"
                     "\tctx.lr = 0x82220018;\n"
                     "\tsub_82230000(ctx, base);\n"
                     "\treturn;\n"
                     "}\n")
        with open(os.path.join(tmp, "c.cpp"), "w", encoding="utf-8") as fh:
            fh.write("DEFINE_REX_FUNC(sub_82220020) {\n"
                     "\tREX_FUNC_PROLOGUE();\n"
                     "\t// lis r11,0x8324\n"
                     "\tctx.r11.s64 = -2094792704;\n"
                     "\t// addi r11,r11,-17580\n"
                     "\tctx.r11.s64 = ctx.r11.s64 + -17580;\n"
                     "\t// lwz r3,0(r11)\n"
                     "\tctx.r3.u64 = REX_LOAD_U32(ctx.r11.u32 + 0);\n"
                     "\treturn;\n"
                     "}\n")
        with open(os.path.join(tmp, "d.cpp"), "w", encoding="utf-8") as fh:
            fh.write("DEFINE_REX_FUNC(sub_82220030) {\n"
                     "\tREX_FUNC_PROLOGUE();\n"
                     "\t// bne cr6,0x82220050\n"
                     "\tif (!ctx.cr6.eq) goto loc_82220050;\n"
                     "\treturn;\n"
                     "loc_82220050:\n"
                     "\t// blr\n"
                     "\treturn;\n"
                     "}\n")
        conocidos = {0x82220000, 0x82220010, 0x82220020, 0x82220030, 0x82230000}
        tamanos = medir_funciones(tmp, conocidos, silencioso=True)
        assert set(tamanos) == conocidos - {0x82230000}, tamanos
        assert tamanos[0x82220000][1] == 0x82220000, (
            "una llamada de cola no debe alargar el tramo: 0x%08X" % tamanos[0x82220000][1])
        assert tamanos[0x82220010][1] == 0x82220010, (
            "ni la llamada ni su direccion de retorno deben alargarlo: 0x%08X"
            % tamanos[0x82220010][1])
        assert tamanos[0x82220020][1] == 0x82220020, (
            "una constante que parece una direccion no debe alargarlo: 0x%08X"
            % tamanos[0x82220020][1])
        assert tamanos[0x82220030][1] == 0x82220050, (
            "un salto hacia delante dentro de la funcion si debe alargarlo: 0x%08X"
            % tamanos[0x82220030][1])
    print("autocomprobacion correcta")


def autocomprobar_construidas():
    """Self-check for construidas(): a stored code address yes, a jump table no.

    Run with `python tools/huecos_punteros.py --autocomprobar`. Both instruction
    sequences are copied from this game's image, and they are the two cases that
    decide whether the filter is right:

      0x8305E634  lis  r10,0x8306 / 0x8305E640 addi r8,r10,0xD1A0 / stw r8,0x24(r31)
                  -> the game fills a vtable with 0x8305D1A0. Miss this and the game
                     dies with "invalid or unregistered function at 0x8305D1A0".

      0x822F1E04  lis  r12,0x822F / slwi r0,r11,2 / addi r12,r12,0x1E1C /
                  lwzx r0,r12,r0
                  -> 0x822F1E1C is a jump table. Declaring it clips the function
                     around it and breaks the build.
    """
    almacen = bytearray()

    def ensamblar(palabras):
        for w in palabras:
            almacen.extend(struct.pack(">I", w))

    # lis r10,0x8306 | addi r8,r10,0xD1A0 | stw r8,0x24(r31)
    # The addi offset is negative, so this is 0x83060000 - 0x2E60 = 0x8305D1A0.
    ensamblar([0x3D408306, 0x390AD1A0, 0x911F0024])
    armada = 0x8305D1A0
    # lis r12,0x822F | slwi r0,r11,2 | addi r12,r12,0x1E1C | lwzx r0,r12,r0
    ensamblar([0x3D80822F, 0x5560103A, 0x398C1E1C, 0x7C0C002E])
    tabla = 0x822F1E1C

    encontradas = construidas(bytes(almacen))
    assert armada in encontradas, (
        "una direccion de codigo construida con lis+addi y guardada no se encontro: 0x%08X"
        % armada)
    assert tabla not in encontradas, (
        "una tabla de saltos se tomo por una direccion de codigo: 0x%08X" % tabla)
    assert len(encontradas) == 1, sorted("0x%08X" % v for v in encontradas)
    print("autocomprobacion de construidas() correcta")


if __name__ == "__main__":
    if "--autocomprobar" in sys.argv:
        autocomprobar()
        autocomprobar_construidas()
        sys.exit(0)
    main()
