# Repository guidance

UtilGoldSrcAssetConverter converts GoldSrc BSP maps into glTF 2.0 / GLB on the CPU. Read `README.md` for the build,
the dependencies and the interface, then `docs/BSPConversion.md` (pipeline, scene graph, materials, lightmaps, tests)
and `docs/GoldSrcBSPExtrasSchema.md` (`extras.goldsrc`) instead of reading the sources first.

## Layout

- `include/Interface/IUtilGoldSrcAssetConverter.h`: the only public header. `src/`: the converter (the static
  `UtilGoldSrcAssetConverterCore`) and the DLL entry. `tools/`: `GoldSrcAssetConverterCLI`. `tests/`: BSP, DLL,
  glTF-Validator and DiligentTools loader tests.
- `thirdparty/ValveSDK`: the Valve SDK interface files, under their original terms. Keep them identical to the
  HLND2T-DiligentGraphics copies.
- No submodules. GoldSrcRenderCore, tinygltf, stb and nlohmann/json come from source trees given in
  `GSRENDERCORE_SOURCE_PATH`, `TINYGLTF_SOURCE_PATH`, `STB_SOURCE_PATH` and `NLOHMANN_JSON_SOURCE_PATH` (or a parent
  project's `GoldSrcRenderCore` target and `NLOHMANN_JSON_INCLUDE_DIRS`), else CMake fetches the versions pinned in
  `CMakeLists.txt`. Core changes are merged in its own repository first, then `UTILGOLDSRCASSETCONVERTER_CORE_COMMIT`
  is updated here.

## Compatibility

- Keep the DLL, CLI and CMake target names, `UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION` and the interface layout
  unless an interface change is requested; a layout change needs a new interface version.
- The interface is ABI stable across modules and build configurations: plain data only, views borrowed for the call,
  `noexcept` methods, `std::bad_alloc` turned into error codes.
- A change of the converted output raises `ConverterVersion` (`src/GoldSrcAssetConverterCore.h`), which invalidates
  cache keys; a change of `extras.goldsrc` raises `ExtrasSchemaVersion` and updates the schema document.
- tinygltf and stb stay pinned to the DiligentTools versions; updating them changes output bytes.

## Code style

- Follow the surrounding code: `p` prefix for pointers, `b` for bools, `sz` for C strings, `m_` for members, `s_` for
  file statics; file-local helpers are `static`, not in an anonymous namespace. No magic numbers, tests included.
- Comments that name GoldSrc engine functions describe the behavior being reproduced; never copy engine code.
- Tests generate their BSP files in code. Do not commit real maps, WADs or other game data.
- Format with clang-format 23.1.3 through the `format` / `format-check` targets. Line endings are normalized before
  formatting; do not pass CRLF files to clang-format directly.

## Verification

Build and run CTest (`UtilGoldSrcAssetConverter.BSP`, `.DLL`, and `.GLTFValidator` with
`UTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS=ON`) on x64, and x86 when the change can differ between them; run
`format-check`. For output changes, also convert real maps locally and compare. Do not commit, push or publish without
authorization.
