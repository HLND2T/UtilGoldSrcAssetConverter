# BSP conversion

`IUtilGoldSrcAssetConverter::ConvertBSP` converts a GoldSrc BSP30 map (`BSPLayout::GoldSrc`, or Blue Shift's
`BSPLayout::BlueShift` with the entity and plane lumps swapped) into one glTF 2.0 scene asset, `.glb` or `.gltf`, on the
CPU. It needs no render device and no running engine. The output loads in standard glTF viewers and in the DiligentTools
`GLTF::Model` loader; the GoldSrc information standard glTF cannot express is kept in `extras.goldsrc`, described in
[GoldSrcBSPExtrasSchema.md](GoldSrcBSPExtrasSchema.md).

## Pipeline

1. **Bindings and validation.** The options and request are checked (`InvalidRequest`). The texture lump's table is
   scanned with bounds checks for the miptex slots it holds, and every present slot is bound (the caller's bindings
   first; a binding of a slot without miptex, or of a slot twice, is `InvalidRequest`), as the engine names every
   texture it loads. The whole file then goes through `GoldSrcRender::ValidateBSPAsset` with these bindings (lump
   table, record sizes, every index and offset, face extents, lightmap ranges, world tree and visibility rows); a file
   that fails is rejected with `InvalidBSP`. Validating with every slot bound matters: an unbound slot loses its
   texinfo flags (GoldSrc's `r_notexture_mip`), which would put `TEX_SPECIAL` faces such as the `aaatrigger` brushes of
   stock maps under the 512-unit surface extent limit.
2. **World build.** `GoldSrcRender::BuildBSPWorld`, the same code the renderer uses, with the same bindings: every face
   keeps its texture name and size and every turbulent face becomes water. More than `BSPMaxLightmapPages` lightmap
   pages fail with `LimitExceeded`.
3. **Entities.** The entity lump is tokenized as `COM_Parse` reads it (`InvalidEntityLump` on a syntax error). Every
   entity keeps its index in the lump and its key values in file order, duplicates included. The worldspawn is found
   by class name, not by position; a missing or repeated worldspawn is a warning.
4. **Geometry.** Every face of every model (see below), materials, lightmap atlases, meshes and the scene nodes.
5. **Output.** tinygltf writes the document; the asset goes to the request's output sink, which is required, with the
   external images after it.

The same input, texture images and options always give the same bytes: there are no timestamps, maps are written in
key order, and `BSPConversionSummary::szCacheKey` names the input, images, converter version, schema version and
options.

## Code reused from the renderer

[`GoldSrcRenderCore`](https://github.com/HLND2T/GoldSrcRenderCore) (the parent project's target, or the pinned
commit fetched by CMake), shared with the renderer, is linked as is:

- `ValidateBSPFile` / `ValidateBSPAsset` for structural validation;
- `BuildBSPWorld` for drawn surfaces (vertex positions, normals, texture coordinates, lightmap coordinates, triangle
  indices), the texture groups (animation sequences, alternate sequences, scroll, detail textures, `TEX_SPECIAL`),
  water surfaces and their groups, lightmap pages of the four lightstyle layers, and the face → surface map;
- `SelectBSPTextureFrame` for the frame a group shows at time 0;
- `FindBSPLeaf` / `FindBSPVisibleSurfaces` in the tests, to check the exported visibility data.

The protocol gained one field for the converter, `BSPWorldFace::waterSurface`, the water surface of a face entry
(written by `BuildBSPWater`), so that the converter maps water back to its faces without guessing.

What `BuildBSPWorld` does not keep is read from the file by `CBSPFileReader`: raw faces, texinfos, planes and models,
the face polygons of sky faces, lightmap extents (mirroring the engine's `CalcSurfaceExtents`), the miptex headers and
embedded pixels, the entity text and the raw lumps of the optional spatial data.

## Coordinates

GoldSrc is right-handed with z up; glTF is right-handed with y up. Vertices and entity transforms keep GoldSrc
coordinates and units. The single root node `BSP_Root` turns them into glTF's with a rotation of -90 degrees about x,
`(x, y, z) → (x, z, -y)`, and scales by `BSPConversionOptions::scaleFactor`. This root transform preserves handedness
and face winding. Readers that want GoldSrc coordinates take the root's children as they are.

Before writing meshes, the converter aligns each triangle's winding with its BSP face normal (counterclockwise from
the front, as glTF requires), swapping two indices when necessary. Copying the renderer's winding unchanged can hide
floors and ceilings from inside rooms. This applies to ordinary surfaces, sky and water; vertex attributes and face
index ranges are preserved.

## Scene graph

```text
BSP_Root                      rotation -90° about x, scale = scaleFactor
├── BSP_World                 worldspawn entity extras
│   ├── World_Geometry        mesh Model_0_Geometry
│   ├── World_Sky             mesh Model_0_Sky
│   └── World_Water           mesh Model_0_Water
├── Brush_Entities
│   └── ent_0012_func_door    TRS of the entity, entity extras
│       ├── Model_3_Geometry  mesh of model 3 (shared)
│       └── Model_3_Water
└── Point_Entities
    └── ent_0005_info_player_start   TRS of the entity, entity extras
```

- Every entity of the lump has exactly one node (unless `bPreserveBrushEntities` / `bPreservePointEntities` leave its
  kind out): the worldspawn is `BSP_World`; an entity whose `model` is an inline model `*N` (`1 ≤ N < model count`) is
  a brush entity under `Brush_Entities`; every other entity, including external models (`.mdl`, `.spr`) and invalid
  inline references, is a point entity under `Point_Entities`. Node names are `ent_<4-digit index>_<classname>`; the
  stable identity is `extras.goldsrc.entityIndex`, never the name.
- Each model has up to three meshes, one per geometry kind (`Geometry`, `Sky`, `Water`), created once. Every brush
  entity showing the model gets child nodes referencing the same meshes; meshes are never duplicated.
- A submodel no entity shows keeps its meshes in the document, outside the scene, and is reported as
  `UnreferencedSubmodel`. Its entry in `extras.goldsrc.models` lists its meshes and an empty `entities`.

## Entity transforms

The node of a brush or point entity has the translation of its `origin` and the rotation the engine's
`R_GetEntityTransform` applies, `T(origin) · Rz(yaw) · Ry(pitch) · Rx(roll)` with angles in degrees:

- `angles` is read as `pitch yaw roll`. `angle` is converted as `ED_ParseEntity` does: `angle ≥ 0` sets the yaw and
  keeps pitch and roll, `-1` gives `(-90, 0, 0)` (up), any other negative value `(90, 0, 0)` (down). The keys apply in
  file order, as the game DLL receives them.
- Classes whose angles are a movement direction rather than an orientation get no rotation (their angles are kept in
  extras with `angleUsage: "moveDirection"`): `func_door`, `func_water`, `momentary_door`, `func_button`,
  `func_conveyor`, `func_plat`, `func_platrot` and every `trigger_*` (HLSDK `SetMovedir`, `InitTrigger`,
  `CFuncPlat::Setup` zero `pev->angles`).
- `dmodel_t.origin` is not applied: compilers write it for origin brushes, but the engine places brush models by the
  entity's `origin` key. It is kept in the models table.
- Invalid numbers in `origin`/`angles` are read as the engine reads them (`atof`-like, missing components 0) and
  reported as `InvalidEntityValue`.

## Entity point lights

Every preserved point entity whose classname is exactly `light` gains a `<entity-node-name>_PointLight` child with
`KHR_lights_punctual` type `point`. The original entity remains an Empty with its name, transform and original
`extras.goldsrc` metadata. The light child has an identity transform, inherits the entity's position, and does not
duplicate its entity identity. Entity statistics count the original entities, not these generated children.

`_light` accepts exactly 1, 3 or 4 whitespace-separated finite nonnegative numbers, using the last matching key:

| Input | Linear contribution vector `V` |
| --- | --- |
| `brightness` | `(brightness, brightness, brightness)` |
| `R G B` | `(R, G, B)` |
| `R G B brightness` | `(R, G, B) * brightness / 255` |
| Missing `_light` | `(300, 300, 300)` |

For `P = max(V)`, the exported color is `V / P` and intensity is
`P * options.pointLightIntensityScale * options.scaleFactor²` candela. Zero contributions export white with
intensity 0. HDR color values are retained through intensity, without gamma conversion. The squared scale factor
explicitly compensates illumination for scene scaling: the glTF extension does not scale intensity with node
transforms. No `range` is written, so lights use inverse-square falloff without a distance cutoff.

`pointLightIntensityScale` defaults to 1, independently of texture `emissiveScale`. It must be finite and
nonnegative. Set it to 0 to omit generated lights while keeping original entities; this also skips `_light`
parsing. Invalid parameters or final intensities exceeding float range fail with `InvalidRequest` before any output
is published. Malformed entity values instead produce an `InvalidEntityValue` warning and omit only that light.

Lights are constant: `style`, `pattern` and `spawnflags` remain metadata and do not animate or turn off the exported
light. `bPreservePointEntities = false` omits both entities and their lights. `light_spot`, `light_environment` and
lights already removed from the BSP are not converted. `KHR_lights_punctual` is listed in `extensionsUsed` only
when lights exist, and is not required. This is a relighting starting point, not a physical equivalence to HLRAD.
The parameter is included in the cache key. The current request layout requires `UtilGoldSrcAssetConverterAPI_004`;
old interface versions are rejected. ConverterVersion is 7 and the extras schema remains 1.

## Faces

Every face entry of every model ends in exactly one place, so nothing is lost or duplicated:

| Face | Where it goes |
| --- | --- |
| Drawn by `BuildBSPWorld` | the model's `Geometry` mesh, vertices copied from the world arrays |
| Turbulent (`!`/`*` texture) | the model's `Water` mesh, the forward fans only; materials are double sided instead of the bridge's reversed fans |
| Sky (miptex name starts with `sky`) | the model's `Sky` mesh, rebuilt as a fan from the face's edges, the plane normal and the texinfo texture coordinates |
| Fewer than 3 edges | the model's `degenerateFaces` list (no area) |
| Sky faces with `bPreserveSkyGeometry` off | the model's `omittedSkyFaces` list |
| Anything else | the model's `unexportedFaces` list, with an `UnexportedFace` warning |

`BuildBSPWorld` skips sky faces because the renderer draws the sky box in their place. The converter rebuilds them
inside the model that lists them: sky faces of submodels stay with that submodel and move with its entity.

Primitives group faces of one material and keep them in file order. Each primitive has `POSITION`, `NORMAL` (unit
length; a zero normal, which a valid file does not have, is replaced by +z and reported as `ZeroLengthNormal`), `TEXCOORD_0` (texture coordinates divided by
the miptex size) and, when lightmapped, `TEXCOORD_1` (lightmap atlas coordinates), with 16-bit indices when they fit.
The primitive's `extras.goldsrc.faces` maps each face to its index range and lightmap data.

## Materials and textures

Ordinary surfaces use glTF metallic-roughness PBR (metallic 0, roughness 1), which Blender imports as Principled BSDF
for relighting. Alpha-tested textures retain `alphaMode: MASK` and cutoff 0.5. Sky placeholders alone retain
`KHR_materials_unlit`. Every material has `extras.goldsrc` with the original GoldSrc surface semantics; `lightmapped`
and `texSpecial` describe the source BSP, not the exported PBR material's response to scene lights.

The converter embeds the Half-Life texture light definitions from Sven Co-op SDK's
`mapping/compilers/hl_lights.rad` ("lights.rad for hl.wad", by Hezus). Ordinary surfaces whose **stored** miptex name
matches an entry, ignoring case, reuse their base texture and `TEXCOORD_0` as `emissiveTexture`. Stored names retain
`~`, before the engine blanks the third character. RGB is divided by 255 for `emissiveFactor`, and
`KHR_materials_emissive_strength.emissiveStrength` is `intensity / 255 * options.emissiveScale`. The multiplier
defaults to **4** (reference intensity 2000 gives about 31.373), after testing the c1a0 interior in Cycles with AgX
and exposure 0. These are linear multipliers of the sRGB-decoded texture. They remain PBR surfaces and can illuminate nearby
geometry in path tracers such as Cycles. Unlisted and unresolved textures have no emission; sky and water are not
texture lights. This table needs no SDK installation and does not read external `.rad` files or map-specific overrides.

`BSPConversionOptions::emissiveScale` is a finite nonnegative float. Set it to 1 for the initial normalized RAD
intensity, or 0 to omit texture emission entirely while retaining the PBR material and alpha clipping. Negative,
nonfinite and overflowing values fail with `InvalidRequest`; final emission strengths must fit in a float. The
multiplier is included in the cache key and changes neither base colors nor baked lightmap data. ConverterVersion
is 7. The public interface is `UtilGoldSrcAssetConverterAPI_004` because the options/request layout changed;
callers must rebuild against the new header. `extras.goldsrc` remains schema version 1.

### Adjacent LTC area light descriptors

Every successful conversion writes `<resource>.sh9/area-lights.json` through the output sink, beside the model.
No area light nodes or extensions are added to glTF. `bExportAreaLights` defaults to true; disabling it writes an
empty descriptor, replacing stale data. `maxAreaLightsPerGroup` defaults to 16 and accepts 1–64; both options
participate in the cache key. CLI equivalents are `--no-area-lights` and `--area-lights-per-group <count>`.
Existing SH files in that directory are neither deleted nor regenerated. A rejected sidecar returns `OutputFailed`;
the existing sink protocol does not provide rollback of previously accepted files.

The extractor consumes the final material emission parameters, decoded miptex RGBA, corrected triangles, and
exported scene transforms. Only ordinary resolved emissive surfaces participate; trigger, sky, water and zero-area
triangles are omitted. Same-material coplanar triangles join across positive-length shared edges, including T
junctions (plane tolerance 0.01 GoldSrc units, normal tolerance 0.1 degree). Unreferenced meshes are excluded;
shared brush models are fitted once and instantiated per exported entity at its initial pose.

Each group uses the convex hull's minimum-area bounding rectangle, a grid with 128 cells on the long side and
4×4 samples per cell. Samples reconstruct UV barycentrically, repeat-wrap, bilinearly filter linearized sRGB,
apply alpha MASK, and multiply the final emissive factor and strength. Greedy row/column splits minimize RGB
squared error until relative RMS error is at most 5% or the rectangle budget is reached. Zero borders are trimmed;
adjacent rectangles only merge if their union is rectangular and remains within the error target.
`radianceRGB` is the sampled RGB area integral divided by rectangle area: fitting preserves sampled energy,
including dark margins, but sub-grid details may be missed. High-error groups remain in the output and are listed
in `statistics.limitedGroups`. More than 100,000 instantiated rectangles fails with `LimitExceeded`.

Descriptor schema v1:

- Header: `schemaVersion`, `converterVersion`, `modelFile`, exact model-byte `modelSha256`, `cacheKey`,
  `coordinateSystem: "GLTF_Y_UP"`, original `unitScale`, and `enabled`.
- `lights`: stable `id`, world `position`, XYZW `quaternion`, `width`, `height`, linear HDR `radianceRGB`,
  and provenance `entityIndex`, `modelIndex`, `materialIndex`, `faceIndices`, `group`.
- `statistics`: unique model-space `emissiveFaceCount`, `groupCount`, instantiated `rectangleCount`,
  and `limitedGroups` entries containing group/model IDs and achieved `relativeError`.

Positions and dimensions include the root rotation/scale and entity transform exactly once. Local −Z emits light;
local X/Y span the rectangle. Geometry scaling does not change surface radiance. This descriptor contains no LTC
lookup table or Three.js-specific photometric conversion. The viewer maps the largest RGB component to intensity
and the normalized RGB to linear color, without a 4π or 683 multiplier.

`tools/LightProbeViewer` accepts these files with or without SH. It validates the model hash, uses official LTC
rectangular lights, and selects a bounded pool (32 by default) from all exported rectangles. Area lights have no
shadows: selection is not occlusion, so light can pass through walls. Keep original material emission in Cycles;
do not add these proxy lights to the bake scene. The existing SH capture excludes directly visible emission and
contains reflected light. Changing material emission requires re-baking SH, even when the new sidecar hash matches.

The intensity conversion is a documented starting point for relighting, not a physical equivalence to HLRAD. Tune
emission, scene scale and exposure in Blender. The original lightmap is not connected to these materials.

Textures are resolved per miptex slot, in this order:

1. the caller's `ITextureResolver` (RGBA8 pixels, or a whole miptex record such as `FindWAD3Texture` returns), asked
   by the name the BSP stores, which the engine looks up in WADs: GoldSrc blanks the third character of names
   containing `~` only after loading (`+0~light2a` is found as such and named `+0 light2a`), so materials carry the
   blanked `originalTextureName` and the stored `storedTextureName`;
2. the pixels embedded in the BSP's texture lump;
3. none: a magenta placeholder material, `missingTexture: true` and a `MissingTexture` warning. Geometry, UVs and the
   texture name and size are kept.

Miptexes are decoded as the engine does: the palette applies, and palette index 255 of `{` textures is transparent
`(0, 0, 0, 0)`, giving `alphaMode: MASK` with cutoff 0.5. Images are PNG, embedded in the buffer by default or written as
external files `textures/<index>_<name>.png` beside the asset without `bEmbedTextures`. The directory does not carry the
resource name, so every asset needs a directory of its own: the image indices of two maps collide.

Sky materials are a sky-blue placeholder (`isGoldSrcSky: true`); the sky box itself comes from the worldspawn
`skyname`, recorded in the asset's extras, with the six face images when the resolver provides them. Water materials are
double sided and name their liquid. Animated textures (`+0name`…), random tiling (`-0name`…), alternate sequences
(`+aname`), scrolling textures and detail textures are described in the material's extras; the base color texture is
the frame shown at time 0.

## Lightmaps and lightstyles

`BuildBSPWorld` packs lightmaps into 128×128 pages, one set of pages per lightstyle layer. The converter lays the pages
out as a square grid (`atlasColumns` × `atlasRows`) and writes one RGB PNG per used layer (`lightmap_layer0` …
`lightmap_layer3`). The texels are the BSP's samples, before lightstyle scaling and overbright; `TEXCOORD_1` addresses
the same atlas cell in every layer. A surface's four style numbers (`style0`–`style3`, 255 for none) are in its face
record, so the runtime can combine `Σ layer[k] × lightstyle(styleK)` as the engine does.

Standard glTF has no slot for a lightmap: the atlases are referenced from `extras.goldsrc.lightmaps.layers` and
`TEXCOORD_1` is unused by the materials. glTF-Validator reports them as unused objects (information, not warnings).

Maps without lighting (`bFullbright`) or with `bPreserveLightmaps` off have no lightmaps and no `TEXCOORD_1`;
`bPreserveLightstyles` off keeps layer 0 only.

## Optional spatial data

`bEmbedBSPVisibilityData` and `bEmbedBSPCollisionData` are off by default. When both are off, the document has no
buffer view, accessor or extras entry for them. When on, the raw lumps are stored as the file stores them (little-endian
BSP30 records) in buffer views and described in `extras.goldsrc.spatialData`:

- visibility: planes, nodes, leaves, marksurfaces and the compressed PVS, with the root node and visible leaf count;
- collision: planes, clipnodes, nodes and leaves, the four hull sizes and each model's hull roots and bounds.

A lump used by both is stored once. These are GoldSrc structures in GoldSrc coordinates; glTF viewers ignore them.

## Diagnostics and errors

`ConvertBSP` never throws. A failure sets the summary's `error` and `szErrorMessage` (cut to `ErrorMessageCapacity`
bytes at a UTF-8 character boundary): `InvalidRequest`, `InvalidBSP`, `InvalidEntityLump`, `LimitExceeded`,
`OutputFailed`, `OutOfMemory`, `InternalError`. Everything a successful conversion could not keep exactly is a
diagnostic given to the request's `IDiagnosticSink` as found, each naming its source object; `warningCount` counts
them.

