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
   external images, the sky's cube map, the area light descriptor and the render manifest after it (see "Output
   layout").

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
├── Point_Entities
│   └── ent_0005_info_player_start   TRS of the entity, entity extras
└── Sky_Box                   mesh Sky_Box: the sky box images, with bExportSkyBoxGeometry (see "Sky box")
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

Since ConverterVersion 15, ordinary un-controlled `func_wall` and `func_illusionary` brushes default to package-owned
static props (`extras.goldsrc.renderFlags`, see the extras schema for exclusions). Renderers keep these nodes as
part of the world independently of network PVS and do not create additional runtime instances. Static `func_wall`
and `func_illusionary` nodes have no rotation, as their Spawn clears angles. Controlled brushes and
other classes continue to require runtime entity presentation.

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

The light child's `extras.goldsrc` carries `lightStyle`, the entity's `style` key read with `atoi` as HLSDK's
`CLight::KeyValue` does (0 when absent), and `targetname` when the entity has one (see
`GoldSrcBSPExtrasSchema.md`, "Node extras: light children"). The compilers' `qcsg` gives every light with a
`targetname` a style of its own from 32 on, shared by lights of the same name, and writes it into `style`; styles 1 to
12 are the preset animations the mapper picks. GoldSrc scales the lightmap layer of a style by its lightstyle value,
which the game switches (`a` off, `m` normal) and animates; a renderer scales the light the same way, so switched and
animated lights follow the game, and a light the map starts off (`spawnflags` 1) is dark until it is switched on. A
`style` outside 0 to 255 is reported as `InvalidEntityValue` and written as 0. The exported intensity is the light at
style value `m`; `pattern` and `spawnflags` remain metadata. Converter versions before 14 wrote no light extras.
`bPreservePointEntities = false` omits both entities and their lights. `light_spot` and lights already removed
from the BSP are not converted; `light_environment` becomes a directional light (see below). `KHR_lights_punctual` is
listed in `extensionsUsed` only when lights exist, and is not required. This is a relighting starting point, not a
physical equivalence to HLRAD. The parameter is included in the cache key. The current request layout requires
`UtilGoldSrcAssetConverterAPI_007`; old interface versions are rejected. ConverterVersion is 14 and the extras schema
is 3.

## Environment light

The first preserved point entity, in lump order, whose classname is exactly `light_environment` gains a
`<entity-node-name>_DirectionalLight` child with `KHR_lights_punctual` type `directional`: the sun HLRAD bakes into
the surfaces that see the sky. As for point lights, the entity stays an Empty with its name, transform and extras,
the child has no entity identity, and entity statistics do not count it.

The direction is the sky vector HLSDK's `CEnvLight::Spawn` computes from the entity's angles. Keys apply in file
order: `angles` and `angle` as for every entity (see "Entity transforms"), and `pitch`, which `CLight::KeyValue`
writes into the pitch of the angles for the classes `light`, `light_spot` and `light_environment` only (other classes,
such as `ambient_generic`, use `pitch` for something else). `UTIL_MakeAimVectors` negates the pitch before
`AngleVectors`, so for pitch `p` and yaw `y` the light shines along `(cos p cos y, cos p sin y, sin p)` in GoldSrc
coordinates; `pitch -90` points straight down. The entity node keeps the engine's rotation, which ignores `pitch`;
the child's rotation cancels it and turns glTF's light axis -z into that direction. `angle -1` and `-2` set the pitch
as `ED_ParseEntity` does, -90 and 90, so the sun points down and up respectively; stock maps use `pitch` instead.

`_light` is read as for point lights (`V`, `P = max(V)`, color `V / P`, white with intensity 0 for zero
contributions). The intensity is illuminance, `P * options.environmentLightIntensityScale` lux; unlike point lights it
does not depend on `scaleFactor`, since a directional light has no distance falloff. `environmentLightIntensityScale`
defaults to **0.2**, independently of the point light and emission multipliers, must be finite and nonnegative, and
is part of the cache key. The default was chosen in the path traced de_dust2 and cs_assault, lit by the sky at the
default `skyRadianceScale`, by comparing linear brightness relative to the sky (tone mapping inverted, so the ratios
do not depend on exposure) with the original lightmaps: shaded walls of de_dust2 then match them, sunlit walls are
about 1.5 times brighter, and walls facing away from the sun in cs_assault are darker, since the lightmaps of ZHLT add a
sky diffuse term that the dim sky of that map does not provide. 0 omits the light, keeps the entity and skips parsing its values. Invalid parameters and final
intensities exceeding float range fail with `InvalidRequest` before any output is published.

