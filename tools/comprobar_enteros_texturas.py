"""Check integer textures (enteros_texturas = integer textures) in production DXBC.

Execute the actual 16-bit integer conversion emitter against a small DXBC
arithmetic fake. Exhaust all R16 lookup values, reproduce BC3 address aliasing,
and check normalized/float formats, filtering, signs, masks and constant channels.
No game data, GPU, dependency or test framework is required.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

from comprobar_sincronizacion_gpu import function

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
uint32_t bits(float f) { uint32_t u; std::memcpy(&u,&f,4); return u; }
float number(uint32_t u) { float f; std::memcpy(&f,&u,4); return f; }
namespace xenos {
enum class TextureFormat : uint32_t { k_16=24 };
enum class TextureSign : uint32_t { kUnsigned=0 };
}
namespace dxbc {
struct Src {
  static constexpr uint32_t kXXXX=0, kYYYY=85;
  bool reg; uint32_t value, swizzle;
  static Src R(uint32_t r,uint32_t s=228) { return {true,r,s}; }
  static Src LU(uint32_t u) { return {false,u,0}; }
  static Src LI(int32_t i) { return LU(uint32_t(i)); }
  static Src LF(float f) { return LU(bits(f)); }
  Src Select(uint32_t c) const { auto s=*this; s.swizzle=((swizzle>>(c*2))&3)*85; return s; }
};
struct Dest {
  uint32_t reg,mask;
  static Dest R(uint32_t r,uint32_t m=15) { return {r,m}; }
};
}
struct Arithmetic {
  std::array<std::array<uint32_t,4>,16> regs{};
  std::vector<bool> scopes{true};
  uint32_t get(dxbc::Src s,uint32_t c) { return s.reg?regs.at(s.value)[(s.swizzle>>(c*2))&3]:s.value; }
  template<class Op> void binary(dxbc::Dest d,dxbc::Src a,dxbc::Src b,Op op) {
    if (!scopes.back()) return;
    auto result=regs.at(d.reg);
    for(uint32_t c=0;c<4;++c) if(d.mask&(1u<<c)) result[c]=op(get(a,c),get(b,c));
    regs.at(d.reg)=result;
  }
  void OpIAdd(dxbc::Dest d,dxbc::Src a,dxbc::Src b) { binary(d,a,b,[](auto x,auto y){return x+y;}); }
  void OpULT(dxbc::Dest d,dxbc::Src a,dxbc::Src b) { binary(d,a,b,[](auto x,auto y){return x<y?UINT32_MAX:0u;}); }
  void OpIEq(dxbc::Dest d,dxbc::Src a,dxbc::Src b) { binary(d,a,b,[](auto x,auto y){return x==y?UINT32_MAX:0u;}); }
  void OpAnd(dxbc::Dest d,dxbc::Src a,dxbc::Src b) { binary(d,a,b,[](auto x,auto y){return x&y;}); }
  void OpMul(dxbc::Dest d,dxbc::Src a,dxbc::Src b) { binary(d,a,b,[](auto x,auto y){return bits(number(x)*number(y));}); }
  void OpRoundNE(dxbc::Dest d,dxbc::Src a) { binary(d,a,a,[](auto x,auto){return bits(std::nearbyint(number(x)));}); }
  void OpUBFE(dxbc::Dest d,dxbc::Src w,dxbc::Src o,dxbc::Src v) {
    if(!scopes.back()) return;
    auto result=regs.at(d.reg);
    for(uint32_t c=0;c<4;++c) if(d.mask&(1u<<c)) result[c]=(get(v,c)>>get(o,c))&((1u<<get(w,c))-1);
    regs.at(d.reg)=result;
  }
  void OpIf(bool nz,dxbc::Src c) { scopes.push_back(scopes.back() && ((get(c,0)!=0)==nz)); }
  void OpEndIf() { assert(scopes.size()>1); scopes.pop_back(); }
};
class DxbcShaderTranslator {
 public:
  Arithmetic a_;
  uint32_t system_temp_result_=0, allocated=2;
  std::array<uint32_t,6> fetch{};
  uint32_t PushSystemTemp() { return allocated++; }
  void PopSystemTemp() { --allocated; }
  dxbc::Src RequestTextureFetchConstantWord(uint32_t i,uint32_t w) { assert(i==15); return dxbc::Src::LU(fetch.at(w)); }
  void RestoreTexture16UnsignedInteger(uint32_t,uint32_t,uint32_t,bool);
};
'''
CHECK = r'''
int main() {
  DxbcShaderTranslator t;
  t.fetch[1]=24;
  t.fetch[3]=0x00A81401; // Observed XXX1 integer lookup fetch, no address or game bytes.
  std::set<uint32_t> old_addresses,new_addresses;
  for(uint32_t v=0;v<=65535;++v) {
    float normalized=float(v)/65535.0f;
    t.a_.regs[0]={bits(normalized),bits(normalized),bits(normalized),bits(1.0f)};
    t.RestoreTexture16UnsignedInteger(15,15,1,true);
    for(unsigned c=0;c<3;++c) assert(number(t.a_.regs[0][c])==float(v));
    assert(number(t.a_.regs[0][3])==1.0f);
    assert(t.allocated==2 && t.a_.scopes.size()==1);
    if(v<1024) {
      old_addresses.insert(bits(8388608.0f+normalized*2));
      new_addresses.insert(bits(8388608.0f+number(t.a_.regs[0][0])*2));
    }
  }
  assert(old_addresses.size()==1 && new_addresses.size()==1024);
  for(uint32_t format=0;format<64;++format) for(uint32_t integer=0;integer<2;++integer) {
    t.fetch[1]=format; t.fetch[3]=integer|(0x688<<1);
    t.a_.regs[0].fill(bits(0.25f)); t.a_.regs[1].fill(0);
    t.RestoreTexture16UnsignedInteger(15,1,1,false);
    bool converts=integer && format>=24 && format<=26;
    assert(number(t.a_.regs[0][0])==(converts?0.25f*65535:0.25f));
    assert(number(t.a_.regs[0][1])==0.25f); // Destination mask.
  }
  for(uint32_t sign=1;sign<4;++sign) {
    t.fetch[1]=24; t.fetch[3]=1;
    t.a_.regs[0].fill(bits(0.25f)); t.a_.regs[1].fill(sign);
    t.RestoreTexture16UnsignedInteger(15,15,1,true);
    for(auto v:t.a_.regs[0]) assert(number(v)==0.25f);
  }
  t.fetch[3]=1|(4<<1)|(5<<4); t.a_.regs[1].fill(0);
  t.a_.regs[0]={bits(0),bits(1),bits(0.5f),bits(0.5f)};
  t.RestoreTexture16UnsignedInteger(15,3,1,true);
  assert(number(t.a_.regs[0][0])==0 && number(t.a_.regs[0][1])==1);
  puts("Production DXBC R16 integer conversion: 65536 values; 1024 distinct BC3 lookup addresses; format/filter/sign/mask/constants: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Pass --cxx pointing to the existing host clang++ compiler')
    source = (ROOT/'sdk/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp').read_text(encoding='utf-8')
    emitter = function(source, 'void DxbcShaderTranslator::RestoreTexture16UnsignedInteger(')
    assert 'RestoreTexture16UnsignedInteger(\n          tfetch_index' in source
    output = ROOT/'out/pruebas_enteros_texturas'
    output.mkdir(parents=True, exist_ok=True)
    cpp, exe = output/'actual.cpp', output/'actual.exe'
    cpp.write_text(PREAMBLE+emitter+CHECK, encoding='utf-8')
    subprocess.run([args.cxx,'-std=c++17','-fuse-ld=lld',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    main()