## Usage

The DLL publishes `IUtilGoldSrcAssetConverter` through `CreateInterface`. Nothing the converter allocates outlives a
call: the caller implements the sinks and the resolver, and everything they receive is borrowed for the callback.

```cpp
#include "IUtilGoldSrcAssetConverter.h"

using namespace GoldSrcAssetConverter;

class CWADTextureResolver final : public ITextureResolver
{
public:
    bool ResolveTexture(const TextureRequest& texture, ResolvedTexture* pResolved) noexcept override
    {
        return texture.kind == TextureKind::Miptex && m_pConverter->FindWAD3Texture(m_Wad.data(), m_Wad.size(), texture.szName, pResolved);
    }
    // m_pConverter, m_Wad
};

class CFileOutputSink final : public IConversionOutputSink
{
public:
    bool WriteFile(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes) noexcept override
    {
        // The asset first, then its external images.
    }
};

auto* hModule    = Sys_LoadModule("UtilGoldSrcAssetConverter.dll");
auto  factory    = hModule ? Sys_GetFactory(hModule) : nullptr;
auto* pConverter = factory ? static_cast<IUtilGoldSrcAssetConverter*>(factory(UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION, nullptr)) : nullptr;

BSPConversionRequestView request;
request.asset.szIdentifier = "c1a0.bsp";
request.asset.pFile        = bsp.data();
request.asset.fileBytes    = static_cast<std::uint32_t>(bsp.size());
request.options.format     = OutputFormat::GLB;
request.pTextureResolver   = &resolver;
request.pOutputSink        = &outputSink;

const auto summary = pConverter->ConvertBSP(request);
```

