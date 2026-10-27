# `extras.goldsrc` schema

Version 1 (`BSPConversionSummary::extrasSchemaVersion`). Every object the converter annotates carries its GoldSrc data
in `extras.goldsrc`, with a `schemaVersion` field. Readers must check `schemaVersion` and ignore fields they do not
know; fields are only added within a version, never removed or reinterpreted. Changing a field's meaning raises the
version, and with it the cache key (see `BSPConversionSummary::szCacheKey`).

General rules:

- Vectors are `[x, y, z]` arrays in GoldSrc coordinates and units (z up), before the root node's rotation and scale.
- Indices into glTF arrays (`textures`, `meshes`, `bufferViews`) are integers; `-1` means none.
- BSP indices (faces, texinfos, miptex slots, models, entities) are the file's own indices.
- Strings are UTF-8. GoldSrc text is bytes: text that is not valid UTF-8 is decoded as Latin-1 (each byte one code
  point) and marked where the schema says so.
- Standard glTF fields are authoritative for rendering in generic viewers; `extras.goldsrc` adds what glTF cannot
  express and never contradicts it.

## `asset.extras.goldsrc`

| Field | Type | Meaning |
| --- | --- | --- |
| `schemaVersion` | integer | `1` |
| `converterVersion` | integer | `ConverterVersion` of the converter that wrote the file |
| `assetType` | string | `"BSP"` |
| `sourceFormat` | string | `"BSP30"` or `"BSP30_BlueShift"` (entity and plane lumps swapped) |
| `coordinateSystem` | string | `"GoldSrc_Z_UP"`: the coordinates below the root node |
| `unitScale` | number | the root node's uniform scale, glTF units per GoldSrc unit (`scaleFactor`, default 1) |
| `resourceName` | string | base name of the output files |
| `sourcePath` | string | the request's `sourcePath`, when given; metadata only |
| `faceRecordFields` | string[] | column names of the face records of primitives (see below) |
| `worldspawnEntity` | integer | entity index of the worldspawn, `-1` when the lump has none |
| `entityCount` | integer | number of entities in the lump; entity indices are `0 … entityCount-1` |
| `models` | object[] | one entry per BSP model, by model index (see below) |
| `texInfos` | `[miptex, flags][]` | every texinfo of the file: its miptex slot and its flags (`1` is `TEX_SPECIAL`) |
| `lightmaps` | object | lightmap atlases (see below) |
| `sky` | object | present when the worldspawn has a `skyname` key (see below) |
| `spatialData` | object | present only when visibility or collision data is embedded (see below) |
| `sourceHash` | string | SHA-256 (lowercase hex) of the BSP file |
| `textureHash` | string | SHA-256 of every resolved image, in resolution order |

### `models[]`

| Field | Type | Meaning |
| --- | --- | --- |
| `index` | integer | model index; 0 is the world |
| `mins`, `maxs` | vector | the model's bounds from `dmodel_t` |
| `origin` | vector | `dmodel_t.origin`, written by compilers for origin brushes; not applied (entities are placed by their `origin` key, as the engine does) |
| `firstFace`, `faceCount` | integer | the model's face range |
| `entities` | integer[] | indices of the entities whose node shows the model; empty for an unreferenced submodel |
| `meshes` | object | `{"surface": m, "sky": m, "water": m}`, glTF mesh indices or `-1`; meshes of an unreferenced submodel are outside the scene |
| `degenerateFaces` | integer[] | faces with fewer than 3 edges (no area); omitted when empty |
| `unexportedFaces` | integer[] | faces that are neither drawn, water nor sky; omitted when empty |
| `omittedSkyFaces` | integer[] | sky faces left out by `bPreserveSkyGeometry = false`; omitted when empty |

Every face of the model's range is in exactly one of: a face record of one of its meshes, `degenerateFaces`,
`unexportedFaces`, `omittedSkyFaces`.

### `lightmaps`

| Field | Type | Meaning |
| --- | --- | --- |
| `present` | boolean | whether lightmaps are exported; when false only `fullbright` follows |
| `fullbright` | boolean | the file has no lighting at all: GoldSrc draws every surface fully bright |
| `texCoord` | integer | `1`: lightmapped primitives store atlas coordinates in `TEXCOORD_1` |
| `encoding` | string | `"rgb8"`: the BSP's 8-bit samples, before lightstyle scaling, gamma and overbright |
| `pageSize` | integer | `128`: luxels per side of a `BuildBSPWorld` lightmap page |
| `pageCount` | integer | number of pages |
| `atlasColumns`, `atlasRows` | integer | grid of pages in the atlas; page `p` is at column `p % atlasColumns`, row `p / atlasColumns` |
| `atlasSize` | `[width, height]` | atlas size in luxels |
| `layers` | `{layer, texture}[]` | one RGB atlas per exported lightstyle layer (0–3), as a glTF texture index; all layers share the layout and `TEXCOORD_1` |
| `luxelSize` | integer | `16`: texture-space units per luxel |

