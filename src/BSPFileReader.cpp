#include "BSPFileReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace GoldSrcAssetConverter
{
//Field offsets within the records.
constexpr std::uint32_t BSPPlaneDistOffset        = 12;
constexpr std::uint32_t BSPPlaneTypeOffset        = 16;
constexpr std::uint32_t BSPTexInfoMiptexOffset    = 32;
constexpr std::uint32_t BSPTexInfoFlagsOffset     = 36;
constexpr std::uint32_t BSPFaceSideOffset         = 2;
constexpr std::uint32_t BSPFaceFirstEdgeOffset    = 4;
constexpr std::uint32_t BSPFaceEdgeCountOffset    = 8;
constexpr std::uint32_t BSPFaceTexInfoOffset      = 10;
constexpr std::uint32_t BSPFaceStylesOffset       = 12;
constexpr std::uint32_t BSPFaceLightOffset        = 16;
constexpr std::uint32_t BSPModelMinsOffset        = 0;
constexpr std::uint32_t BSPModelMaxsOffset        = 12;
constexpr std::uint32_t BSPModelOriginOffset      = 24;
constexpr std::uint32_t BSPModelHeadNodesOffset   = 36;
constexpr std::uint32_t BSPModelVisLeavesOffset   = 52;
constexpr std::uint32_t BSPMiptexTableOffset      = 4;
constexpr std::uint32_t BSPMiptexOffsetBytes      = 4;
constexpr std::uint32_t BSPMiptexWidthOffset      = 16;
constexpr std::uint32_t BSPMiptexHeightOffset     = 20;
constexpr std::uint32_t BSPMiptexMipOffsetsOffset = 24;
constexpr std::int32_t  BSPAbsentOffset           = -1;

//GoldSrc blanks this character of miptex names containing '~' when it loads them.
constexpr std::size_t BSPTildeBlankedCharacter = 2;

//Initial texture coordinate bounds of GoldSrc's CalcSurfaceExtents; kept because they change the extents of faces
//lying entirely beyond them.
constexpr float BSPExtentsInitialMin = 999999.0f;
constexpr float BSPExtentsInitialMax = -99999.0f;

static std::uint16_t ReadUInt16(const std::uint8_t* pBytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(pBytes[offset] | pBytes[offset + 1] << 8);
}

static std::int16_t ReadInt16(const std::uint8_t* pBytes, std::size_t offset)
{
    return static_cast<std::int16_t>(ReadUInt16(pBytes, offset));
}

static std::int32_t ReadInt32(const std::uint8_t* pBytes, std::size_t offset)
{
    return static_cast<std::int32_t>(GoldSrcRender::ReadUInt32(pBytes, offset));
}

static float ReadFloat(const std::uint8_t* pBytes, std::size_t offset)
{
    const auto bits = GoldSrcRender::ReadUInt32(pBytes, offset);

    float value;
    std::memcpy(&value, &bits, sizeof(value));

    return value;
}

static Vector3 ReadVector(const std::uint8_t* pBytes, std::size_t offset)
{
    return {ReadFloat(pBytes, offset), ReadFloat(pBytes, offset + sizeof(float)), ReadFloat(pBytes, offset + 2 * sizeof(float))};
}

void CBSPFileReader::Open(const std::uint8_t* pFile, std::uint32_t fileBytes, GoldSrcRender::BSPLayout layout)
{
    m_pFile     = pFile;
    m_FileBytes = fileBytes;

    for (std::uint32_t i = 0; i < GoldSrcRender::BSPLumpCount; ++i)
    {
        m_Offsets[i] = GoldSrcRender::ReadBSPLumpOffset(pFile, i);
        m_Lengths[i] = GoldSrcRender::ReadBSPLumpLength(pFile, i);
    }

    //Blue Shift stores the entities and the planes in each other's table entry.
    if (layout == GoldSrcRender::BSPLayout::BlueShift)
    {
        std::swap(m_Offsets[GoldSrcRender::BSPEntityLump], m_Offsets[GoldSrcRender::BSPPlaneLump]);
        std::swap(m_Lengths[GoldSrcRender::BSPEntityLump], m_Lengths[GoldSrcRender::BSPPlaneLump]);
    }

    ReadMiptexes();
}

const std::uint8_t* CBSPFileReader::GetLumpData(std::uint32_t lump) const
{
    return m_pFile + m_Offsets[lump];
}

std::uint32_t CBSPFileReader::GetLumpOffset(std::uint32_t lump) const
{
    return m_Offsets[lump];
}

std::uint32_t CBSPFileReader::GetLumpLength(std::uint32_t lump) const
{
    return m_Lengths[lump];
}

std::uint32_t CBSPFileReader::GetRecordCount(std::uint32_t lump, std::uint32_t recordBytes) const
{
    return m_Lengths[lump] / recordBytes;
}

BSPRawModel CBSPFileReader::ReadModel(std::uint32_t index) const
{
    const auto* pModel = GetLumpData(GoldSrcRender::BSPModelLump) + static_cast<std::size_t>(index) * GoldSrcRender::BSPModelBytes;

    BSPRawModel model;
    model.mins   = ReadVector(pModel, BSPModelMinsOffset);
    model.maxs   = ReadVector(pModel, BSPModelMaxsOffset);
    model.origin = ReadVector(pModel, BSPModelOriginOffset);

    for (std::uint32_t hull = 0; hull < BSPMaxHulls; ++hull)
        model.headNodes[hull] = ReadInt32(pModel, BSPModelHeadNodesOffset + hull * sizeof(std::int32_t));

    model.visLeaves = ReadInt32(pModel, BSPModelVisLeavesOffset);
    model.firstFace = static_cast<std::uint32_t>(ReadInt32(pModel, GoldSrcRender::BSPModelFirstFaceOffset));
    model.faceCount = static_cast<std::uint32_t>(ReadInt32(pModel, GoldSrcRender::BSPModelFaceCountOffset));

    return model;
}

BSPRawFace CBSPFileReader::ReadFace(std::uint32_t index) const
{
    const auto* pFace = GetLumpData(GoldSrcRender::BSPFaceLump) + static_cast<std::size_t>(index) * BSPFaceBytes;

    BSPRawFace face;
    face.plane       = ReadUInt16(pFace, 0);
    face.bBack       = ReadInt16(pFace, BSPFaceSideOffset) != 0;
    face.firstEdge   = GoldSrcRender::ReadUInt32(pFace, BSPFaceFirstEdgeOffset);
    face.edgeCount   = ReadUInt16(pFace, BSPFaceEdgeCountOffset);
    face.texInfo     = ReadUInt16(pFace, BSPFaceTexInfoOffset);
    face.lightOffset = ReadInt32(pFace, BSPFaceLightOffset);
    std::memcpy(face.styles.data(), pFace + BSPFaceStylesOffset, face.styles.size());

    return face;
}

BSPRawTexInfo CBSPFileReader::ReadTexInfo(std::uint32_t index) const
{
    const auto* pTexInfo = GetLumpData(GoldSrcRender::BSPTexInfoLump) + static_cast<std::size_t>(index) * BSPTexInfoBytes;

    BSPRawTexInfo texInfo;

    for (std::uint32_t axis = 0; axis < GoldSrcRender::BSPTextureAxes; ++axis)
    {
        for (std::uint32_t i = 0; i < GoldSrcRender::BSPTextureVectorSize; ++i)
            texInfo.vectors[axis][i] = ReadFloat(pTexInfo, (axis * GoldSrcRender::BSPTextureVectorSize + i) * sizeof(float));
    }

    texInfo.miptex = ReadInt32(pTexInfo, BSPTexInfoMiptexOffset);
    texInfo.flags  = ReadInt32(pTexInfo, BSPTexInfoFlagsOffset);

    return texInfo;
}

BSPRawPlane CBSPFileReader::ReadPlane(std::uint32_t index) const
{
    const auto* pPlane = GetLumpData(GoldSrcRender::BSPPlaneLump) + static_cast<std::size_t>(index) * BSPPlaneBytes;

    BSPRawPlane plane;
    plane.normal = ReadVector(pPlane, 0);
    plane.dist   = ReadFloat(pPlane, BSPPlaneDistOffset);
    plane.type   = ReadInt32(pPlane, BSPPlaneTypeOffset);

    return plane;
}

void CBSPFileReader::ReadFacePolygon(const BSPRawFace& face, std::vector<Vector3>& polygon) const
{
    polygon.resize(face.edgeCount);

    const auto* pSurfEdges = GetLumpData(GoldSrcRender::BSPSurfEdgeLump);
    const auto* pEdges     = GetLumpData(GoldSrcRender::BSPEdgeLump);
    const auto* pVertices  = GetLumpData(GoldSrcRender::BSPVertexLump);

    for (std::uint32_t i = 0; i < face.edgeCount; ++i)
    {
        const auto  surfEdge = ReadInt32(pSurfEdges, static_cast<std::size_t>(face.firstEdge + i) * BSPSurfEdgeBytes);
        const auto* pEdge    = pEdges + static_cast<std::size_t>(surfEdge > 0 ? surfEdge : -surfEdge) * BSPEdgeBytes;
        const auto  vertex   = ReadUInt16(pEdge, surfEdge > 0 ? 0 : sizeof(std::uint16_t));

        polygon[i] = ReadVector(pVertices, static_cast<std::size_t>(vertex) * BSPVertexBytes);
    }
}

Vector3 CBSPFileReader::ReadFaceNormal(const BSPRawFace& face) const
{
    const auto normal = ReadPlane(face.plane).normal;

    return face.bBack ? Vector3{-normal.x, -normal.y, -normal.z} : normal;
}

double ComputeTextureCoordinate(const BSPRawTexInfo& texInfo, std::uint32_t axis, const Vector3& position)
{
    const auto* vector = texInfo.vectors[axis];

    return static_cast<double>(position.x) * vector[0] + static_cast<double>(position.y) * vector[1] + static_cast<double>(position.z) * vector[2] + vector[3];
}

BSPFaceLightmapExtents CBSPFileReader::ComputeLightmapExtents(const BSPRawFace& face, const BSPRawTexInfo& texInfo) const
{
    float mins[GoldSrcRender::BSPTextureAxes] = {BSPExtentsInitialMin, BSPExtentsInitialMin};
    float maxs[GoldSrcRender::BSPTextureAxes] = {BSPExtentsInitialMax, BSPExtentsInitialMax};

    std::vector<Vector3> polygon;
    ReadFacePolygon(face, polygon);

    for (const auto& position : polygon)
    {
        for (std::uint32_t axis = 0; axis < GoldSrcRender::BSPTextureAxes; ++axis)
        {
            //GoldSrc keeps the extremes as floats.
            const auto value = static_cast<float>(ComputeTextureCoordinate(texInfo, axis, position));

            mins[axis] = std::min(mins[axis], value);
            maxs[axis] = std::max(maxs[axis], value);
        }
    }

    BSPFaceLightmapExtents extents;
    std::uint32_t          sizes[GoldSrcRender::BSPTextureAxes]{};

    for (std::uint32_t axis = 0; axis < GoldSrcRender::BSPTextureAxes; ++axis)
    {
        const auto luxelMin = static_cast<std::int32_t>(std::floor(mins[axis] / BSPLuxelSize));
        const auto luxelMax = static_cast<std::int32_t>(std::ceil(maxs[axis] / BSPLuxelSize));

        extents.textureMins[axis] = luxelMin * static_cast<std::int32_t>(BSPLuxelSize);
        sizes[axis]               = static_cast<std::uint32_t>(luxelMax - luxelMin) + 1;
    }

    extents.luxelWidth  = sizes[0];
    extents.luxelHeight = sizes[1];

    return extents;
}

void CBSPFileReader::ReadMiptexes()
{
    m_Miptexes.clear();

    const auto  length = GetLumpLength(GoldSrcRender::BSPTextureLump);
    const auto* pLump  = GetLumpData(GoldSrcRender::BSPTextureLump);

    //The texture lump was validated: a count, an offset per entry, and a whole header at every present offset.
    if (!length)
        return;

    m_Miptexes.resize(static_cast<std::uint32_t>(ReadInt32(pLump, 0)));

    for (std::uint32_t i = 0; i < m_Miptexes.size(); ++i)
    {
        const auto offset = ReadInt32(pLump, BSPMiptexTableOffset + i * BSPMiptexOffsetBytes);

        if (offset == BSPAbsentOffset)
            continue;

        auto& miptex = m_Miptexes[i];

        char szName[BSPMiptexNameBytes + 1]{};
        std::memcpy(szName, pLump + offset, BSPMiptexNameBytes);

        miptex.bPresent     = true;
        miptex.rawName      = szName;
        miptex.width        = GoldSrcRender::ReadUInt32(pLump, offset + BSPMiptexWidthOffset);
        miptex.height       = GoldSrcRender::ReadUInt32(pLump, offset + BSPMiptexHeightOffset);
        miptex.bEmbedded    = GoldSrcRender::ReadUInt32(pLump, offset + BSPMiptexMipOffsetsOffset) != 0;
        miptex.recordOffset = GetLumpOffset(GoldSrcRender::BSPTextureLump) + static_cast<std::uint32_t>(offset);
        miptex.recordBytes  = length - static_cast<std::uint32_t>(offset);

        if (std::strchr(szName, '~'))
            szName[BSPTildeBlankedCharacter] = ' ';

        miptex.name = szName;
    }
}

const BSPRawMiptex* CBSPFileReader::FindFaceMiptex(const BSPRawFace& face) const
{
    if (m_Miptexes.empty())
        return nullptr;

    const auto texInfo = ReadTexInfo(face.texInfo);

    return &m_Miptexes[static_cast<std::uint32_t>(texInfo.miptex)];
}

std::string CBSPFileReader::ReadEntityText() const
{
    const auto* pText  = reinterpret_cast<const char*>(GetLumpData(GoldSrcRender::BSPEntityLump));
    const auto  length = GetLumpLength(GoldSrcRender::BSPEntityLump);
    const auto* pEnd   = std::find(pText, pText + length, '\0');

    return std::string(pText, pEnd);
}

bool ScanBSPMiptexSlots(const std::uint8_t* pFile, std::uint32_t fileBytes, std::vector<std::uint32_t>& slots)
{
    slots.clear();

    if (!pFile || fileBytes < GoldSrcRender::BSPHeaderBytes)
        return false;

    //The texture lump has the same table entry in every BSPLayout.
    const std::uint64_t offset = GoldSrcRender::ReadBSPLumpOffset(pFile, GoldSrcRender::BSPTextureLump);
    const std::uint64_t length = GoldSrcRender::ReadBSPLumpLength(pFile, GoldSrcRender::BSPTextureLump);

    if (offset + length > fileBytes)
        return false;

    if (!length)
        return true;

    const auto* pLump = pFile + offset;

    if (length < BSPMiptexTableOffset)
        return false;

    const auto count = ReadInt32(pLump, 0);

    if (count < 0 || static_cast<std::uint64_t>(count) > GoldSrcRender::MaxTableEntries || BSPMiptexTableOffset + static_cast<std::uint64_t>(count) * BSPMiptexOffsetBytes > length)
        return false;

    for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(count); ++slot)
    {
        const auto miptexOffset = ReadInt32(pLump, BSPMiptexTableOffset + slot * BSPMiptexOffsetBytes);

        if (miptexOffset == BSPAbsentOffset)
            continue;

        if (miptexOffset < 0 || static_cast<std::uint64_t>(miptexOffset) + BSPMiptexBytes > length)
            return false;

        slots.push_back(slot);
    }

    return true;
}

bool IsBSPSkyTextureName(const std::string& name)
{
    constexpr char s_SkyPrefix[] = "sky";

    return name.compare(0, sizeof(s_SkyPrefix) - 1, s_SkyPrefix) == 0;
}
} // namespace GoldSrcAssetConverter
