#if !defined(IUTIL_GOLDSRC_ASSET_CONVERTER_H)
#    define IUTIL_GOLDSRC_ASSET_CONVERTER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "GoldSrcRenderBSPTypes.h"
#    include "interface.h"

#    include <cstdint>

//Converts GoldSrc assets into glTF 2.0 scene assets on the CPU, without a render device or a running engine.
//This stage converts BSP30 maps (BSPLayout::GoldSrc and BSPLayout::BlueShift) only. The converter lives in
//UtilGoldSrcAssetConverter.dll and is published only through EXPOSE_SINGLE_INTERFACE; callers resolve
//UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION through CreateInterface. Every type reachable from the interface is
//ABI stable across modules and build configurations: nothing the converter allocates outlives a call, and the
//converted files, diagnostics and texture images go through interfaces the caller implements.
//The glTF layout and the extras.goldsrc schema are described in UtilGoldSrcAssetConverter/docs.
namespace GoldSrcAssetConverter
{
enum class OutputFormat : std::uint32_t
{
    //A .gltf JSON document whose binary buffer is embedded as a data URI.
    GLTF,
    //A binary .glb container: JSON chunk and BIN chunk.
    GLB
};

//What a conversion keeps. Everything a renderer needs is kept by default; the BSP spatial structures, which the
//engine keeps reading from the original BSP, are only embedded on request.
struct BSPConversionOptions
{
    OutputFormat format{OutputFormat::GLB};

    //Geometry and entities. Sky faces are converted to real triangles; brush entities and point entities each get a
    //node of their own.
    bool bPreserveSkyGeometry{true};
    bool bPreserveBrushEntities{true};
    bool bPreservePointEntities{true};

    //Rendering metadata: lightmap atlases (TEXCOORD_1), the lightstyle of every lightmap layer, face/surface mapping
    //tables and the ordered entity key values.
    bool bPreserveLightmaps{true};
    bool bPreserveLightstyles{true};
    bool bPreserveSurfaceMetadata{true};
    bool bPreserveEntityKeyValues{true};

    //Images are stored in the binary buffer; otherwise they are written as external PNG files under textures/ beside
    //the asset.
    bool bEmbedTextures{true};

    //Optional raw BSP spatial data, stored in the binary buffer and indexed by extras.goldsrc.spatialData.
    //Visibility: planes, nodes, leaves, marksurfaces and the compressed PVS. Collision: planes, clipnodes, nodes,
    //leaves and the hull roots of every model.
    bool bEmbedBSPVisibilityData{false};
    bool bEmbedBSPCollisionData{false};

    //Uniform scale applied by the root node, in glTF units per GoldSrc unit; finite and above 0.
    float scaleFactor{1.0f};

    //Multiplier of normalized RAD texture light intensity; finite and nonnegative. 0 disables texture emission.
    //The resulting emissive strengths must fit in a float. This does not affect base colors or lightmap metadata.
    float emissiveScale{4.0f};

    //Multiplier for constant point lights generated as children of preserved "light" entities. Finite and
    //nonnegative; 0 disables conversion and leaves the entity nodes intact. Intensity is also multiplied by
    //scaleFactor squared to preserve illumination when scaling the map. Final intensities must fit in a float.
    float pointLightIntensityScale{1.0f};

