#include "BSPConverterInternal.h"

namespace GoldSrcAssetConverter
{
//GoldSrcRender::BuildBSPWorld skips sky faces (GoldSrcRender::BSPSkippedFaces::sky): the bridge draws the sky box
//instead of them. Their triangles are rebuilt here from the face, edge, surfedge, vertex, plane and texinfo lumps, so
//that the asset keeps every sky surface of every model; the caller checks that the face has no surface and no water
//surface, so nothing is exported twice.
void ExtractBSPSkyFace(const CBSPFileReader& reader, std::uint32_t face, BSPPrimitive& primitive)
{
    const auto  data    = reader.ReadFace(face);
    const auto  texInfo = reader.ReadTexInfo(data.texInfo);
    const auto* pMiptex = reader.FindFaceMiptex(data);
    const auto  normal  = reader.ReadFaceNormal(data);

    const double sizes[GoldSrcRender::BSPTextureAxes] = {static_cast<double>(pMiptex->width), static_cast<double>(pMiptex->height)};

    std::vector<Vector3> polygon;
    reader.ReadFacePolygon(data, polygon);

    const auto firstVertex = static_cast<std::uint32_t>(primitive.positions.size());

    for (const auto& position : polygon)
    {
        primitive.positions.push_back(position);
        primitive.normals.push_back(normal);
        primitive.texcoords.push_back({static_cast<float>(ComputeTextureCoordinate(texInfo, 0, position) / sizes[0]),
                                       static_cast<float>(ComputeTextureCoordinate(texInfo, 1, position) / sizes[1])});
    }

    //BSP faces are convex; a fan keeps their winding, as AppendFaceVertices builds the drawn surfaces.
    for (std::uint32_t i = 1; i + 1 < data.edgeCount; ++i)
    {
        primitive.indices.push_back(firstVertex);
        primitive.indices.push_back(firstVertex + i);
        primitive.indices.push_back(firstVertex + i + 1);
    }
}
} // namespace GoldSrcAssetConverter
