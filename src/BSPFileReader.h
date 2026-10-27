#if !defined(BSP_FILE_READER_H)
#    define BSP_FILE_READER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "GoldSrcRenderBSP.h"
#    include "GoldSrcRenderAssetUtil.h"

#    include <array>
#    include <cstdint>
#    include <string>
#    include <vector>

//Read-only access to the raw records of a BSP30 file that GoldSrcRender::BuildBSPWorld does not keep: the entity text,
//the models' origins and hull roots, the faces' texinfos and polygons, the miptex headers and the raw lumps of the
//optional spatial data. The file must have passed GoldSrcRender::ValidateBSPAsset, which checks every index read here.
namespace GoldSrcAssetConverter
{
using GoldSrcRender::Vector2;
using GoldSrcRender::Vector3;

//Lumps the protocol does not name. GoldSrc's Mod_LoadClipnodes reads the clipnodes of hulls 1 to 3.
constexpr std::uint32_t BSPClipNodeLump = 9;

//Record sizes and field offsets of the BSP30 lumps read here.
constexpr std::uint32_t BSPPlaneBytes       = 20;
constexpr std::uint32_t BSPVertexBytes      = 12;
constexpr std::uint32_t BSPNodeBytes        = 24;
constexpr std::uint32_t BSPTexInfoBytes     = 40;
constexpr std::uint32_t BSPFaceBytes        = 20;
constexpr std::uint32_t BSPLeafBytes        = 28;
constexpr std::uint32_t BSPMarkSurfaceBytes = 2;
constexpr std::uint32_t BSPEdgeBytes        = 4;
constexpr std::uint32_t BSPSurfEdgeBytes    = 4;
constexpr std::uint32_t BSPClipNodeBytes    = 8;
constexpr std::uint32_t BSPMiptexBytes      = 40;
constexpr std::uint32_t BSPMiptexNameBytes  = 16;
constexpr std::uint32_t BSPMiptexMips       = 4;
constexpr std::uint32_t BSPMaxHulls         = 4;
constexpr std::uint32_t BSPAxes             = 3;

//Faces with fewer edges have no area: BuildBSPWorld counts them as degenerate.
constexpr std::uint32_t BSPMinFaceEdges = 3;

//GoldSrc's TEX_SPECIAL texinfo flag, and the luxel size of lightmaps.
constexpr std::int32_t  BSPTextureSpecial = 1;
constexpr std::uint32_t BSPLuxelSize      = 16;

struct BSPRawPlane
{
    Vector3      normal;
    float        dist{};
    std::int32_t type{};
};

struct BSPRawFace
{
    std::uint32_t                                              plane{};
    bool                                                       bBack{};
    std::uint32_t                                              firstEdge{};
    std::uint32_t                                              edgeCount{};
    std::uint32_t                                              texInfo{};
    std::array<std::uint8_t, GoldSrcRender::BSPLightmapStyles> styles{};
    std::int32_t                                               lightOffset{};
};

struct BSPRawTexInfo
{
    float        vectors[GoldSrcRender::BSPTextureAxes][GoldSrcRender::BSPTextureVectorSize]{};
    std::int32_t miptex{};
    std::int32_t flags{};
};

//A miptex entry of the texture lump. name is the name as GoldSrc's Mod_LoadTextures loads it (the third character of
//names containing '~' blanked), rawName the 16 stored bytes up to the first terminator. The record starts recordOffset
//bytes into the file and recordBytes bytes remain in the lump from there; bEmbedded marks records whose first mip is
//stored in the BSP.
struct BSPRawMiptex
{
    bool          bPresent{};
    std::string   name;
    std::string   rawName;
    std::uint32_t width{};
    std::uint32_t height{};
    bool          bEmbedded{};
    std::uint32_t recordOffset{};
    std::uint32_t recordBytes{};
};

struct BSPRawModel
{
    Vector3                               mins;
    Vector3                               maxs;
    Vector3                               origin;
    std::array<std::int32_t, BSPMaxHulls> headNodes{};
    std::int32_t                          visLeaves{};
    std::uint32_t                         firstFace{};
    std::uint32_t                         faceCount{};
};

//Lightmap rectangle of a face in luxels, as GoldSrc's CalcSurfaceExtents computes it.
struct BSPFaceLightmapExtents
{
    std::int32_t  textureMins[GoldSrcRender::BSPTextureAxes]{};
    std::uint32_t luxelWidth{};
    std::uint32_t luxelHeight{};
};

class CBSPFileReader
{
public:
    //The file must stay alive while the reader is used.
    void Open(const std::uint8_t* pFile, std::uint32_t fileBytes, GoldSrcRender::BSPLayout layout);

