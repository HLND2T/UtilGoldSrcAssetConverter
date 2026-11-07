#if !defined(BSP_CONVERTER_INTERNAL_H)
#    define BSP_CONVERTER_INTERNAL_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "GoldSrcAssetConverterCore.h"
#    include "BSPEntityLumpParser.h"
#    include "BSPFileReader.h"
#    include "GLTFDocumentBuilder.h"
#    include "SHA256.h"

#    include <nlohmann/json.hpp>

#    include <array>
#    include <cstdint>
#    include <map>
#    include <string>
#    include <tuple>
#    include <vector>

//Stages of ConvertBSP shared between its source files. Every stage reads the validated BSP through CBSPFileReader and
//the GoldSrcRender::BSPWorld built from it; nothing here outlives a conversion.
namespace GoldSrcAssetConverter
{
using GoldSrcRender::BSPNoSurface;

//Counts the diagnostics of a conversion and forwards each to the request's sink, which may be null.
class CConversionDiagnostics
{
public:
    explicit CConversionDiagnostics(IDiagnosticSink* pSink) :
        m_pSink(pSink)
    {
    }

    void Report(DiagnosticSeverity severity, DiagnosticCode code, const std::string& message);

    std::uint32_t GetCount() const
    {
        return m_Count;
    }

private:
    IDiagnosticSink* m_pSink{};
    std::uint32_t    m_Count{};
};

//The kind of geometry a primitive holds. Each kind of each model is a glTF mesh of its own.
enum class BSPGeometryKind : std::uint32_t
{
    Surface,
    Sky,
    Water
};

constexpr std::uint32_t BSPGeometryKindCount = 3;

const char* GetGeometryKindName(BSPGeometryKind kind);

//Binding index of faces drawn with the missing texture (see GoldSrcRender::BSPMissingTextureBinding).
constexpr std::uint32_t MissingBinding = GoldSrcRender::BSPMissingTextureBinding;

//What makes faces share a material: their kind, their miptex (NoMiptexIndex without texture lump entry), the binding
//they show at scene time 0, their animation sequences (see GoldSrcRender::BSPWorldTextureGroup::animation) and
//whether they have a lightmap, which drawn faces with TEX_SPECIAL (bTexSpecial) never have.
struct BSPMaterialKey
{
    BSPGeometryKind kind{BSPGeometryKind::Surface};
    std::uint32_t   miptex{NoMiptexIndex};
    std::uint32_t   binding{MissingBinding};
    std::uint32_t   animationFirst{};
    std::uint32_t   animationCount{};
    std::uint32_t   alternateFirst{};
    std::uint32_t   alternateCount{};
    std::uint32_t   detailBinding{MissingBinding};
    std::uint32_t   scrollWidth{};
    bool            bTexSpecial{};
    bool            bLightmapped{};

    bool operator<(const BSPMaterialKey& other) const
    {
        return std::tie(kind, miptex, binding, animationFirst, animationCount, alternateFirst, alternateCount, detailBinding, scrollWidth, bTexSpecial, bLightmapped) <
            std::tie(other.kind, other.miptex, other.binding, other.animationFirst, other.animationCount, other.alternateFirst, other.alternateCount, other.detailBinding, other.scrollWidth, other.bTexSpecial, other.bLightmapped);
    }
};

//Faces of one model sharing a kind and a material, and the face records of the primitive (see
//docs/GoldSrcBSPExtrasSchema.md, "Face records").
struct BSPFaceRecord
{
    std::uint32_t                                              face{};
    std::uint32_t                                              firstIndex{};
    std::uint32_t                                              indexCount{};
    std::uint32_t                                              texInfo{};
    std::uint32_t                                              sourceSurface{BSPNoSurface};
    std::uint32_t                                              lightmapPage{};
    std::uint32_t                                              lightmapX{};
    std::uint32_t                                              lightmapY{};
    std::uint32_t                                              lightmapWidth{};
    std::uint32_t                                              lightmapHeight{};
    std::array<std::uint8_t, GoldSrcRender::BSPLightmapStyles> styles{};
    std::int32_t                                               lightOffset{};
    std::int32_t                                               textureMins[GoldSrcRender::BSPTextureAxes]{};
};

struct BSPPrimitive
{
    std::uint32_t              material{};
    std::vector<Vector3>       positions;
    std::vector<Vector3>       normals;
    std::vector<Vector2>       texcoords;
    std::vector<Vector2>       lightmapTexcoords;
    std::vector<std::uint32_t> indices;
    std::vector<BSPFaceRecord> faces;
};

struct BSPModelGeometry
{
    std::uint32_t                                               model{};
    std::array<std::vector<BSPPrimitive>, BSPGeometryKindCount> primitives;
    std::vector<std::uint32_t>                                  degenerateFaces;
    std::vector<std::uint32_t>                                  unexportedFaces;

    //Sky faces left out because BSPConversionOptions::bPreserveSkyGeometry is off.
    std::vector<std::uint32_t> omittedSkyFaces;