    //Write <resource>/<resource>.sh9/area-lights.json, including an empty list when disabled to replace stale
    //descriptors.
    bool          bExportAreaLights{true};
    std::uint32_t maxAreaLightsPerGroup{16}; //Valid range: 1-64.
};

enum class ConversionError : std::uint32_t
{
    None,
    InvalidRequest,
    InvalidBSP,
    InvalidEntityLump,
    LimitExceeded,
    OutOfMemory,
    OutputFailed,
    InternalError
};

enum class DiagnosticSeverity : std::uint32_t
{
    Info,
    Warning,
    Error
};

//Why something could not be converted exactly. Every warning names its source object in the message.
enum class DiagnosticCode : std::uint32_t
{
    MissingTexture,
    InvalidTextureData,
    MissingDetailTexture,
    MissingSkyboxTexture,
    MissingWorldspawn,
    DuplicateWorldspawn,
    InvalidModelReference,
    UnreferencedSubmodel,
    DegenerateFace,
    UnexportedFace,
    NonUTF8Text,
    InvalidEntityValue,
    ZeroLengthNormal,
    ConversionFailure
};

//szMessage is borrowed for the duration of the sink call.
struct DiagnosticView
{
    DiagnosticSeverity severity{DiagnosticSeverity::Warning};
    DiagnosticCode     code{DiagnosticCode::ConversionFailure};
    const char*        szMessage{};
};

//The images a conversion asks the caller for. Miptex: the texture of a miptex slot of the BSP's texture lump, as the
//engine loads it from the BSP or a WAD. Detail: the detail texture of a slot (see GoldSrcRender::DetailTextureBinding).
//SkyboxFace: one face of the worldspawn skyname environment, named "<skyname><suffix>" with the suffixes rt, bk, lf,
//ft, up, dn.
enum class TextureKind : std::uint32_t
{
    Miptex,
    Detail,
    SkyboxFace
};

constexpr std::uint32_t NoMiptexIndex = 0xFFFFFFFF;

//Strings are borrowed for the duration of the resolver call. szName of a Miptex request is the name the BSP stores,
//which the engine looks up in WADs (before it blanks the third character of names containing '~'); that of a Detail
//request is the engine's texture name, which detail texture lists match. assetId is the binding's texture (or detail texture)
//of the request's BSPAssetView, GoldSrcRender::InvalidAssetId when the caller did not bind the slot. width and height
//are the miptex header's for Miptex and Detail requests, 0 for skybox faces. bEmbedded tells that the BSP itself holds
//the miptex pixels; the converter decodes them when the resolver declines.
struct TextureRequest
{
    TextureKind            kind{TextureKind::Miptex};
    const char*            szName{};
    std::uint32_t          miptexIndex{NoMiptexIndex};
    GoldSrcRender::AssetId assetId{GoldSrcRender::InvalidAssetId};
    std::uint32_t          width{};
    std::uint32_t          height{};
    bool                   bEmbedded{};
};

//RGBA8: width * height texels, 4 bytes each, rows top to bottom, in texture gamma. Miptex: a whole GoldSrc miptex
//record as WAD3 and BSP texture lumps store it (header, four mips, palette); width and height are read from it and the
//palette is applied as the engine does, palette index 255 of '{' textures being transparent.
enum class ResolvedTextureFormat : std::uint32_t
{
    RGBA8,
    Miptex
};

//pData is borrowed until the resolver is called again or the conversion returns.
struct ResolvedTexture
{
    ResolvedTextureFormat format{ResolvedTextureFormat::RGBA8};
    std::uint32_t         width{};
    std::uint32_t         height{};
    const std::uint8_t*   pData{};
    std::uint64_t         dataBytes{};
};

//Supplies the images a conversion asks for, on the converting thread.
class ITextureResolver : public IBaseInterface
{
public:
    //Returns false when the caller has no image for the request.
    virtual bool ResolveTexture(const TextureRequest& request, ResolvedTexture* pTexture) noexcept = 0;
};

//Receives the converted files, on the converting thread, with paths relative to the sink's root: the asset itself first,
//<resource>/<resource>.glb or .gltf, then its external images under <resource>/textures/, the area light descriptor
//<resource>/<resource>.sh9/area-lights.json, and last the render manifest <resource>.manifest.json, which names the
//others relative to the root. szRelativePath and pData are borrowed for the call. Assets written to one root need
//distinct resource names.
class IConversionOutputSink : public IBaseInterface
{
public:
    //Returns false to fail the conversion with ConversionError::OutputFailed.
    virtual bool WriteFile(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes) noexcept = 0;
};

//Receives every diagnostic as it is found, on the converting thread.
class IDiagnosticSink : public IBaseInterface
{
public:
    virtual void OnDiagnostic(const DiagnosticView& diagnostic) noexcept = 0;
};

//Bytes of szResourceName and szSourcePath, without their terminator.
constexpr std::uint32_t MaxConversionStringLength = 4096;

//Everything is borrowed for the call. asset's bindings name the textures the caller knows (each a slot the texture
//lump holds, at most once), and the converter binds every other miptex slot itself, so that every face keeps its
//texture name, size and texinfo flags, as the engine loads the map. pOutputSink is required; pTextureResolver and
//pDiagnosticSink may be null. szResourceName names the asset directory and the output files (see IConversionOutputSink),
//characters other than letters, digits, '-', '_' and '.' replaced by '_'; null or empty selects the asset's identifier
//file name, or "map". A name of dots only is InvalidRequest. szSourcePath is only recorded in the metadata and may be
//null. Both are null-terminated UTF-8 of at most MaxConversionStringLength bytes.
struct BSPConversionRequestView
{
    GoldSrcRender::BSPAssetView asset;
    BSPConversionOptions        options;
    ITextureResolver*           pTextureResolver{};
    const char*                 szResourceName{};
    const char*                 szSourcePath{};
    IConversionOutputSink*      pOutputSink{};
    IDiagnosticSink*            pDiagnosticSink{};
};

//Counts of the converted asset. Faces count face entries of models: a face listed by two models counts twice.
struct ConversionStatistics
{
    std::uint32_t modelCount{};
    std::uint32_t brushModelCount{};
    std::uint32_t worldFaceCount{};
    std::uint32_t skyFaceCount{};
    std::uint32_t waterFaceCount{};
    std::uint32_t degenerateFaceCount{};
    std::uint32_t entityCount{};
    std::uint32_t brushEntityNodeCount{};
    std::uint32_t pointEntityNodeCount{};
    std::uint32_t unreferencedSubmodelCount{};
    std::uint32_t meshCount{};
    std::uint32_t materialCount{};
    std::uint32_t textureCount{};
    std::uint32_t missingTextureCount{};
    std::uint32_t lightmapPageCount{};
    std::uint32_t lightmapCount{};
    std::uint64_t visibilityDataBytes{};
    std::uint64_t collisionDataBytes{};
};

//Capacities of the strings of a summary, terminator included: a lowercase hex SHA-256 digest, the cache key and the
//error message.
constexpr std::uint32_t SHA256HexCapacity    = 65;
constexpr std::uint32_t CacheKeyCapacity     = 256;
constexpr std::uint32_t ErrorMessageCapacity = 1024;

//error is ConversionError::None on success, when every file has been written to the output sink. szErrorMessage is
//truncated at a UTF-8 character boundary. warningCount counts the diagnostics given to the sink. converterVersion
//changes whenever the same input converts to different output; extrasSchemaVersion is the version of the
//extras.goldsrc schema (docs/GoldSrcBSPExtrasSchema.md); both are the loaded module's. szSourceHash and szTextureHash
//are SHA-256 digests of the BSP file and of every resolved image; szCacheKey combines them with both versions and the
//options, so that two conversions with the same cache key produce the same bytes. Strings are empty when a conversion
//fails before they are known.
struct BSPConversionSummary
{
    ConversionError      error{ConversionError::None};
    char                 szErrorMessage[ErrorMessageCapacity]{};
    std::uint32_t        warningCount{};
    ConversionStatistics statistics;
    std::uint32_t        converterVersion{};
    std::uint32_t        extrasSchemaVersion{};
    char                 szSourceHash[SHA256HexCapacity]{};
    char                 szTextureHash[SHA256HexCapacity]{};
    char                 szCacheKey[CacheKeyCapacity]{};
};

static_assert(GoldSrcRender::IsABIStable<BSPConversionOptions> && GoldSrcRender::IsABIStable<DiagnosticView>);
static_assert(GoldSrcRender::IsABIStable<TextureRequest> && GoldSrcRender::IsABIStable<ResolvedTexture>);
static_assert(GoldSrcRender::IsABIStable<BSPConversionRequestView> && GoldSrcRender::IsABIStable<ConversionStatistics>);
static_assert(GoldSrcRender::IsABIStable<BSPConversionSummary>);
} // namespace GoldSrcAssetConverter

//Stateless: conversions may run on several threads at once, each calling back on its own thread.
class IUtilGoldSrcAssetConverter : public IBaseInterface
{
public:
    //Converts one BSP map into a glTF scene asset (see docs/BSPConversion.md).
    virtual GoldSrcAssetConverter::BSPConversionSummary ConvertBSP(const GoldSrcAssetConverter::BSPConversionRequestView& request) noexcept = 0;

    //Finds a texture lump named szName (compared without case, as the engine's W_GetLumpinfo does) in a WAD3 file held
    //in memory and returns it as a ResolvedTextureFormat::Miptex texture borrowing the file's bytes. Returns false when
    //the file is not a valid WAD3 or has no such miptex lump. A helper for texture resolvers.
    virtual bool FindWAD3Texture(const std::uint8_t* pWad, std::uint64_t wadBytes, const char* szName, GoldSrcAssetConverter::ResolvedTexture* pTexture) noexcept = 0;
};

#    define UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION "UtilGoldSrcAssetConverterAPI_004"

#endif
