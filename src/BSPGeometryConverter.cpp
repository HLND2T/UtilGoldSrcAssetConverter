#include "BSPConverterInternal.h"

#include <algorithm>
#include <limits>

namespace GoldSrcAssetConverter
{
//Index of the primitive of material in primitives, created on first use.
static BSPPrimitive& FindPrimitive(std::vector<BSPPrimitive>& primitives, std::map<std::uint32_t, std::size_t>& indices, std::uint32_t material)
{
    const auto it = indices.find(material);

    if (it != indices.end())
        return primitives[it->second];

    indices.emplace(material, primitives.size());
    primitives.emplace_back();
    primitives.back().material = material;

    return primitives.back();
}

//Raw miptex index of a face, NoMiptexIndex in a file without texture lump.
static std::uint32_t GetFaceMiptex(const CBSPFileReader& reader, const BSPRawFace& face)
{
    if (reader.GetMiptexes().empty())
        return NoMiptexIndex;

    return static_cast<std::uint32_t>(reader.ReadTexInfo(face.texInfo).miptex);
}

//Copies the vertices that indices first to first + count of the BSPWorld reference, which belong to one face and are
//consecutive, into primitive, and appends the indices of its triangles. Lightmap coordinates are moved into the
//atlas cell of lightmapPage when the primitive is lightmapped.
static void AppendWorldTriangles(const GoldSrcRender::BSPWorld& world, std::uint32_t first, std::uint32_t count, const BSPLightmapAtlasLayout* pAtlas, std::uint32_t lightmapPage, BSPPrimitive& primitive)
{
    if (!count)
        return;

    auto minVertex = std::numeric_limits<std::uint32_t>::max();
    auto maxVertex = std::uint32_t{0};

    for (auto i = first; i < first + count; ++i)
    {
        minVertex = std::min(minVertex, world.indices[i]);
        maxVertex = std::max(maxVertex, world.indices[i]);
    }

    const auto base = static_cast<std::uint32_t>(primitive.positions.size());

    for (auto vertex = minVertex; vertex <= maxVertex; ++vertex)
    {
        primitive.positions.push_back(world.positions[vertex]);
        primitive.normals.push_back(world.normals[vertex]);
        primitive.texcoords.push_back(world.texcoords[vertex]);

        if (pAtlas)
            primitive.lightmapTexcoords.push_back(ToLightmapAtlasTexcoord(*pAtlas, lightmapPage, world.lightmapTexcoords[vertex]));
    }

    for (auto i = first; i < first + count; ++i)
        primitive.indices.push_back(base + world.indices[i] - minVertex);
}

//The surface's group, by the groups' surface ranges.
static std::vector<std::uint32_t> MapSurfaceGroups(const GoldSrcRender::BSPWorld& world)
{
    std::vector<std::uint32_t> groups(world.surfaces.size());

    for (std::uint32_t group = 0; group < world.textureGroups.size(); ++group)
    {
        const auto& textureGroup = world.textureGroups[group];

        for (auto surface = textureGroup.firstSurface; surface < textureGroup.firstSurface + textureGroup.surfaceCount; ++surface)
            groups[surface] = group;
    }

    return groups;
}

static std::vector<std::uint32_t> MapWaterGroups(const GoldSrcRender::BSPWorld& world)
{
    std::vector<std::uint32_t> groups(world.waterSurfaces.size());

    for (std::uint32_t group = 0; group < world.waterGroups.size(); ++group)
    {
        const auto& waterGroup = world.waterGroups[group];

        for (auto surface = waterGroup.firstSurface; surface < waterGroup.firstSurface + waterGroup.surfaceCount; ++surface)
            groups[surface] = group;
    }

    return groups;
}

/*
====================
BuildBSPModelGeometry

Every face of every model ends in exactly one place: a drawn surface of the BSPWorld (lit or unlit), a water surface
(its pieces in their own winding; the reversed fans the bridge draws from behind are left out, materials are double
sided instead), a sky face rebuilt from the file, or the model's list of degenerate faces, which have no area. A face
that is none of these is reported as not exported. Primitives keep the faces in file order, each with its face record.
====================
*/
void BuildBSPModelGeometry(const BSPConversionSource& source, const BSPLightmapAtlasLayout& atlas, CBSPMaterialRegistry& materials, std::vector<BSPModelGeometry>& models)
{
    const auto& world   = source.world;
    const auto& reader  = source.reader;
    const auto& options = source.pRequest->options;

    const auto surfaceGroups = MapSurfaceGroups(world);
    const auto waterGroups   = MapWaterGroups(world);

    const bool bLightmaps = options.bPreserveLightmaps && !world.bFullbright && atlas.pageCount;

    models.clear();
    models.resize(world.submodels.size());

    for (std::uint32_t model = 0; model < world.submodels.size(); ++model)
    {
        const auto& submodel = world.submodels[model];
        auto&       geometry = models[model];
        geometry.model       = model;

        std::array<std::map<std::uint32_t, std::size_t>, BSPGeometryKindCount> primitiveIndices;

        for (std::uint32_t i = 0; i < submodel.faceCount; ++i)
        {
            const auto  face  = submodel.firstFace + i;
            const auto& entry = world.faces[submodel.firstFaceEntry + i];
            const auto  data  = reader.ReadFace(face);

            BSPFaceRecord record;
            record.face        = face;
            record.texInfo     = data.texInfo;
            record.styles      = data.styles;
            record.lightOffset = data.lightOffset;

            BSPMaterialKey key;
            key.miptex = GetFaceMiptex(reader, data);

            BSPPrimitive* pPrimitive = nullptr;

            if (entry.surface != BSPNoSurface)
            {
                const auto& surface = world.surfaces[entry.surface];
                const auto& group   = world.textureGroups[surfaceGroups[entry.surface]];

                key.kind           = BSPGeometryKind::Surface;
                key.binding        = GoldSrcRender::SelectBSPTextureFrame(world, group, false, 0).binding;
                key.animationFirst = group.animation.firstFrame;
                key.animationCount = group.animation.frameCount;
                key.alternateFirst = group.alternateAnimation.firstFrame;
                key.alternateCount = group.alternateAnimation.frameCount;
                key.detailBinding  = group.detailBinding;
                key.scrollWidth    = group.scrollWidth;
                key.bTexSpecial    = group.bUnlit;
                key.bLightmapped   = bLightmaps && !group.bUnlit;

                pPrimitive = &FindPrimitive(geometry.primitives[static_cast<std::uint32_t>(key.kind)], primitiveIndices[static_cast<std::uint32_t>(key.kind)], materials.Register(key));

                record.sourceSurface  = entry.surface;
                record.firstIndex     = static_cast<std::uint32_t>(pPrimitive->indices.size());
                record.textureMins[0] = entry.textureMins[0];
                record.textureMins[1] = entry.textureMins[1];

                if (key.bLightmapped)
                {
                    const auto extents = reader.ComputeLightmapExtents(data, reader.ReadTexInfo(data.texInfo));

                    record.lightmapPage   = surface.lightmapPage;
                    record.lightmapX      = entry.lightmapX;
                    record.lightmapY      = entry.lightmapY;
                    record.lightmapWidth  = extents.luxelWidth;
                    record.lightmapHeight = extents.luxelHeight;
                }

                AppendWorldTriangles(world, surface.firstIndex, surface.indexCount, key.bLightmapped ? &atlas : nullptr, surface.lightmapPage, *pPrimitive);
            }
            else if (entry.waterSurface != BSPNoSurface)
            {
                const auto& water = world.waterSurfaces[entry.waterSurface];
                const auto& group = world.waterGroups[waterGroups[entry.waterSurface]];

                key.kind    = BSPGeometryKind::Water;
                key.binding = group.binding;

                pPrimitive = &FindPrimitive(geometry.primitives[static_cast<std::uint32_t>(key.kind)], primitiveIndices[static_cast<std::uint32_t>(key.kind)], materials.Register(key));

                record.sourceSurface = entry.waterSurface;
                record.firstIndex    = static_cast<std::uint32_t>(pPrimitive->indices.size());

                const auto firstVertex = static_cast<std::uint32_t>(pPrimitive->positions.size());
                AppendWorldTriangles(world, water.firstIndex, water.indexCount, nullptr, 0, *pPrimitive);

                //The BSPWorld keeps the warp's raw coordinates; the asset has the texture coordinates of the texinfo.
                const auto  texInfo = reader.ReadTexInfo(data.texInfo);
                const auto* pMiptex = reader.FindFaceMiptex(data);

                for (auto vertex = firstVertex; vertex < pPrimitive->positions.size(); ++vertex)
                {
                    const auto& position = pPrimitive->positions[vertex];

                    pPrimitive->texcoords[vertex] = {static_cast<float>(ComputeTextureCoordinate(texInfo, 0, position) / pMiptex->width),
                                                     static_cast<float>(ComputeTextureCoordinate(texInfo, 1, position) / pMiptex->height)};
                }
            }
            else if (data.edgeCount < BSPMinFaceEdges)
            {
                geometry.degenerateFaces.push_back(face);
                continue;
            }
            else
            {
                const auto* pMiptex = reader.FindFaceMiptex(data);

                if (!pMiptex || !pMiptex->bPresent || !IsBSPSkyTextureName(pMiptex->name))
                {
                    geometry.unexportedFaces.push_back(face);
                    continue;
                }

                if (!options.bPreserveSkyGeometry)
                {
                    geometry.omittedSkyFaces.push_back(face);
                    continue;
                }

                key.kind = BSPGeometryKind::Sky;

                pPrimitive = &FindPrimitive(geometry.primitives[static_cast<std::uint32_t>(key.kind)], primitiveIndices[static_cast<std::uint32_t>(key.kind)], materials.Register(key));

                record.firstIndex = static_cast<std::uint32_t>(pPrimitive->indices.size());
                ExtractBSPSkyFace(reader, face, *pPrimitive);
            }

            record.indexCount = static_cast<std::uint32_t>(pPrimitive->indices.size()) - record.firstIndex;
            pPrimitive->faces.push_back(record);
        }
    }
}
} // namespace GoldSrcAssetConverter