    //glTF mesh of each kind, -1 for kinds without primitives.
    std::array<int, BSPGeometryKindCount> meshes{-1, -1, -1};
};

//The source data every stage reads.
struct BSPConversionSource
{
    const BSPConversionRequestView*            pRequest{};
    CBSPFileReader                             reader;
    GoldSrcRender::BSPWorld                    world;
    std::vector<GoldSrcRender::TextureBinding> bindings;
    std::vector<std::uint32_t>                 bindingSlots;
    std::vector<BSPEntity>                     entities;
    std::vector<BSPEntityInfo>                 entityInfos;
};

//extras.goldsrc.renderFlags: a package-owned instance, independent of runtime entity presentation.
constexpr std::uint32_t BSPRenderFlagStaticProp = 0x1;

//Flags by entity, with shared submodels kept dynamic when any reference is not static.
std::vector<std::uint32_t> MakeEntityRenderFlags(const BSPConversionSource& source, CConversionDiagnostics& diagnostics);

//Materials in order of first use.
class CBSPMaterialRegistry
{
public:
    std::uint32_t Register(const BSPMaterialKey& key);

    const std::vector<BSPMaterialKey>& GetKeys() const
    {
        return m_Keys;
    }

private:
    std::map<BSPMaterialKey, std::uint32_t> m_Indices;
    std::vector<BSPMaterialKey>             m_Keys;
};

//Lightmap pages placed in a grid: page p is cell (p % columns, p / columns) of an atlas of columns x rows pages.
struct BSPLightmapAtlasLayout
{
    std::uint32_t pageCount{};
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::uint32_t width{};
    std::uint32_t height{};
};

BSPLightmapAtlasLayout MakeLightmapAtlasLayout(std::uint32_t pageCount);

//Lightmap coordinates of the atlas for coordinates of a page.
Vector2 ToLightmapAtlasTexcoord(const BSPLightmapAtlasLayout& layout, std::uint32_t page, const Vector2& texcoord);

//The RGB texels of lightstyle layer layer of every page, placed in the atlas.
void BuildLightmapAtlas(const GoldSrcRender::BSPWorld& world, std::uint32_t layer, const BSPLightmapAtlasLayout& layout, std::vector<std::uint8_t>& texels);

//Builds the geometry of every model (BSPGeometryConverter.cpp): drawn surfaces and water from the BSPWorld, sky faces
//from the file (see ExtractBSPSkyFace), each face in exactly one primitive of its model.
void BuildBSPModelGeometry(const BSPConversionSource& source, const BSPLightmapAtlasLayout& atlas, CBSPMaterialRegistry& materials, std::vector<BSPModelGeometry>& models);

//Appends the triangles of a sky face, which BuildBSPWorld leaves out, to primitive (BSPSkyGeometryExtractor.cpp): its
//polygon as a fan in the file's winding, with the plane normal and the texture coordinates of its texinfo.
void ExtractBSPSkyFace(const CBSPFileReader& reader, std::uint32_t face, BSPPrimitive& primitive);

//Decodes a GoldSrc miptex record (header, four mips, palette) into RGBA8 texels of its first mip, as the engine's
//GL_Upload8 converts it: palette index 255 of '{' textures is transparent black. False with a reason when the record
//does not hold what its header says.
bool DecodeMiptex(const std::uint8_t* pRecord, std::size_t recordBytes, std::vector<std::uint8_t>& texels, std::uint32_t& width, std::uint32_t& height, std::string& error);

//Resolves, encodes and shares the images of a conversion (BSPTextureConverter.cpp).
//Suffixes of the sky box faces, in GoldSrcRender::SkyFace order, as the engine names their images (R_LoadSkys).
inline constexpr const char* SkyboxFaceSuffixes[GoldSrcRender::SkyFaceCount] = {"rt", "bk", "lf", "ft", "up", "dn"};

struct BSPDecodedImage
{
    std::vector<std::uint8_t> texels;
    std::uint32_t             width{};
    std::uint32_t             height{};
};

//The images of the sky box faces of skyName, in the order rt, bk, lf, ft, up, dn: their glTF textures, -1 for a face
//without image, and the decoded RGBA8 texels of the faces that have one (empty otherwise).
struct BSPSkyboxImages
{
    std::string                                              skyName;
    std::array<int, GoldSrcRender::SkyFaceCount>             textures{};
    std::array<BSPDecodedImage, GoldSrcRender::SkyFaceCount> images;

    bool HasImage() const
    {
        for (const auto texture : textures)
        {
            if (texture >= 0)
                return true;
        }

        return false;
    }
};

class CBSPTextureConverter
{
public:
    CBSPTextureConverter(const BSPConversionSource& source, CGLTFDocumentBuilder& document, CConversionDiagnostics& diagnostics);

    //glTF texture of a miptex slot, -1 when no image could be resolved (a warning is reported once).
    int GetMiptexTexture(std::uint32_t slot);

    //glTF texture of a detail binding, -1 when the resolver has none.
    int GetDetailTexture(std::uint32_t detailBinding);

    //Images of the six skybox faces of skyname, resolved as SkyboxFace requests (a warning for each missing face). The
    //textures clamp to the edges of their image.
    BSPSkyboxImages GetSkyboxImages(const std::string& skyName);