`GoldSrcAssetConverterCLI` (`tools/GoldSrcAssetConverterCLI.cpp`) is a complete command line tool. It loads
`UtilGoldSrcAssetConverter.dll` from its own directory:

```text
GoldSrcAssetConverterCLI <map> [-basedir <directory> [-game <mod>] [-addons] [-nohdmodels]] [-o <directory>]
                         [--wad <file.wad>]... [--wad-dir <directory>] [--gltf] [--blueshift] [--external-textures]
                         [--visibility] [--collision] [--scale <factor>]
                         [--emissive-scale <factor>] [--point-light-intensity-scale <factor>]
```

`--emissive-scale` overrides the default texture emission multiplier of 4; for example,
`GoldSrcAssetConverterCLI c1a0 -basedir <game-directory> --emissive-scale 8 -o <output-directory>`.
Use `--emissive-scale 0` to disable texture emission. Invalid numeric strings are rejected rather than parsed as 0.

`--point-light-intensity-scale` independently adjusts entity point lights (default 1). Use 0 to retain the original
entity nodes without generating lights; it does not change texture emission. For example,
`GoldSrcAssetConverterCLI c1a0 -basedir <game-directory> --point-light-intensity-scale 4 -o <output-directory>`.

Without `-basedir`, the map and the WAD files are local paths. With `-basedir`, the tool loads `filesystem_stdio.dll`
(not built by this project; see the README) from its own directory and mounts the search paths the engine's `FileSystem_SetGameDirectory` mounts for an English,
non-low-violence installation: `<game>_addon` (with `-addons`), `<game>_hd` (unless `-nohdmodels`), `<game>`,
`<game>_downloads`, the `fallback_dir` chain of `liblist.gam`, then `valve_hd`, the base directory and `valve`. The map is
a path relative to them, or a name looked up as `maps/<name>.bsp`; the WAD files of the worldspawn's `wad` key are
opened through them as `Mod_LoadEntities` and `TEX_InitFromWad` do (file name only, `.wad` extension, decal WADs
skipped), and a missing one is reported and skipped. Textures are searched in the worldspawn's WAD files, then in the
`--wad` files. Each map is written into a directory named after it in the output directory (`-o`, the current directory
by default): `maps/c1a0.bsp` gives `c1a0/c1a0.glb` (`c1a0/c1a0.gltf` with `--gltf`), and the images in `c1a0/textures/`
with `--external-textures`.

