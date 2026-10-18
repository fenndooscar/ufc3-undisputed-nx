"""Check texture indices (indices_texturas = texture indices) in actual DXBC code.

Compile the production texture/sampler binding allocators with small host types.
A mixed four-texture shader must fit exactly in its descriptor constant buffer;
repeated bindings must reuse their slots. No GPU or test framework is needed.
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
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#define assert_always() assert(false)
namespace fmt { template<class... T> std::string format(const char*, T&&...) { return {}; } }
namespace xenos {
enum class FetchOpDimension { k1D, k2D, k3DOrStacked, kCube };
enum class TextureFilter { kPoint, kLinear, kBaseMap, kUseFetchConst };
enum class AnisoFilter { kDisabled, kMax_1_1, kMax_2_1, kMax_4_1, kMax_8_1, kMax_16_1, kUseFetchConst };
}
class DxbcShaderTranslator {
 public:
  struct TextureBinding {
    uint32_t fetch_constant = 0, bindful_srv_index = 0, bindful_srv_rdef_name_ptr = 0;
    uint32_t bindless_descriptor_index = 0;
    xenos::FetchOpDimension dimension{};
    bool is_signed = false;
    std::string bindful_name;
  };
  struct SamplerBinding {
    uint32_t fetch_constant = 0, bindless_descriptor_index = 0;
    xenos::TextureFilter mag_filter{}, min_filter{}, mip_filter{};
    xenos::AnisoFilter aniso_filter{};
    std::string bindful_name;
  };
  static constexpr uint32_t kMaxTextureBindings = 128, kMaxSamplerBindings = 64;
  static constexpr uint32_t kBindingIndexUnallocated = UINT32_MAX;
  bool bindless_resources_used_ = true;
  uint32_t srv_count_ = 0;
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  std::unordered_map<uint32_t,uint32_t> texture_bindings_for_bindful_srv_indices_;
  uint32_t GetBindlessResourceCount() const {
    return uint32_t(texture_bindings_.size() + sampler_bindings_.size());
  }
  uint32_t FindOrAddTextureBinding(uint32_t, xenos::FetchOpDimension, bool);
  uint32_t FindOrAddSamplerBinding(uint32_t, xenos::TextureFilter,
                                  xenos::TextureFilter, xenos::TextureFilter, xenos::AnisoFilter);
};
'''
CHECK = r'''
int main() {
  using F = xenos::TextureFilter;
  using A = xenos::AnisoFilter;
  using D = xenos::FetchOpDimension;
  DxbcShaderTranslator shader;
  for (uint32_t fetch : {12u, 0u, 3u, 13u}) {
    auto dimension = fetch >= 3 && fetch != 12 ? D::kCube : D::k2D;
    auto sampler = shader.FindOrAddSamplerBinding(fetch,F::kUseFetchConst,F::kUseFetchConst,
                                                 F::kUseFetchConst,A::kUseFetchConst);
    auto texture = shader.FindOrAddTextureBinding(fetch,dimension,false);
    auto signed_texture = shader.FindOrAddTextureBinding(fetch,dimension,true);
    auto count = shader.GetBindlessResourceCount();
    assert(shader.FindOrAddSamplerBinding(fetch,F::kUseFetchConst,F::kUseFetchConst,
                                          F::kUseFetchConst,A::kUseFetchConst) == sampler);
    assert(shader.FindOrAddTextureBinding(fetch,dimension,false) == texture);
    assert(shader.FindOrAddTextureBinding(fetch,dimension,true) == signed_texture);
    assert(shader.GetBindlessResourceCount() == count);
  }
  assert(shader.GetBindlessResourceCount() == 12);
  std::vector<uint32_t> descriptor_cbuffer(shader.GetBindlessResourceCount());
  try {
    for (const auto& binding : shader.texture_bindings_)
      ++descriptor_cbuffer.at(binding.bindless_descriptor_index);
    for (const auto& binding : shader.sampler_bindings_)
      ++descriptor_cbuffer.at(binding.bindless_descriptor_index);
  } catch (const std::out_of_range&) { return 42; }
  assert(std::all_of(descriptor_cbuffer.begin(),descriptor_cbuffer.end(),[](auto uses){return uses==1;}));
  assert(shader.sampler_bindings_.front().bindless_descriptor_index == 0);
  DxbcShaderTranslator bindful;
  bindful.bindless_resources_used_ = false;
  bindful.FindOrAddTextureBinding(0,D::k1D,false);
  bindful.FindOrAddSamplerBinding(0,F::kLinear,F::kPoint,F::kBaseMap,A::kDisabled);
  assert(bindful.texture_bindings_[0].bindless_descriptor_index == 0);
  assert(bindful.sampler_bindings_[0].bindless_descriptor_index == 0);
  puts("Production DXBC bindings: 12 unique slots fit 12 words; deduplication and bindful indices: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    args = parser.parse_args()
    if not args.cxx:
        parser.error("Pass --cxx pointing to the existing host clang++ compiler")
    source = (ROOT / "sdk/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp").read_text(encoding="utf-8")
    allocators = "\n".join(function(source, f"uint32_t DxbcShaderTranslator::{name}(")
                           for name in ("FindOrAddTextureBinding", "FindOrAddSamplerBinding"))
    output = ROOT / "out/pruebas_indices_texturas"
    output.mkdir(parents=True, exist_ok=True)
    for negative in (False, True):
        name = "control_anterior" if negative else "actual"
        body = allocators
        if negative:
            assert body.count("GetBindlessResourceCount() - 1") == 2
            body = body.replace("GetBindlessResourceCount() - 1", "GetBindlessResourceCount()")
        cpp, exe = output / f"{name}.cpp", output / f"{name}.exe"
        cpp.write_text(PREAMBLE + body + CHECK, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-fuse-ld=lld", str(cpp), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)])
        assert result.returncode == (42 if negative else 0), (name, result.returncode)
    print("Negative control: the previous one-based allocation overflows the descriptor cbuffer, as expected")


if __name__ == "__main__":
    main()