A face's lightmap is `Σ layers[k] × lightstyle(style_k)` over the face's styles (face record `style0`–`style3`, `255`
for none), as the engine combines them. Without `bPreserveLightstyles` only layer 0 is exported and `style1`–`style3`
are written as 255. The lightmap textures are referenced only from here: standard glTF materials do not use them.

### `sky`

| Field | Type | Meaning |
| --- | --- | --- |
| `skyname` | string | the worldspawn `skyname` |
| `faceOrder` | string[] | `["rt", "bk", "lf", "ft", "up", "dn"]`: the image name suffixes, in the engine's order |
| `faceTextures` | integer[6] | glTF texture of each face in `faceOrder`, `-1` when unresolved; only when a resolver is given |

The sky geometry itself is in the models' `sky` meshes.

### `spatialData`

Present only with `bEmbedBSPVisibilityData` or `bEmbedBSPCollisionData`. Lumps are stored as the BSP stores them
(little-endian BSP30 records) in buffer views; a lump used by both kinds is stored once.

A lump description is `{"byteLength": n, "recordBytes": r, "count": n / r, "bufferView": v}`; `recordBytes` and `count`
are absent for byte data (the visibility rows), and `bufferView` is absent for an empty lump (glTF buffer views cannot
be empty).

`visibility`:

| Field | Type | Meaning |
| --- | --- | --- |
| `present` | boolean | `true` |
| `encoding` | string | `"goldsrc-bsp30"` |
| `formatVersion` | integer | `1` |
| `rootNode` | integer | the world's hull 0 head node |
| `visibleLeafCount` | integer | `dmodel_t.visleafs` of the world: leaves 1 … n have PVS rows of `(n + 7) / 8` bytes decompressed |
| `lumps` | object | `planes` (20 bytes), `nodes` (24), `leaves` (28), `markSurfaces` (2), `visibility` (bytes) |
| `byteLength` | integer | bytes of the lumps referenced |

To query: walk `nodes` from `rootNode` with `planes` to the point's leaf; its `visofs` points into `visibility`
(GoldSrc run-length coding: a zero byte is followed by the count of zero bytes); set bits name visible leaves, whose
`firstmarksurface`/`nummarksurfaces` select face indices in `markSurfaces`.

`collision`:

| Field | Type | Meaning |
| --- | --- | --- |
| `present` | boolean | `true` |
| `encoding` | string | `"goldsrc-bsp30"` |
| `formatVersion` | integer | `1` |
| `lumps` | object | `planes` (20 bytes), `clipNodes` (8), `nodes` (24), `leaves` (28) |
| `hulls` | object[4] | `{index, mins, maxs, tree}`: hull 0 (point, `tree: "nodes"`), 1 (standing, ±16 ±16 ±36), 2 (large, ±32), 3 (crouching, ±16 ±16 ±18), hulls 1–3 using `tree: "clipNodes"` |
| `models` | object[] | `{index, headNodes[4], mins, maxs, origin}` per model: the root of each hull's tree |
| `byteLength` | integer | bytes of the lumps referenced |

Hull 0 descends `nodes` to `leaves` (their `contents`); hulls 1–3 descend `clipNodes`, whose negative children are
contents. A model is collided in its entity's space, as the engine transforms the trace.

## Node extras: entities

The worldspawn's node (`BSP_World`) and the node of every brush and point entity carry:

| Field | Type | Meaning |
| --- | --- | --- |
| `schemaVersion` | integer | `1` |
| `entityIndex` | integer | the entity's index in the lump: its stable identity (node names are not) |
| `classname` | string | the `classname` value (last occurrence), `""` when absent |
| `originalModelString` | string | the `model` value, when present |
| `modelKind` | string | `"inline"` (`*N`, valid model), `"external"` (a file such as `.mdl`/`.spr`), `"invalid"` (`*N` out of range or malformed), `"none"` |
| `bspModelIndex` | integer | the inline model shown (0 for the worldspawn), else `-1` |
| `originalOrigin` | vector | the `origin` key as parsed, when present |
| `originalAngles` | vector | `pitch yaw roll` from `angles`/`angle` as the engine parses them, when present |
| `angleUsage` | string | `"rotation"`: the node's rotation applies the angles; `"moveDirection"`: the class uses them as a movement direction (doors, buttons, plats, conveyors, water, triggers) and the node has no rotation |
| `render` | object | `rendermode`, `renderamt`, `renderfx` (integers, as `atoi`) and `rendercolor` (`[r, g, b]`), those present; omitted when none |
| `keyvalues` | array | every pair in file order, duplicates and unknown keys included: `[key, value]`, or `[key, value, "latin1"]` when the pair was not UTF-8; omitted without `bPreserveEntityKeyValues` |