## Building and testing

The project builds standalone: CMake fetches GoldSrcRenderCore, tinygltf, stb and nlohmann/json at pinned versions
unless their source trees are given, and the Valve SDK interface files are in `thirdparty/ValveSDK` (see the README). A
parent build may instead add it with `add_subdirectory` and pass its own dependency locations and GoldSrcRenderCore
target; HLND2T-DiligentGraphics does so and installs the DLL and the tool next to the game executable. `UTILGOLDSRCASSETCONVERTER_BUILD_CLI=OFF` leaves the tool out. The converter itself is
the static `UtilGoldSrcAssetConverterCore`, linked into the DLL and the BSP tests; `src/GoldSrcAssetConverterCore.h` is
internal to this project. Standalone, it also builds the tests:

```bash
cmake -S . -B build -DUTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

- `UtilGoldSrcAssetConverter.BSP` converts a generated test BSP in every configuration and checks the documents with
  an independent GLB reader and tinygltf.
- `UtilGoldSrcAssetConverter.DLL` loads the built DLL, resolves the interface through `CreateInterface` and converts
  through caller-implemented sinks only.
- `UtilGoldSrcAssetConverter.GLTFValidator` (`UTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS=ON`, needs node and npm) runs
  the Khronos glTF-Validator on every output and fails on any error.
- `UtilGoldSrcAssetConverter.DiligentLoader` (when `DILIGENT_INCLUDE_DIRS`, `DILIGENT_LIBRARY_DIRS` and
  `DILIGENT_LIBRARIES` name the prebuilt DiligentEngine) loads every output with `GLTF::Model` without a device.

## Limitations

- Only BSP30 maps; `.mdl` and `.spr` assets are not converted (their entities are point entities naming the model).
- Lightmaps are kept as metadata and are not applied to the PBR materials; scene lights or texture emission provide lighting.
- Sky geometry is kept with a placeholder material; viewers do not draw the sky box.
- Water is static geometry; the warp, the wave height and the leaf contents are runtime behavior and only described.
- Brush entities are placed by their key values at load time; movement, render modes and triggers are not simulated.
- Detail textures and sky box images are only converted when the resolver provides them.