    bool HasAlpha(std::uint32_t slot) const;

    void RetainEmissiveImages(const tinygltf::Model& model);

    const BSPDecodedImage* GetDecodedImage(int texture) const
    {
        const auto it = m_DecodedImages.find(texture);
        return it == m_DecodedImages.end() ? nullptr : &it->second;
    }

    std::uint32_t GetTextureCount() const
    {
        return m_TextureCount;
    }

    std::uint32_t GetMissingCount() const
    {
        return m_MissingCount;
    }

    //SHA-256 of every resolved image, in the order they were resolved.
    std::string FinishHash();

private:
    int  AddImage(const std::string& name, const std::vector<std::uint8_t>& texels, std::uint32_t width, std::uint32_t height, bool bAlpha, int sampler);
    int  GetRepeatSampler();
    int  GetClampSampler();
    bool Resolve(const TextureRequest& request, const BSPRawMiptex* pEmbedded, std::vector<std::uint8_t>& texels, std::uint32_t& width, std::uint32_t& height, std::string& error);

    const BSPConversionSource&     m_Source;
    CGLTFDocumentBuilder&          m_Document;
    CConversionDiagnostics&        m_Diagnostics;
    std::map<std::uint32_t, int>   m_MiptexTextures;
    std::map<int, BSPDecodedImage> m_DecodedImages;
    std::map<std::uint32_t, bool>  m_MiptexAlpha;
    std::map<std::uint32_t, int>   m_DetailTextures;
    std::uint32_t                  m_TextureCount{};
    std::uint32_t                  m_MissingCount{};
    CSHA256                        m_Hash;
    int                            m_Sampler{-1};
    int                            m_ClampSampler{-1};
};

//extras.goldsrc of the conversion's objects (BSPMetadataWriter.cpp). Strings that are not UTF-8 are stored as
//Latin-1 decoded text and marked (see MakeJSONString).
nlohmann::json MakeEntityExtras(const BSPConversionSource& source, std::uint32_t entity, CConversionDiagnostics& diagnostics);
//The light child of a light entity: its lightstyle and targetname (see AddPointLights).
nlohmann::json MakeLightExtras(const BSPConversionSource& source, std::uint32_t entity, CConversionDiagnostics& diagnostics);
nlohmann::json MakePrimitiveExtras(const BSPConversionSource& source, std::uint32_t model, BSPGeometryKind kind, const BSPPrimitive& primitive);
nlohmann::json MakeMaterialExtras(const BSPConversionSource& source, const BSPMaterialKey& key, CBSPTextureConverter& textures);

//UTF-8 text for JSON: the bytes themselves when they are UTF-8, otherwise each byte decoded as Latin-1 (U+0000 to
//U+00FF, reversible) with bLatin1 set.
std::string MakeJSONString(const std::string& bytes, bool& bLatin1);

//Display name of a miptex slot, or "missing" without one.
std::string GetMiptexName(const BSPConversionSource& source, std::uint32_t miptex);

//Embeds the optional spatial data of the options into the document's buffer and describes it (BSPSpatialDataWriter.cpp).
//Returns null when neither kind is enabled.
nlohmann::json AppendBSPSpatialData(const BSPConversionSource& source, CGLTFDocumentBuilder& document, ConversionStatistics& statistics);

//Builds world-space rectangular emitters without modifying the glTF scene.
ConversionError BuildBSPAreaLights(const BSPConversionSource& source, const std::vector<BSPModelGeometry>& models, const CBSPTextureConverter& textures, const tinygltf::Model& gltf, nlohmann::json& output, std::string& error);

nlohmann::json FitBSPAreaLightPrimitive(const BSPPrimitive& primitive, const tinygltf::Material& material, const BSPDecodedImage& image, std::uint32_t budget, nlohmann::json& statistics, std::uint32_t modelIndex);

//The sky of a converted map (BSPSkyBoxConverter.cpp): extras.goldsrc.sky, null when the map has neither sky faces nor
//a worldspawn skyname, and the cube map file, empty when it is not exported.
struct BSPSkyBox
{
    nlohmann::json            extras;
    std::vector<std::uint8_t> environmentMap;
};

//Resolves the sky box images of a map with sky faces as the engine loads them and, when a face has an image, makes the
//cube map with BSPConversionOptions::bExportSkyEnvironmentMap, which extras.goldsrc.sky names as environmentMapName
//(relative to the asset), and adds the Sky_Box node under the scene's root with bExportSkyBoxGeometry.
BSPSkyBox AddSkyBox(const BSPConversionSource& source, std::uint32_t worldspawn, const std::string& environmentMapName, CBSPTextureConverter& textures, CGLTFDocumentBuilder& document, ConversionStatistics& statistics);

//The DDS cube map (RGBA8 sRGB, one mip) of the sky box faces, oriented for the converted scene's axes (see
//AddSkyBox); faces without image are black. Every face of the cube is as large as the largest image.
std::vector<std::uint8_t> MakeSkyEnvironmentMap(const BSPSkyboxImages& images);
} // namespace GoldSrcAssetConverter

#endif
