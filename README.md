# UtilGoldSrcAssetConverter

Converts GoldSrc assets into glTF 2.0 scene assets on the CPU, without a render device or a running engine. This
stage converts BSP30 maps (GoldSrc and Blue Shift) into `.glb` or `.gltf` documents that standard glTF tools and the
DiligentTools `GLTF::Model` loader read, keeping the GoldSrc data glTF cannot express in a versioned `extras.goldsrc`
schema.

- [docs/BSPConversion.md](docs/BSPConversion.md): pipeline, scene graph, coordinates, faces, materials, lightmaps,
  optional spatial data, building, testing and limitations.
- [docs/GoldSrcBSPExtrasSchema.md](docs/GoldSrcBSPExtrasSchema.md): every `extras.goldsrc` field.

The converter is a DLL (`UtilGoldSrcAssetConverter.dll`, C++17) with one public header,
`include/Interface/IUtilGoldSrcAssetConverter.h`. Like `UtilAssetsIntegrity`, it publishes `IUtilGoldSrcAssetConverter`
only through `EXPOSE_SINGLE_INTERFACE`: callers load the module and resolve `UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION`
through `CreateInterface`, never linking its import library. The interface is ABI stable across modules and build
configurations: requests and summaries are plain data, and the converted files, diagnostics and texture images go
through `IConversionOutputSink`, `IDiagnosticSink` and `ITextureResolver`, which the caller implements. It reuses the
BSP validation and world build of [GoldSrcRenderCore](https://github.com/HLND2T/GoldSrcRenderCore) (standard library
only) and writes documents with tinygltf.

```cpp
#include "IUtilGoldSrcAssetConverter.h"

auto* hModule    = Sys_LoadModule("UtilGoldSrcAssetConverter.dll");
auto  factory    = Sys_GetFactory(hModule);
auto* pConverter = static_cast<IUtilGoldSrcAssetConverter*>(factory(UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION, nullptr));

GoldSrcAssetConverter::BSPConversionRequestView request;
request.asset.szIdentifier = "c1a0.bsp";
request.asset.pFile        = bsp.data();
request.asset.fileBytes    = static_cast<std::uint32_t>(bsp.size());
request.pOutputSink        = &outputSink; // receives c1a0.glb

const auto summary = pConverter->ConvertBSP(request);
// summary.error, summary.szErrorMessage, summary.warningCount, summary.szCacheKey
```

`GoldSrcAssetConverterCLI` (`tools/GoldSrcAssetConverterCLI.cpp`) loads the DLL from its own directory and resolves
textures from WAD files, given on the command line or, with `-basedir`, found through the game's search paths by
`filesystem_stdio.dll` as the engine does:

```text
GoldSrcAssetConverterCLI maps/c1a0.bsp -o out --wad-dir valve
GoldSrcAssetConverterCLI c1a0 -basedir "D:/Half-Life" -o out
GoldSrcAssetConverterCLI de_dust2 -basedir "D:/Half-Life" -game cstrike -o out
```

Each map goes into a directory named after it, with its render manifest beside it: the first command writes
`out/c1a0/c1a0.glb` and `out/c1a0.manifest.json`, and with `--gltf --external-textures` `out/c1a0/c1a0.gltf` and the
images in `out/c1a0/textures/` (see "Output layout" in `docs/BSPConversion.md`).

## Building

Requires CMake 3.20, Git and MSVC (Windows x86 or x64; continuous integration builds both). The first configuration
downloads the dependencies (see below) unless their source trees are given:

```bash
git clone https://github.com/HLND2T/UtilGoldSrcAssetConverter.git
cd UtilGoldSrcAssetConverter
cmake -S . -B build -DUTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix install
```

`UTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS` runs the Khronos glTF-Validator and needs node and npm; leave it out
otherwise. The tests build their BSP files in code and need no game data. The DiligentTools loader test is built only
when `DILIGENT_INCLUDE_DIRS`, `DILIGENT_LIBRARY_DIRS` and `DILIGENT_LIBRARIES` name a prebuilt DiligentEngine.

| Dependency | Source tree (cache or environment variable) | Fetched when the variable is empty |
| --- | --- | --- |
| [GoldSrcRenderCore](https://github.com/HLND2T/GoldSrcRenderCore) | `GSRENDERCORE_SOURCE_PATH` | `UTILGOLDSRCASSETCONVERTER_CORE_COMMIT` |
| [tinygltf](https://github.com/syoyo/tinygltf) | `TINYGLTF_SOURCE_PATH` (directory of `tiny_gltf.h`) | `UTILGOLDSRCASSETCONVERTER_TINYGLTF_COMMIT` |
| [stb](https://github.com/DiligentGraphics/stb) | `STB_SOURCE_PATH` | `UTILGOLDSRCASSETCONVERTER_STB_COMMIT` |
| [nlohmann/json](https://github.com/nlohmann/json) | `NLOHMANN_JSON_SOURCE_PATH`, or the include directories in `NLOHMANN_JSON_INCLUDE_DIRS` | release v3.12.0 (`json.tar.xz`, SHA-256 checked) |
| Valve SDK `interface.h`, `interface.cpp`, `FileSystem.h` | `UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR` | not fetched: `thirdparty/ValveSDK` in this repository |

The pinned versions are set in `CMakeLists.txt`. tinygltf and stb are pinned to the versions DiligentTools ships, so
the converter writes the same bytes in every build. A parent project that already defines the `GoldSrcRenderCore`
target keeps it, and nothing is fetched when every source tree is given.

### `-basedir` and `filesystem_stdio.dll`

Converting local files (`--wad`, `--wad-dir`) needs only the DLL and the tool. `-basedir` loads `filesystem_stdio.dll`
from the tool's directory, and this project does not build it: use the `filesystem_stdio.dll` of an
HLND2T-DiligentGraphics installation (the tool is installed next to it there). `thirdparty/ValveSDK/FileSystem.h` is
that build's version of the interface: a `filesystem_stdio.dll` built from another version of `FileSystem.h` (for
example the original Half-Life SDK, whose `Read` / `Write` take `int` sizes) does not match on x64.

### Formatting

```bash
python -m pip install clang-format==23.1.3
cmake -S . -B build/format -DFORMAT_VALIDATION_ONLY=ON
cmake --build build/format --target format-check   # or --target format to apply
```

## License

MIT for the code of this project, see [LICENSE](LICENSE). `thirdparty/ValveSDK` is Valve SDK code under its original
terms, and the texture light table comes from the Sven Co-op SDK; [NOTICE](NOTICE) lists these and the licenses of the
fetched dependencies. Half-Life and GoldSrc are trademarks of Valve Corporation; this project is not affiliated with or endorsed
by Valve.
