#include "BSPConverterInternal.h"

#include <map>

namespace GoldSrcAssetConverter
{
//GoldSrc's hull sizes (the engine's Mod_LoadSubmodels and the game DLLs' hull table): hull 0 is a point traced through
//the nodes and leaves, hulls 1 to 3 trace their clipnodes.
struct BSPHullSize
{
    Vector3     mins;
    Vector3     maxs;
    const char* szTree;
};

static const BSPHullSize s_HullSizes[BSPMaxHulls] = {
    {{0, 0, 0}, {0, 0, 0}, "nodes"},
    {{-16, -16, -36}, {16, 16, 36}, "clipNodes"},
    {{-32, -32, -32}, {32, 32, 32}, "clipNodes"},
    {{-16, -16, -18}, {16, 16, 18}, "clipNodes"},
};

//A raw lump copied once into the buffer, whichever kind of spatial data references it.
class CSpatialLumpWriter
{
public:
    explicit CSpatialLumpWriter(const BSPConversionSource& source, CGLTFDocumentBuilder& document) :
        m_Source(source), m_Document(document)
    {
    }

    //The lump's description; records of recordBytes, or 0 for byte data. An empty lump has a count of 0 and no
    //buffer view, since glTF buffer views cannot be empty.
    nlohmann::json Describe(std::uint32_t lump, std::uint32_t recordBytes, std::uint64_t& bytes)
    {
        const auto length = m_Source.reader.GetLumpLength(lump);

        nlohmann::json description;
        description["byteLength"] = length;

        if (recordBytes)
        {
            description["recordBytes"] = recordBytes;
            description["count"]       = length / recordBytes;
        }

        if (!length)
            return description;

        auto it = m_Views.find(lump);

        if (it == m_Views.end())
            it = m_Views.emplace(lump, m_Document.AddBufferView(m_Source.reader.GetLumpData(lump), length, 0)).first;

        description["bufferView"] = it->second;
        bytes += length;

        return description;
    }

private:
    const BSPConversionSource&   m_Source;
    CGLTFDocumentBuilder&        m_Document;
    std::map<std::uint32_t, int> m_Views;
};

static nlohmann::json MakeVectorJSON(const Vector3& vector)
{
    return nlohmann::json::array({vector.x, vector.y, vector.z});
}

/*
====================
AppendBSPSpatialData

Visibility keeps what a PVS query needs: the planes and nodes to find a point's leaf from the world's root, the
leaves' contents, visibility offsets and marksurface ranges, the marksurfaces (face indices) and the compressed rows,
with the world's visible leaf count that sizes them. Collision keeps the planes, clipnodes, nodes and leaves, the four
hull sizes and every model's hull roots and bounds. Lumps are stored as the file stores them (little-endian BSP30
records), each once even when both kinds reference it.
====================
*/
nlohmann::json AppendBSPSpatialData(const BSPConversionSource& source, CGLTFDocumentBuilder& document, ConversionStatistics& statistics)
{
    const auto& options = source.pRequest->options;

    if (!options.bEmbedBSPVisibilityData && !options.bEmbedBSPCollisionData)
        return nullptr;

    const auto& reader = source.reader;
    const auto  world  = reader.ReadModel(0);

    CSpatialLumpWriter lumps(source, document);
    nlohmann::json     spatial;

    if (options.bEmbedBSPVisibilityData)
    {
        nlohmann::json visibility;
        visibility["present"]          = true;
        visibility["encoding"]         = "goldsrc-bsp30";
        visibility["formatVersion"]    = 1;
        visibility["rootNode"]         = world.headNodes[0];
        visibility["visibleLeafCount"] = world.visLeaves;

        auto& bytes = statistics.visibilityDataBytes;

        nlohmann::json visibilityLumps;
        visibilityLumps["planes"]       = lumps.Describe(GoldSrcRender::BSPPlaneLump, BSPPlaneBytes, bytes);
        visibilityLumps["nodes"]        = lumps.Describe(GoldSrcRender::BSPNodeLump, BSPNodeBytes, bytes);
        visibilityLumps["leaves"]       = lumps.Describe(GoldSrcRender::BSPLeafLump, BSPLeafBytes, bytes);
        visibilityLumps["markSurfaces"] = lumps.Describe(GoldSrcRender::BSPMarkSurfaceLump, BSPMarkSurfaceBytes, bytes);
        visibilityLumps["visibility"]   = lumps.Describe(GoldSrcRender::BSPVisibilityLump, 0, bytes);

        visibility["lumps"]      = std::move(visibilityLumps);
        visibility["byteLength"] = bytes;
        spatial["visibility"]    = std::move(visibility);
    }

    if (options.bEmbedBSPCollisionData)
    {
        nlohmann::json collision;
        collision["present"]       = true;
        collision["encoding"]      = "goldsrc-bsp30";
        collision["formatVersion"] = 1;

        auto& bytes = statistics.collisionDataBytes;

        nlohmann::json collisionLumps;
        collisionLumps["planes"]    = lumps.Describe(GoldSrcRender::BSPPlaneLump, BSPPlaneBytes, bytes);
        collisionLumps["clipNodes"] = lumps.Describe(BSPClipNodeLump, BSPClipNodeBytes, bytes);
        collisionLumps["nodes"]     = lumps.Describe(GoldSrcRender::BSPNodeLump, BSPNodeBytes, bytes);
        collisionLumps["leaves"]    = lumps.Describe(GoldSrcRender::BSPLeafLump, BSPLeafBytes, bytes);

        nlohmann::json hulls = nlohmann::json::array();

        for (std::uint32_t hull = 0; hull < BSPMaxHulls; ++hull)
            hulls.push_back({{"index", hull}, {"mins", MakeVectorJSON(s_HullSizes[hull].mins)}, {"maxs", MakeVectorJSON(s_HullSizes[hull].maxs)}, {"tree", s_HullSizes[hull].szTree}});

        nlohmann::json models = nlohmann::json::array();

        for (std::uint32_t model = 0; model < reader.GetRecordCount(GoldSrcRender::BSPModelLump, GoldSrcRender::BSPModelBytes); ++model)
        {
            const auto data = reader.ReadModel(model);

            models.push_back({{"index", model}, {"headNodes", data.headNodes}, {"mins", MakeVectorJSON(data.mins)}, {"maxs", MakeVectorJSON(data.maxs)}, {"origin", MakeVectorJSON(data.origin)}});
        }

        collision["lumps"]      = std::move(collisionLumps);
        collision["hulls"]      = std::move(hulls);
        collision["models"]     = std::move(models);
        collision["byteLength"] = bytes;
        spatial["collision"]    = std::move(collision);
    }

    return spatial;
}
} // namespace GoldSrcAssetConverter