Without worldspawn, `BSP_World` carries `{schemaVersion, entityIndex: -1, bspModelIndex: 0}`.

The node's TRS is the entity transform in GoldSrc coordinates: translation = origin, rotation = the engine's
`R_GetEntityTransform` rotation (yaw about z, pitch about y, roll about x) for `angleUsage: "rotation"`. Brush entity
nodes show their model through child nodes named `Model_<n>_<Geometry|Sky|Water>` that reference the model's shared
meshes; point entity nodes have no mesh.

## Primitive extras

| Field | Type | Meaning |
| --- | --- | --- |
| `schemaVersion` | integer | `1` |
| `bspModelIndex` | integer | the model the primitive belongs to |
| `geometryKind` | string | `"surface"`, `"sky"` or `"water"` |
| `faceCount` | integer | number of faces in the primitive |
| `faces` | array[] | one record per face in file order, columns named by the asset's `faceRecordFields`; omitted without `bPreserveSurfaceMetadata` |

Face record columns:

| Column | Meaning |
| --- | --- |
| `face` | BSP face index |
| `firstIndex`, `indexCount` | the face's triangles in the primitive's index accessor |
| `texInfo` | the face's texinfo |
| `sourceSurface` | `BSPWorld` surface (surface kind) or water surface (water kind) index; `-1` for sky |
| `lightmapPage`, `lightmapX`, `lightmapY` | page and luxel position of the face's lightmap block (0 when unlit) |
| `lightmapWidth`, `lightmapHeight` | the block's size in luxels (`extents / 16 + 1`; 0 when unlit) |
| `style0` … `style3` | the face's lightstyles, `255` for none |
| `lightOffset` | the face's byte offset in the lighting lump, `-1` for none |
| `textureMinS`, `textureMinT` | `texturemins` of `CalcSurfaceExtents`: the lightmap's origin in texture space |

## Material extras

| Field | Type | Meaning |
| --- | --- | --- |
| `schemaVersion` | integer | `1` |
| `miptexIndex` | integer | the miptex slot of the faces, `-1` without texture lump |
| `originalTextureName` | string | the miptex name as the engine reads it (with the `~` rule applied) |
| `storedTextureName` | string | the name as stored in the file, when it differs |
| `textureSize` | `[width, height]` | the miptex header's size: the texture coordinate scale |
| `embedded` | boolean | the BSP holds the miptex pixels |
| `surfaceType` | string | `"regular"`, `"sky"` or `"water"` |
| `isGoldSrcSky` | boolean | sky material: the runtime draws the sky box there (the material is a placeholder colour) |
| `placeholder` | boolean | the base colour is a placeholder, not the texture (sky) |
| `liquidType` | string | water: `"water"`, `"slime"` or `"lava"` by the compilers' name convention (`!lava`, `*slime`, …) |
| `turbulent` | boolean | water: GoldSrc warps the texture |
| `lightmapped` | boolean | the faces have a lightmap (`TEXCOORD_1`) |
| `alphaTest` | boolean | `{` texture: palette index 255 is transparent (`alphaMode: MASK`) |
| `missingTexture` | boolean | no image was resolved: the base colour is a magenta placeholder |
| `texSpecial` | boolean | surfaces: the texinfo has `TEX_SPECIAL` (drawn unlit and tiled) |
| `scroll` | object | `{width, tiled: true}`: the surface scrolls with its entity (`scroll*` textures and `TEX_SPECIAL` faces), wrapping at `width` texels |
| `displayedMiptexIndex` | integer | the slot of the frame shown at time 0 when it differs from `miptexIndex` (animations) |
| `animation` | object | animated textures (see below) |
| `detail` | object | `{scale: [s, t], assetId, texture}`: the detail texture of the slot, `texture` a glTF texture or `-1` |

`animation`:

| Field | Type | Meaning |
| --- | --- | --- |
| `framesPerSecond` | number | the engine's texture animation rate |
| `randomTiling` | boolean | `-` prefix: each surface starts at a random frame |
| `frames`, `frameTextures` | integer[] | the `+0`…`+9` sequence as miptex slots and glTF textures (`-1` unresolved) |
| `alternateFrames`, `alternateFrameTextures` | integer[] | the `+a`…`+j` sequence an entity's `frame` toggles to |

## Reading the asset at runtime

- Locate entities by `extras.goldsrc.entityIndex`, not by node name or order.
- Place nodes in GoldSrc space by ignoring `BSP_Root`'s rotation and scale, or work in glTF space with them.
- Render lightmaps from `asset.extras.goldsrc.lightmaps.layers` and `TEXCOORD_1`, weighting by the face records'
  styles; render the sky box from `sky` where `isGoldSrcSky` materials are; animate water and textures from the
  material extras.
- Use `spatialData` only when present; otherwise read PVS and collision from the original BSP.
