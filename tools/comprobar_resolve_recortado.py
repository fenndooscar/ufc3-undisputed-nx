"""Check clipped tiled resolves against production scissor and rectangle code.

Compile the actual register layouts, GetScissor, float conversion and the
GetResolveInfo rectangle stage. A small memory fake supplies synthetic vertices;
no game data, GPU or dependency is needed. Negative controls retain draw scissor
semantics and prove that unsigned resolve origins lose a partial tile.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from comprobar_sincronizacion_gpu import function

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define static_assert_size(T,S) static_assert(sizeof(T)==S)
// Empty resolves are legitimate negative controls in this harness.
#define assert_true(...) ((void)0)
#define assert_always() ((void)0)
#define REXGPU_ERROR(...) ((void)0)
namespace rex {
template<class T, size_t N> constexpr size_t countof(const T (&)[N]) { return N; }
template<class T> T align(T v,T a) { return (v+a-1)&~(a-1); }
}
'''
HARNESS = r'''
struct RegisterFile {
  uint32_t values[0x5003]{};
  xenos::xe_gpu_vertex_fetch_t fetch{};
  uint32_t& operator[](uint32_t r) { return values[r]; }
  uint32_t operator[](uint32_t r) const { return values[r]; }
  template<class T> T Get() const {
    T result; std::memcpy(&result, &values[T::register_index], 4); return result;
  }
  auto GetVertexFetch(uint32_t index) const { assert(index==0); return fetch; }
};
namespace memory {
struct Memory {
  float vertices[6]{};
  const float* TranslateVirtual(uint32_t address) const {
    assert(address==0xA0004000); return vertices;
  }
};
}
namespace xenos {
constexpr uint32_t kResolveAlignmentPixels=8, kResolveAlignmentPixelsLog2=3;
constexpr uint32_t kMaxResolveSize=8192;
float GpuSwap(float f, Endian endian) {
  uint32_t u; std::memcpy(&u,&f,4);
  uint32_t mask=uint32_t(endian)==2 ? 3 : 0;
  uint8_t bytes[4]; std::memcpy(bytes,&u,4);
  uint8_t swapped[4]; for(unsigned i=0;i<4;++i) swapped[i]=bytes[i^mask];
  std::memcpy(&f,swapped,4); return f;
}
}
struct Scissor { uint32_t offset[2],extent[2]; };
struct ResolveInfo {
  struct { uint32_t packed=0,width_div_8=0; } coordinate_info;
  uint32_t height_div_8=0;
  reg::RB_COPY_CONTROL rb_copy_control{};
};
void GetScissor(const RegisterFile&, Scissor&, bool=true, bool=false);
'''
CHECK = r'''
uint32_t xy(int32_t x,int32_t y) {
  return (uint32_t(x)&0x3FFF)|((uint32_t(y)&0x3FFF)<<16);
}
RegisterFile registers(int x0,int y0,int x1,int y1) {
  RegisterFile r;
  r[XE_GPU_REG_RB_SURFACE_INFO]=2080;
  r[XE_GPU_REG_RB_COPY_CONTROL]=1<<20;
  r[XE_GPU_REG_PA_SU_VTX_CNTL]=0;
  r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL]=xy(x0,y0);
  r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]=xy(x1,y1);
  r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR]=8192|(8192<<16);
  r.fetch.type=xenos::FetchConstantType::kVertex;
  r.fetch.address=0x4000/4; r.fetch.size=6;
  r.fetch.endian=static_cast<xenos::Endian>(2);
  return r;
}
bool resolve(RegisterFile r,int x0,int y0,int x1,int y1,ResolveInfo& result) {
  memory::Memory m;
  float vertices[]={float(x0)-.5f,float(y0)-.5f,float(x1)-.5f,
                    float(y0)-.5f,float(x1)-.5f,float(y1)-.5f};
  for(unsigned i=0;i<6;++i) m.vertices[i]=xenos::GpuSwap(vertices[i],r.fetch.endian);
  return GetResolveInfo(r,m,1,1,false,false,result);
}
int main() {
  ResolveInfo result;
  auto r=registers(448,-320,1600,256);
  Scissor previous, fixed;
  GetScissor(r,previous,false); GetScissor(r,fixed,false,true);
  assert(previous.extent[1]==0); // Existing draw interpretation is unchanged.
  assert(fixed.offset[0]==448 && fixed.offset[1]==0);
  assert(fixed.extent[0]==1152 && fixed.extent[1]==256);
  assert(resolve(r,448,-320,1600,256,result));
  assert(result.coordinate_info.width_div_8==144 && result.height_div_8==32);
  // The preceding tile and ordinary positive copies keep their full region.
  assert(resolve(registers(448,704,1600,1280),448,704,1600,1280,result));
  assert(result.coordinate_info.width_div_8==144 && result.height_div_8==72);
  // The same crossing on X, both axes, and fully offscreen rectangles.
  assert(resolve(registers(-320,448,256,1600),-320,448,256,1600,result));
  assert(result.coordinate_info.width_div_8==32 && result.height_div_8==144);
  assert(resolve(registers(-320,-320,256,256),-320,-320,256,256,result));
  assert(result.coordinate_info.width_div_8==32 && result.height_div_8==32);
  assert(!resolve(registers(-576,-320,-8,-8),-576,-320,-8,-8,result));
  // Sign extension must precede the window offset, including its disable bit.
  r=registers(448,-320,1600,256);
  r[XE_GPU_REG_PA_SC_WINDOW_OFFSET]=512<<16;
  r[XE_GPU_REG_PA_SU_SC_MODE_CNTL]=1<<16;
  GetScissor(r,fixed,false,true);
  assert(fixed.offset[1]==192 && fixed.extent[1]==576);
  assert(resolve(r,448,-320,1600,256,result) && result.height_div_8==72);
  r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL]|=1u<<31;
  GetScissor(r,fixed,false,true);
  assert(fixed.offset[1]==0 && fixed.extent[1]==256);
  // Screen scissor and pitch remain effective; valid positive TL is unchanged.
  r=registers(-320,-320,256,256);
  r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL]=16|(24<<16);
  r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR]=128|(192<<16);
  r[XE_GPU_REG_RB_SURFACE_INFO]=96;
  GetScissor(r,fixed,true,true);
  assert(fixed.offset[0]==16 && fixed.extent[0]==80);
  assert(fixed.offset[1]==24 && fixed.extent[1]==168);
  for(int origin : {0,1,4096,8191}) {
    r=registers(origin,origin,8192,8192);
    GetScissor(r,previous,false); GetScissor(r,fixed,false,true);
    assert(std::memcmp(&previous,&fixed,sizeof(fixed))==0);
  }
  puts("Production tiled resolve: negative X/Y tails, positive copies, offsets, screen/pitch clipping and unchanged draws: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Pass --cxx pointing to the existing host clang++ compiler')
    draw = (ROOT/'sdk/src/graphics/util/draw.cpp').read_text(encoding='utf-8')
    registers = (ROOT/'sdk/include/rex/graphics/registers.h').read_text(encoding='utf-8')
    xenos = (ROOT/'sdk/include/rex/graphics/xenos.h').read_text(encoding='utf-8')
    table = (ROOT/'sdk/include/rex/graphics/register_table.inc').read_text(encoding='utf-8')
    enums = ('CopySampleSelect','CopyCommand','MsaaSamples','PolygonModeEnable',
             'PolygonType','PixelCenter','VertexRounding','VertexQuantization',
             'FetchConstantType','Endian')
    layouts = ('PA_SC_WINDOW_SCISSOR_TL','PA_SC_WINDOW_SCISSOR_BR','PA_SC_WINDOW_OFFSET',
               'PA_SC_SCREEN_SCISSOR_TL','PA_SC_SCREEN_SCISSOR_BR','PA_SU_SC_MODE_CNTL',
               'PA_SU_VTX_CNTL','RB_SURFACE_INFO','RB_COPY_CONTROL')
    source = PREAMBLE + 'namespace xenos {\n'
    source += '\n'.join(function(xenos, 'enum class '+name+' :')+';' for name in enums)
    source += function(xenos, 'union alignas(uint32_t) xe_gpu_vertex_fetch_t')+';\n}\n'
    source += '#define XE_GPU_REGISTER(I,T,N) XE_GPU_REG_##N=I,\nenum Register {\n'+table+'\n};\n#undef XE_GPU_REGISTER\nnamespace reg {\n'
    source += '\n'.join(function(registers, 'union alignas(uint32_t) '+name+' {')+';' for name in layouts)
    source += '\n}\nnamespace ui {\n'+function((ROOT/'sdk/src/ui/graphics_util.cpp').read_text(encoding='utf-8'), 'int32_t FloatToD3D11Fixed16p8(')+'\n}\n'
    source += HARNESS + function(draw, 'void GetScissor(')
    # Execute the actual resolve rectangle stage; later format/shader selection
    # does not affect the empty-region regression being checked here.
    rectangle = function(draw, 'bool GetResolveInfo(').split('  // 3 bits for each.', 1)[0]
    source += rectangle + '  return true;\n}\n' + CHECK
    output = ROOT/'out/pruebas_resolve_recortado'
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output/'actual.cpp', output/'actual.exe'
    cpp.write_text(source, encoding='utf-8')
    subprocess.run([args.cxx,'-std=c++17','-fuse-ld=lld',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    main()