Maps often repeat `light_environment` with the same values. Only the first valid entity adds a light: a malformed
`_light` is an `InvalidEntityValue` warning and the next entity is tried, and a later entity whose direction, color
or brightness differs from the converted one is reported as `DuplicateEnvironmentLight` and not converted. Repeats
with the same values are not reported. The child's extras carry `lightStyle` and `targetname` as those of point lights
do; the sky's diffuse light (`_diffuse_light` of later compilers) is not converted; the sky box is a separate environment light (see "Sky box"). A renderer tracing shadow rays
must let them leave through the sky faces: the converted `World_Sky` geometry encloses outdoor areas as the BSP does.

## Sky box

The engine loads a sky box only for a map with a texture named `sky*`, whether or not its sky faces are kept. The
images are `gfx/env/<skyname><suffix>` for the suffixes `rt bk lf ft up dn`, `<skyname>` being the worldspawn's
`skyname`, or `desert` (`sv_skyname`'s default) without one. When no face of the named sky loads, `desert` is loaded
instead; a face without image is drawn black. The converter asks the resolver for these images as
`TextureKind::SkyboxFace` requests named `<skyname><suffix>`, RGBA8 images oriented as the files are; a missing face
is reported as `MissingSkyboxTexture`. Without resolver, or for a map without sky texture, the asset's extras only
name the sky.

The images are kept as glTF textures, which the extras name. With at least one image, two options control what the
converter makes of them. `bExportSkyEnvironmentMap` (the default) adds the cube map and the environment light, which
renderers, path tracers and bakers use; `bExportSkyBoxGeometry` (off by default) adds the `Sky_Box` node, which only
viewers that show the glTF as it stands need. Each works without the other.

With `bExportSkyEnvironmentMap`, the converter adds:

- the cube map `<resource>/<resource>.sky.dds`: a DDS file with the DX10 header, one `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`
  cube without mip levels, faces in Direct3D order (`+x -x +y -y +z -z`) in the axes of the converted scene: Y up,
  as `BSP_Root` turns GoldSrc's `(x, y, z)` into `(x, z, -y)`. Sampled with a scene direction `(x, y, z)`, it shows
  what the engine draws in the GoldSrc direction `(x, -z, y)`. Each face is as wide as the largest sky image, texels
  take the nearest image texel, alpha is opaque and directions toward a face without image are black.
- the manifest's environment light, a graph node after the world (see "Render manifest"):

```json
{ "name": "Sky", "type": "EnvironmentLight", "path": "c1a0/c1a0.sky.dds", "radianceScale": [2, 2, 2] }
```

  It is the environment light of a Donut scene, which the RTXPT path tracer draws where rays leave the scene and
  samples as light. `radianceScale` is the radiance of a white texel, `BSPConversionOptions::skyRadianceScale` (default
  2, finite and nonnegative, part of the cache key) on each channel; the texels are sRGB and decode to linear values
  before the scale applies. The default matches the sky on screen in GoldSrc, whose gamma tables show it about 1.5
  times brighter than its linear texels, after the path tracer's tone mapping at exposure 1.

With `bExportSkyBoxGeometry`, the converter adds the `Sky_Box` node, a child of `BSP_Root` beside `BSP_World`, so that
it is not part of the world. Its mesh is a cube centered on the map's origin whose faces lie `sv_zmax × 0.57735` away, 4096 × 0.57735
GoldSrc units for the default `sv_zmax`, as `R_DrawSkyBox` sizes the box around the view. Each face is a quad seen from
inside, its normal toward the center, with an unlit material `Sky_Box_<suffix>` showing the image (black without one)
through a clamped sampler, the texture coordinates half a texel inside the edges as the engine's. Viewers see the sky
from inside the map; a renderer that draws the sky around the view, or a baker that traces the world, leaves the node
out: the box is finite, so rays leaving the map would hit it instead of the environment light.

Both options are part of the cache key. Neither changes the map's own sky faces, which `bPreserveSkyGeometry` keeps in
the models' `Sky` meshes (see "Faces"). Converter versions before 13 had one option, `bExportSkyBox` (on by default),
for both the node and the cube map. The extras (`asset.extras.goldsrc.sky`, see the schema) name the sky, the images
used, the cube map and the box.

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
is 13. The public interface is `UtilGoldSrcAssetConverterAPI_007` because the options/request layout changed;
callers must rebuild against the new header. `extras.goldsrc` is schema version 2.

### Adjacent LTC area light descriptors

Every successful conversion writes `<resource>/<resource>.sh9/area-lights.json` through the output sink, beside the model.
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
external files `textures/<index>_<name>.png` beside the asset without `bEmbedTextures`. The image directory does not
carry the resource name; the asset directory `<resource>/` keeps the images of two maps apart.

Sky materials are a sky-blue placeholder (`isGoldSrcSky: true`); the sky box itself is converted separately (see
"Sky box"). Water materials are
double sided and name their liquid. Animated textures (`+0name`…), random tiling (`-0name`…), alternate sequences
(`+aname`), scrolling textures and detail textures are described in the material's extras; the base color texture is
the frame shown at time 0.

## Output layout

Every path the output sink receives is relative to its root, in this order:

| File | Content |
| --- | --- |
| `<resource>/<resource>.glb` or `.gltf` | the asset |
| `<resource>/textures/<index>_<name>.png` | the external images, without `bEmbedTextures`; the asset's URIs name them relative to it |
| `<resource>/<resource>.sky.dds` | the sky's cube map, with `bExportSkyEnvironmentMap` (see "Sky box") |
| `<resource>/<resource>.sh9/area-lights.json` | the area light descriptor |
| `<resource>.manifest.json` | the render manifest, written last |

`<resource>` is `szResourceName`, or the identifier's file name without extension, with characters other than letters,
digits, `-`, `_` and `.` replaced by `_`. A name of dots only would leave the root and is `InvalidRequest`. Assets
written to one root need distinct resource names. With the root at a game's `maps/`, `maps/c1a0.bsp` gives
`maps/c1a0/c1a0.glb` and `maps/c1a0.manifest.json` beside the map. The sink protocol has no rollback: the manifest comes
last, so that it is only written once the files it names were accepted.

### Render manifest

The manifest (schema `RenderManifestSchemaVersion`, 2) tells a game which glTF models make up the map's rendering, and
how the models are placed. Its paths are relative to the manifest's directory, use `/`, and stay inside it. It is also
a scene file in the Donut format that the RTXPT path tracer reads: `models` and `graph` are what such a scene loader
needs, and it ignores the other keys. Loaded as a scene, the manifest's directory is the assets root. The manifest, its
directory and everything under it form the map's world package: a renderer reads the files from there as it needs
them, the models from the manifest, their buffers and images through the URIs of each glTF file (relative to that
file), and the light probes from `lightProbes`; nothing outside the directory belongs to the package. The converter
writes one model, the world:

```json
{
  "schemaVersion": 2,
  "converterVersion": 14,
  "bspSha256": "<SHA-256 of the BSP bytes, lowercase hex>",
  "unitScale": 0.0254,
  "models": ["c1a0/c1a0.glb"],
  "graph": [{ "name": "World", "model": 0, "role": "world" }],
  "lightProbes": { "type": "SH9", "header": "c1a0/c1a0.sh9/runtime.json", "atlas": "c1a0/c1a0.sh9/atlas_rgba32f.bin" }
}
```

- `bspSha256` is `sourceHash`, the digest of the converted BSP bytes: a reader checks that the manifest belongs to the
  map it loads, also after `asset.extras` was lost in an editor round trip.
- `unitScale` is the conversion's `scaleFactor`, scene units per GoldSrc unit: the scene position of the GoldSrc point
  `(x, y, z)` is `(x, z, -y) × unitScale`, the transform of `BSP_Root` (see "Coordinates"). A game uses it to place its
  camera and entities in the scene.
- `models` names the glTF files.
- `graph` places the models by index (`model`, an unsigned integer). TRS (`translation`, `rotation` as xyzw
  quaternion, `scaling`) are in scene space, the space of the world model: Y up, in scene units. They are omitted for
  the world, and nodes may nest under `children`. `role: "world"` marks the model that stands for the BSP world; other
  nodes are static decorations a reader may add. A node without model whose `type` is `"EnvironmentLight"` is the
  sky's environment light (see "Sky box"), which the converter adds after the world when it writes a cube map.
- `lightProbes` names the SH9 header and atlas of `gltf-bake-lighting`, which do not exist until the map is baked; a
  reader treats them as not baked yet, not as an error, while they are missing.

Converters before version 10 also wrote `files`, a list of the other files for a game to collect, with wildcards. It
is no longer written, and readers ignore it: the files a model needs are the ones its glTF references.

Readers ignore unknown keys. The manifest can be maintained by hand after conversion (for example to add decorations);
a normal convert, edit and bake cycle needs no change to it.

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
        // The asset first, then its external images, the sky's cube map, the area light descriptor and the manifest.
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
                         [--environment-light-intensity-scale <factor>] [--no-sky-environment-map]
                         [--sky-box-geometry] [--sky-radiance-scale <factor>]
```

`--emissive-scale` overrides the default texture emission multiplier of 4; for example,
`GoldSrcAssetConverterCLI c1a0 -basedir <game-directory> --emissive-scale 8 -o <output-directory>`.
Use `--emissive-scale 0` to disable texture emission. Invalid numeric strings are rejected rather than parsed as 0.

`--point-light-intensity-scale` independently adjusts entity point lights (default 1). Use 0 to retain the original
entity nodes without generating lights; it does not change texture emission. For example,
`GoldSrcAssetConverterCLI c1a0 -basedir <game-directory> --point-light-intensity-scale 4 -o <output-directory>`.

`--environment-light-intensity-scale` sets the lux per `_light` brightness unit of the `light_environment` sun
(default 0.2). Use 0 to retain the entity without generating the directional light.

`--no-sky-environment-map` exports neither the sky's cube map nor its environment light, `--sky-box-geometry` also
exports the `Sky_Box` node for glTF viewers, and `--sky-radiance-scale` sets the radiance of a white sky texel in the
manifest's environment light (default 2). The tool reads sky images through the game's file system,
so only with `-basedir`: `gfx/env/<skyname><suffix>.tga`, then `.bmp`.

Without `-basedir`, the map and the WAD files are local paths. With `-basedir`, the tool loads `filesystem_stdio.dll`
(not built by this project; see the README) from its own directory and mounts the search paths the engine's `FileSystem_SetGameDirectory` mounts for an English,
non-low-violence installation: `<game>_addon` (with `-addons`), `<game>_hd` (unless `-nohdmodels`), `<game>`,
`<game>_downloads`, the `fallback_dir` chain of `liblist.gam`, then `valve_hd`, the base directory and `valve`. The map is
a path relative to them, or a name looked up as `maps/<name>.bsp`; the WAD files of the worldspawn's `wad` key are
opened through them as `Mod_LoadEntities` and `TEX_InitFromWad` do (file name only, `.wad` extension, decal WADs
skipped), and a missing one is reported and skipped. Textures are searched in the worldspawn's WAD files, then in the
`--wad` files. The output directory (`-o`, the current directory by default) is the sink's root (see "Output layout"):
`maps/c1a0.bsp` gives `c1a0/c1a0.glb` (`c1a0/c1a0.gltf` with `--gltf`), the images in `c1a0/textures/` with
`--external-textures`, and `c1a0.manifest.json`. Pass a game's `maps/` directory to install the result beside the map.

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
- Sky geometry is kept with a placeholder material. The optional `Sky_Box` node has a fixed size around the map's
  origin, not the view; renderers draw the sky from the cube map instead.
- The tool does not decode `.dds` sky images, which the engine tries before `.tga`; such a sky converts only when a
  TGA or BMP image of the same name exists.
- Water is static geometry; the warp, the wave height and the leaf contents are runtime behavior and only described.
- Brush entities are placed by their key values at load time; movement, render modes and triggers are not simulated.
- Detail textures and sky box images are only converted when the resolver provides them.