    const std::uint8_t* GetFile() const
    {
        return m_pFile;
    }

    std::uint32_t GetFileBytes() const
    {
        return m_FileBytes;
    }

    //Lumps by meaning, whatever the file's layout.
    const std::uint8_t* GetLumpData(std::uint32_t lump) const;
    std::uint32_t       GetLumpOffset(std::uint32_t lump) const;
    std::uint32_t       GetLumpLength(std::uint32_t lump) const;
    std::uint32_t       GetRecordCount(std::uint32_t lump, std::uint32_t recordBytes) const;

    BSPRawModel   ReadModel(std::uint32_t index) const;
    BSPRawFace    ReadFace(std::uint32_t index) const;
    BSPRawTexInfo ReadTexInfo(std::uint32_t index) const;
    BSPRawPlane   ReadPlane(std::uint32_t index) const;

    //Corners of the face in its winding.
    void ReadFacePolygon(const BSPRawFace& face, std::vector<Vector3>& polygon) const;

    //Plane normal of the face's vertices, turned around for faces on the back of their plane.
    Vector3 ReadFaceNormal(const BSPRawFace& face) const;

    BSPFaceLightmapExtents ComputeLightmapExtents(const BSPRawFace& face, const BSPRawTexInfo& texInfo) const;

    //Every entry of the texture lump; empty without one.
    const std::vector<BSPRawMiptex>& GetMiptexes() const
    {
        return m_Miptexes;
    }

    //The miptex of a face, or null without texture lump.
    const BSPRawMiptex* FindFaceMiptex(const BSPRawFace& face) const;

    //The entity lump's text up to its first terminator.
    std::string ReadEntityText() const;

private:
    void ReadMiptexes();

    const std::uint8_t*                                    m_pFile{};
    std::uint32_t                                          m_FileBytes{};
    std::array<std::uint32_t, GoldSrcRender::BSPLumpCount> m_Offsets{};
    std::array<std::uint32_t, GoldSrcRender::BSPLumpCount> m_Lengths{};
    std::vector<BSPRawMiptex>                              m_Miptexes;
};

//Texture coordinate (dot product with the axis plus its offset) of position along one texinfo axis, in double
//precision as GoldSrc's CalcSurfaceExtents computes it.
double ComputeTextureCoordinate(const BSPRawTexInfo& texInfo, std::uint32_t axis, const Vector3& position);

//GoldSrc's sky surfaces: Mod_LoadFaces flags faces whose texture name starts with "sky" (with case) SURF_DRAWSKY.
bool IsBSPSkyTextureName(const std::string& name);

//Slots of the miptexes the texture lump holds, read with bounds checks from a file that is not validated yet, so that
//the file is validated with every slot bound as the engine binds it: unbound slots lose their texinfo flags (as
//GoldSrc's r_notexture_mip), which puts TEX_SPECIAL faces under the surface extent limit. Returns false when the
//header or the texture lump table is out of range; GoldSrcRender::ValidateBSPFile then rejects the file.
bool ScanBSPMiptexSlots(const std::uint8_t* pFile, std::uint32_t fileBytes, std::vector<std::uint32_t>& slots);
} // namespace GoldSrcAssetConverter

#endif
