#include "BSPConverterInternal.h"
#include "CoordinateConverter.h"
#include "HalfLifeTextureLights.h"
#include "PNGEncoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace GoldSrcAssetConverter
{
//Placeholder colors of materials without image: sky faces, which GoldSrc covers with the sky box, and faces whose
//texture could not be resolved.
constexpr std::size_t s_ColorComponents                            = 4;
static const double   s_SkyPlaceholderColor[s_ColorComponents]     = {0.5, 0.7, 1.0, 1.0};
static const double   s_MissingPlaceholderColor[s_ColorComponents] = {1.0, 0.0, 1.0, 1.0};

//Alpha tested texels are those whose alpha is below this; '{' textures only have alpha 0 or 1.
constexpr double s_AlphaCutoff = 0.5;

//RAD's RGB and intensity normalization before the caller's emissive multiplier.
constexpr double s_RADNormalization = 255;

//GoldSrc's default point light brightness and RGB normalization, used as a relighting starting point.
constexpr double      s_DefaultPointLightBrightness = 300;
constexpr double      s_PointLightRGBNormalization  = 255;
constexpr std::size_t s_PointLightColorComponents   = 3;
constexpr std::size_t s_PointLightMaxComponents     = s_PointLightColorComponents + 1;

//Normals shorter than this are rebuilt (see NormalizePrimitiveNormals).
constexpr float s_MinNormalLength = 1e-6f;

constexpr char s_DefaultResourceName[]       = "map";
constexpr char s_ExternalImageDirectory[]    = "textures/";
constexpr char s_GeneratorName[]             = "UtilGoldSrcAssetConverter";
constexpr char s_UnlitExtension[]            = "KHR_materials_unlit";
constexpr char s_EmissiveStrengthExtension[] = "KHR_materials_emissive_strength";
constexpr int  s_NodeNameIndexDigits         = 4;

//Files beside the asset, in <resource>/: the light probe directory <resource>.sh9/ holds the area light descriptor
//written here and the SH9 files a later bake writes. The render manifest is <resource>.manifest.json beside <resource>/.
constexpr char s_LightProbeDirectoryExtension[] = ".sh9/";
constexpr char s_AreaLightFileName[]            = "area-lights.json";
constexpr char s_LightProbeHeaderFileName[]     = "runtime.json";
constexpr char s_LightProbeAtlasFileName[]      = "atlas_rgba32f.bin";
constexpr char s_ManifestExtension[]            = ".manifest.json";
constexpr char s_ManifestWildcard[]             = "*";
constexpr char s_ManifestWorldRole[]            = "world";
constexpr char s_ManifestWorldNodeName[]        = "World";
constexpr char s_ManifestLightProbeType[]       = "SH9";

//Bytes after the first of a UTF-8 character are 10xxxxxx.
constexpr unsigned char s_UTF8ContinuationMask = 0xC0;
constexpr unsigned char s_UTF8ContinuationBits = 0x80;

//Columns of a face record (MakePrimitiveExtras), named in the asset's extras.
static const char* const s_FaceRecordFields[] = {"face", "firstIndex", "indexCount", "texInfo", "sourceSurface", "lightmapPage", "lightmapX", "lightmapY", "lightmapWidth",
                                                 "lightmapHeight", "style0", "style1", "style2", "style3", "lightOffset", "textureMinS", "textureMinT"};

void CConversionDiagnostics::Report(DiagnosticSeverity severity, DiagnosticCode code, const std::string& message)
{
    ++m_Count;

    if (!m_pSink)
        return;

    DiagnosticView diagnostic;
    diagnostic.severity  = severity;
    diagnostic.code      = code;
    diagnostic.szMessage = message.c_str();

    m_pSink->OnDiagnostic(diagnostic);
}

const char* GetGeometryKindName(BSPGeometryKind kind)
{
    switch (kind)
    {
        case BSPGeometryKind::Sky:
            return "sky";
        case BSPGeometryKind::Water:
            return "water";
        case BSPGeometryKind::Surface:
            break;
    }

    return "surface";
}

//Node and mesh name part of a kind.
static const char* GetGeometryKindNodeName(BSPGeometryKind kind)
{
    switch (kind)
    {
        case BSPGeometryKind::Sky:
            return "Sky";
        case BSPGeometryKind::Water:
            return "Water";
        case BSPGeometryKind::Surface:
            break;
    }

    return "Geometry";
}

std::uint32_t CBSPMaterialRegistry::Register(const BSPMaterialKey& key)
{
    const auto it = m_Indices.find(key);

    if (it != m_Indices.end())
        return it->second;

    const auto index = static_cast<std::uint32_t>(m_Keys.size());

    m_Indices.emplace(key, index);
    m_Keys.push_back(key);

    return index;
}

//Copies as much of source as szDestination holds with its terminator, without cutting a UTF-8 character.
template <std::size_t Capacity>
static void CopyTruncatedString(char (&szDestination)[Capacity], std::string_view source)
{
    static_assert(Capacity > 0);

    auto length = std::min(source.size(), Capacity - 1);

    while (length && length < source.size() && (static_cast<unsigned char>(source[length]) & s_UTF8ContinuationMask) == s_UTF8ContinuationBits)
        --length;

    std::memcpy(szDestination, source.data(), length);
    szDestination[length] = '\0';
}

//Does not allocate, so that the exception handlers of ConvertBSP can report with it.
static void Fail(BSPConversionSummary& summary, ConversionError error, std::string_view message)
{
    summary.error = error;
    CopyTruncatedString(summary.szErrorMessage, message);
}

//A request string may be null; otherwise it must end within MaxConversionStringLength bytes. No byte past the limit
//is read.
static bool ValidRequestString(const char* szString)
{
    if (!szString)
        return true;

    for (std::uint32_t i = 0; i <= MaxConversionStringLength; ++i)
    {
        if (!szString[i])
            return true;
    }

    return false;
}

//Characters of output file names: letters, digits, '-', '_' and '.'.
static std::string MakeSafeFileName(const std::string& name)
{
    std::string safe;

    for (const auto c : name)
        safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ? c : '_');

    return safe;
}

//The request's resource name, otherwise the identifier's file name without extension.
static std::string GetResourceName(const BSPConversionRequestView& request)
{
    if (request.szResourceName && *request.szResourceName)
        return MakeSafeFileName(request.szResourceName);

    if (!request.asset.szIdentifier || !*request.asset.szIdentifier)
        return s_DefaultResourceName;

    std::string name = request.asset.szIdentifier;

    const auto separator = name.find_last_of("/\\");

    if (separator != std::string::npos)
        name = name.substr(separator + 1);

    const auto extension = name.find_last_of('.');

    if (extension != std::string::npos && extension)
        name = name.substr(0, extension);

    return name.empty() ? std::string(s_DefaultResourceName) : MakeSafeFileName(name);
}

//The resource name also names the asset's directory, so "." and ".." would leave the sink's root.
static bool IsValidResourceDirectory(const std::string& resourceName)
{
    return resourceName.find_first_not_of('.') != std::string::npos;
}

//The render manifest of an asset (docs/BSPConversion.md, "Render manifest"). Paths are relative to the manifest's
//directory, which holds the asset directory. The engine reads the model and the files listed; the light probe files
//are declared before a bake writes them, so that the manifest stays valid once they exist.
static nlohmann::json MakeRenderManifest(const BSPConversionOptions& options, const std::string& assetDirectory, const std::string& assetPath, const std::string& lightProbeDirectory, const std::string& sourceDigest, bool bExternalImages)
{
    const auto lightProbeHeader = lightProbeDirectory + s_LightProbeHeaderFileName;
    const auto lightProbeAtlas  = lightProbeDirectory + s_LightProbeAtlasFileName;

    nlohmann::json model;
    model["path"]      = assetPath;
    model["unitScale"] = options.scaleFactor;

    nlohmann::json files = nlohmann::json::array();

    if (bExternalImages)
        files.push_back(assetDirectory + s_ExternalImageDirectory + s_ManifestWildcard);

    files.push_back(lightProbeHeader);
    files.push_back(lightProbeAtlas);

    nlohmann::json world;
    world["name"]  = s_ManifestWorldNodeName;
    world["model"] = 0;
    world["role"]  = s_ManifestWorldRole;

    nlohmann::json lightProbes;
    lightProbes["type"]   = s_ManifestLightProbeType;
    lightProbes["header"] = lightProbeHeader;
    lightProbes["atlas"]  = lightProbeAtlas;

    nlohmann::json manifest;
    manifest["schemaVersion"]    = RenderManifestSchemaVersion;
    manifest["converterVersion"] = ConverterVersion;
    manifest["bspSha256"]        = sourceDigest;
    manifest["models"]           = nlohmann::json::array({std::move(model)});
    manifest["files"]            = std::move(files);
    manifest["graph"]            = nlohmann::json::array({std::move(world)});
    manifest["lightProbes"]      = std::move(lightProbes);

    return manifest;
}

static bool ValidateOptions(const BSPConversionOptions& options, std::string& error)
{
    if (options.format != OutputFormat::GLB && options.format != OutputFormat::GLTF)
    {
        error = "unknown output format";
        return false;
    }

    if (!std::isfinite(options.scaleFactor) || !(options.scaleFactor > 0))
    {
        error = "the scale factor must be finite and above 0";
        return false;
    }

    if (!std::isfinite(options.emissiveScale) || options.emissiveScale < 0)
    {
        error = "the emissive scale must be finite and nonnegative";
        return false;
    }

    if (options.maxAreaLightsPerGroup < 1 || options.maxAreaLightsPerGroup > 64)
    {
        error = "area lights per group must be in [1, 64]";
        return false;
    }

    if (!std::isfinite(options.pointLightIntensityScale) || options.pointLightIntensityScale < 0)
    {
        error = "the point light intensity scale must be finite and nonnegative";
        return false;
    }

    for (const auto& light : s_HalfLifeTextureLights)
    {
        if (light.intensity / s_RADNormalization * options.emissiveScale > std::numeric_limits<float>::max())
        {
            error = "the emissive scale is too large for float texture light strengths";
            return false;
        }
    }

    return true;
}

//The caller's bindings, completed with a binding for every other present miptex slot (slots, ascending), so that
//BuildBSPWorld gives every face its texture's size, category and texinfo flags, as the engine names every texture it
//loads. The added bindings carry GoldSrcRender::InvalidAssetId: the converter resolves images by slot, not by asset.
//Returns false when a caller binding names a slot without miptex or a slot bound twice.
static bool BindEveryMiptex(BSPConversionSource& source, const std::vector<std::uint32_t>& slots, std::string& error)
{
    constexpr std::uint32_t s_Unbound = std::numeric_limits<std::uint32_t>::max();

    const auto& asset = source.pRequest->asset;

    source.bindings.clear();
    source.bindingSlots.clear();

    if (asset.textureCount && !asset.pTextures)
    {
        error = "the request has texture bindings without an array";
        return false;
    }

    //Caller binding of each present slot.
    std::vector<std::uint32_t> callerBindings(slots.size(), s_Unbound);

    for (std::uint32_t i = 0; i < asset.textureCount; ++i)
    {
        const auto slot = asset.pTextures[i].slot;
        const auto it   = std::lower_bound(slots.begin(), slots.end(), slot);

        if (it == slots.end() || *it != slot)
        {
            error = "texture binding " + std::to_string(i) + " names miptex slot " + std::to_string(slot) + ", which the texture lump does not hold";
            return false;
        }

        auto& callerBinding = callerBindings[static_cast<std::size_t>(it - slots.begin())];

        if (callerBinding != s_Unbound)
        {
            error = "miptex slot " + std::to_string(slot) + " is bound twice";
            return false;
        }

        callerBinding = i;
    }

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        GoldSrcRender::TextureBinding binding;
        binding.slot = slots[i];

        if (callerBindings[i] != s_Unbound)
            binding.texture = asset.pTextures[callerBindings[i]].texture;

        source.bindings.push_back(binding);
        source.bindingSlots.push_back(slots[i]);
    }

    return true;
}

static void ReadEntities(BSPConversionSource& source, CConversionDiagnostics& diagnostics)
{
    const auto modelCount = static_cast<std::uint32_t>(source.world.submodels.size());

    source.entityInfos.clear();

    for (const auto& entity : source.entities)
    {
        std::vector<std::string> invalidValues;
        source.entityInfos.push_back(InterpretBSPEntity(entity, modelCount, invalidValues));

        const auto& info = source.entityInfos.back();

        bool       bLatin1{};
        const auto label = "entity " + std::to_string(entity.index) + " (" + MakeJSONString(info.classname, bLatin1) + ")";

        for (const auto& key : invalidValues)
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidEntityValue, label + ": key \"" + key + "\" is not a vector of numbers; missing components are 0");

        if (info.modelKind == BSPEntityModelKind::Invalid)
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidModelReference, label + ": model \"" + MakeJSONString(info.model, bLatin1) + "\" names no model of the file (" + std::to_string(modelCount) + " models); the entity is kept as a point entity");
    }
}

//The entity mapped to model 0: the first worldspawn, or none.
static std::uint32_t FindWorldspawn(const BSPConversionSource& source, CConversionDiagnostics& diagnostics)
{
    auto worldspawn = static_cast<std::uint32_t>(source.entityInfos.size());

    for (std::uint32_t i = 0; i < source.entityInfos.size(); ++i)
    {
        if (!source.entityInfos[i].bWorldspawn)
            continue;

        if (worldspawn == source.entityInfos.size())
            worldspawn = i;
        else
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::DuplicateWorldspawn, "entity " + std::to_string(i) + " is another worldspawn; only entity " + std::to_string(worldspawn) + " owns model 0, this one is kept as a point entity");
    }

    if (worldspawn == source.entityInfos.size())
        diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::MissingWorldspawn, "the entity lump has no worldspawn; model 0 is still converted, without entity data");

    return worldspawn;
}

//Unit normals for glTF: BSP plane normals are unit vectors up to rounding; a zero normal, which a valid file does not
//have, is replaced by +z and counted.
static std::uint32_t NormalizePrimitiveNormals(BSPPrimitive& primitive)
{
    std::uint32_t replaced = 0;

    for (auto& normal : primitive.normals)
    {
        const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

        if (!(length > s_MinNormalLength))
        {
            normal = {0, 0, 1};
            ++replaced;
            continue;
        }

        normal = {normal.x / length, normal.y / length, normal.z / length};
    }

    return replaced;
}

//glTF fronts are counterclockwise about the BSP face normal; the renderer's index order may face the other way.
//Only swap triangle corners, keeping vertex attributes and face index ranges intact for surfaces, sky and water.
//Double precision avoids float cancellation on the nearly collinear edges of compiled BSP polygons.
static void AlignPrimitiveWinding(BSPPrimitive& primitive)
{
    constexpr std::size_t s_TriangleVertices = 3;

    for (std::size_t i = 0; i < primitive.indices.size(); i += s_TriangleVertices)
    {
        const auto& a      = primitive.positions[primitive.indices[i]];
        const auto& b      = primitive.positions[primitive.indices[i + 1]];
        const auto& c      = primitive.positions[primitive.indices[i + 2]];
        const auto& normal = primitive.normals[primitive.indices[i]];

        const double ab[] = {static_cast<double>(b.x) - a.x, static_cast<double>(b.y) - a.y, static_cast<double>(b.z) - a.z};
        const double ac[] = {static_cast<double>(c.x) - a.x, static_cast<double>(c.y) - a.y, static_cast<double>(c.z) - a.z};
        const auto   dot  = (ab[1] * ac[2] - ab[2] * ac[1]) * normal.x +
            (ab[2] * ac[0] - ab[0] * ac[2]) * normal.y +
            (ab[0] * ac[1] - ab[1] * ac[0]) * normal.z;

        if (dot < 0)
            std::swap(primitive.indices[i + 1], primitive.indices[i + 2]);
    }
}

static void SetPlaceholderColor(tinygltf::Material& material, const double (&color)[s_ColorComponents])
{
    material.pbrMetallicRoughness.baseColorFactor.assign(std::begin(color), std::end(color));
}

static std::string MakeMaterialName(const BSPConversionSource& source, const BSPMaterialKey& key)
{
    auto name = GetMiptexName(source, key.miptex);

    if (key.kind != BSPGeometryKind::Surface)
        name += std::string("_") + GetGeometryKindName(key.kind);
    else if (!key.bLightmapped)
        name += "_unlit";

    return name;
}

static bool TextureLightNameMatches(const std::string& name, const char* szLightName)
{
    const std::string_view lightName(szLightName);

    if (name.size() != lightName.size())
        return false;

    for (std::size_t i = 0; i < name.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(lightName[i])))
            return false;
    }

    return true;
}

//RAD colors are linear radiosity coefficients. Normalize RGB and intensity separately by 255; the resulting
//emission is texture * (RGB / 255) * (intensity / 255) * emissiveScale. This is not an HLRAD unit match.
//Use glTF HDR emission on a PBR surface so path tracers can light nearby geometry; never mark emitters as unlit.
static void AddTextureLight(tinygltf::Material& material, const BSPRawMiptex& miptex, float emissiveScale)
{
    if (emissiveScale == 0)
        return;

    for (const auto& light : s_HalfLifeTextureLights)
    {
        if (!TextureLightNameMatches(miptex.rawName, light.szName))
            continue;

        material.emissiveTexture.index    = material.pbrMetallicRoughness.baseColorTexture.index;
        material.emissiveTexture.texCoord = 0;
        material.emissiveFactor.clear();

        for (const auto color : light.color)
            material.emissiveFactor.push_back(color / s_RADNormalization);

        tinygltf::Value::Object strength;
        strength["emissiveStrength"]                     = tinygltf::Value(light.intensity / s_RADNormalization * emissiveScale);
        material.extensions[s_EmissiveStrengthExtension] = tinygltf::Value(std::move(strength));
        return;
    }
}

static void AddMaterials(const BSPConversionSource& source, const CBSPMaterialRegistry& registry, CBSPTextureConverter& textures, CGLTFDocumentBuilder& document)
{
    auto& model = document.GetModel();
    bool  bSkyMaterial{};
    bool  bEmissiveMaterial{};

    for (const auto& key : registry.GetKeys())
    {
        tinygltf::Material material;
        material.name                                 = MakeMaterialName(source, key);
        material.pbrMetallicRoughness.metallicFactor  = 0;
        material.pbrMetallicRoughness.roughnessFactor = 1;

        const auto slot = key.binding == MissingBinding ? NoMiptexIndex : source.bindingSlots[key.binding];

        if (key.kind == BSPGeometryKind::Sky)
        {
            //The sky remains a placeholder until the caller replaces it with an environment.
            SetPlaceholderColor(material, s_SkyPlaceholderColor);
            material.extensions[s_UnlitExtension] = tinygltf::Value(tinygltf::Value::Object());
            bSkyMaterial                          = true;
        }
        else
        {
            const auto texture = slot == NoMiptexIndex ? -1 : textures.GetMiptexTexture(slot);

            if (texture >= 0)
            {
                material.pbrMetallicRoughness.baseColorTexture.index    = texture;
                material.pbrMetallicRoughness.baseColorTexture.texCoord = 0;

                if (textures.HasAlpha(slot))
                {
                    material.alphaMode   = "MASK";
                    material.alphaCutoff = s_AlphaCutoff;
                }

                if (key.kind == BSPGeometryKind::Surface)
                    AddTextureLight(material, source.reader.GetMiptexes()[slot], source.pRequest->options.emissiveScale);

                bEmissiveMaterial |= material.extensions.count(s_EmissiveStrengthExtension) != 0;
            }
            else
                SetPlaceholderColor(material, s_MissingPlaceholderColor);
        }

        //GoldSrc draws water from both sides (EmitWaterPolys reverses the pieces seen from behind).
        material.doubleSided = key.kind == BSPGeometryKind::Water;

        nlohmann::json extras;
        extras["goldsrc"] = MakeMaterialExtras(source, key, textures);
        material.extras   = ToGLTFValue(extras);

        model.materials.push_back(std::move(material));
    }

    if (bSkyMaterial)
        model.extensionsUsed.push_back(s_UnlitExtension);

    if (bEmissiveMaterial)
        model.extensionsUsed.push_back(s_EmissiveStrengthExtension);
}

//Lightmap atlases of the exported lightstyle layers: every layer a surface uses, or only layer 0 without
//BSPConversionOptions::bPreserveLightstyles.
static nlohmann::json AddLightmaps(const BSPConversionSource& source, const BSPLightmapAtlasLayout& atlas, CGLTFDocumentBuilder& document, ConversionStatistics& statistics)
{
    const auto& options = source.pRequest->options;
    const auto& world   = source.world;

    nlohmann::json lightmaps;
    lightmaps["fullbright"] = world.bFullbright;

    if (!options.bPreserveLightmaps || world.bFullbright || !atlas.pageCount)
    {
        lightmaps["present"] = false;
        return lightmaps;
    }

    const auto sampler = document.AddSampler(TINYGLTF_TEXTURE_FILTER_LINEAR, TINYGLTF_TEXTURE_FILTER_LINEAR, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);

    nlohmann::json layers = nlohmann::json::array();

    for (std::uint32_t layer = 0; layer < GoldSrcRender::BSPLightmapStyles; ++layer)
    {
        const bool bUsed = (world.usedStyleLayers >> layer) & 1;

        if (!bUsed || (layer && !options.bPreserveLightstyles))
            continue;

        std::vector<std::uint8_t> texels;
        BuildLightmapAtlas(world, layer, atlas, texels);

        constexpr std::uint32_t s_RGBChannels = 3;

        std::vector<std::uint8_t> png;

        if (!EncodePNG(texels.data(), atlas.width, atlas.height, s_RGBChannels, png))
            throw std::runtime_error("PNG encoding of lightmap layer " + std::to_string(layer) + " failed");

        const auto name    = "lightmap_layer" + std::to_string(layer);
        const auto image   = document.AddPNGImage(name, std::move(png));
        const auto texture = document.AddTexture(image, sampler, name);

        layers.push_back({{"layer", layer}, {"texture", texture}});
        ++statistics.lightmapCount;
    }

    statistics.lightmapPageCount = atlas.pageCount;

    lightmaps["present"]      = true;
    lightmaps["texCoord"]     = 1;
    lightmaps["encoding"]     = "rgb8";
    lightmaps["pageSize"]     = GoldSrcRender::BSPLightmapPageSize;
    lightmaps["pageCount"]    = atlas.pageCount;
    lightmaps["atlasColumns"] = atlas.columns;
    lightmaps["atlasRows"]    = atlas.rows;
    lightmaps["atlasSize"]    = nlohmann::json::array({atlas.width, atlas.height});
    lightmaps["layers"]       = std::move(layers);
    lightmaps["luxelSize"]    = BSPLuxelSize;

    return lightmaps;
}

static void AddMeshes(const BSPConversionSource& source, std::vector<BSPModelGeometry>& models, CGLTFDocumentBuilder& document, CConversionDiagnostics& diagnostics, ConversionStatistics& statistics)
{
    auto& gltf = document.GetModel();

    for (auto& geometry : models)
    {
        std::uint32_t replacedNormals = 0;

        for (std::uint32_t kindIndex = 0; kindIndex < BSPGeometryKindCount; ++kindIndex)
        {
            const auto kind       = static_cast<BSPGeometryKind>(kindIndex);
            auto&      primitives = geometry.primitives[kindIndex];

            if (primitives.empty())
                continue;

            tinygltf::Mesh mesh;
            mesh.name = "Model_" + std::to_string(geometry.model) + "_" + GetGeometryKindNodeName(kind);

            for (auto& primitive : primitives)
            {
                replacedNormals += NormalizePrimitiveNormals(primitive);
                AlignPrimitiveWinding(primitive);

                const auto vertexCount = static_cast<std::uint32_t>(primitive.positions.size());

                tinygltf::Primitive gltfPrimitive;
                gltfPrimitive.mode                     = TINYGLTF_MODE_TRIANGLES;
                gltfPrimitive.material                 = static_cast<int>(primitive.material);
                gltfPrimitive.attributes["POSITION"]   = document.AddVec3Accessor(primitive.positions, true);
                gltfPrimitive.attributes["NORMAL"]     = document.AddVec3Accessor(primitive.normals, false);
                gltfPrimitive.attributes["TEXCOORD_0"] = document.AddVec2Accessor(primitive.texcoords);

                if (!primitive.lightmapTexcoords.empty())
                    gltfPrimitive.attributes["TEXCOORD_1"] = document.AddVec2Accessor(primitive.lightmapTexcoords);

                gltfPrimitive.indices = document.AddIndexAccessor(primitive.indices, vertexCount);

                nlohmann::json extras;
                extras["goldsrc"]    = MakePrimitiveExtras(source, geometry.model, kind, primitive);
                gltfPrimitive.extras = ToGLTFValue(extras);

                mesh.primitives.push_back(std::move(gltfPrimitive));

                //Emitter extraction needs the corrected geometry after scene transforms have been built.
                if (!source.pRequest->options.bExportAreaLights || kind != BSPGeometryKind::Surface ||
                    gltf.materials[primitive.material].emissiveTexture.index < 0)
                    primitive = {};
            }

            geometry.meshes[kindIndex] = static_cast<int>(gltf.meshes.size());
            gltf.meshes.push_back(std::move(mesh));
            ++statistics.meshCount;
        }

        if (replacedNormals)
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::ZeroLengthNormal, "model " + std::to_string(geometry.model) + ": " + std::to_string(replacedNormals) + " vertices had a zero normal, replaced by +z");
    }
}

static int AddNode(tinygltf::Model& gltf, tinygltf::Node&& node)
{
    gltf.nodes.push_back(std::move(node));

    return static_cast<int>(gltf.nodes.size()) - 1;
}

//Child nodes of a model's meshes under parent; the meshes are shared by every node that shows the model.
static void AddModelMeshNodes(tinygltf::Model& gltf, const BSPModelGeometry& geometry, const char* szPrefix, int parent)
{
    for (std::uint32_t kind = 0; kind < BSPGeometryKindCount; ++kind)
    {
        if (geometry.meshes[kind] < 0)
            continue;

        tinygltf::Node node;
        node.name = std::string(szPrefix) + GetGeometryKindNodeName(static_cast<BSPGeometryKind>(kind));
        node.mesh = geometry.meshes[kind];

        const auto index = AddNode(gltf, std::move(node));
        gltf.nodes[parent].children.push_back(index);
    }
}

static std::string MakeEntityNodeName(const BSPConversionSource& source, std::uint32_t entity)
{
    char szIndex[s_NodeNameIndexDigits + 1 + sizeof(std::uint32_t) * CHAR_BIT]{};
    std::snprintf(szIndex, sizeof(szIndex), "%0*u", s_NodeNameIndexDigits, entity);

    bool bLatin1{};

    return "ent_" + std::string(szIndex) + "_" + MakeJSONString(source.entityInfos[entity].classname, bLatin1);
}

//Entity transform: the origin, and the rotation of the angles when the entity is drawn rotated by them.
static void SetEntityTransform(const BSPEntityInfo& info, tinygltf::Node& node)
{
    if (info.origin.x || info.origin.y || info.origin.z)
        node.translation = {info.origin.x, info.origin.y, info.origin.z};

    if (info.angleUsage != BSPEntityAngleUsage::Rotation)
        return;

    const auto rotation = MakeGoldSrcAnglesRotation(info.angles);

    if (!IsIdentityRotation(rotation))
        node.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
}

struct BSPSceneNodes
{
    std::uint32_t brushEntityNodes{};
    std::uint32_t pointEntityNodes{};

    //Entities showing each model.
    std::vector<std::vector<std::uint32_t>> modelEntities;

    //Original entity nodes only, -1 for omitted entities; generated light children have no entity identity.
    std::vector<int> entityNodes;
};

/*
====================
AddSceneNodes

BSP_Root turns GoldSrc coordinates into glTF's. Under it, BSP_World is the worldspawn entity and shows model 0;
Brush_Entities holds a node per entity whose model is an inline submodel, placed by its origin and angles, with child
nodes sharing the submodel's meshes; Point_Entities holds a node per other entity. Every entity of the lump has
exactly one node (unless the options leave out its kind), whose extras carry its identity and key values.
====================
*/
static BSPSceneNodes AddSceneNodes(const BSPConversionSource& source, const std::vector<BSPModelGeometry>& models, std::uint32_t worldspawn, CGLTFDocumentBuilder& document, CConversionDiagnostics& diagnostics)
{
    auto&       gltf    = document.GetModel();
    const auto& options = source.pRequest->options;

    BSPSceneNodes nodes;
    nodes.modelEntities.resize(models.size());
    nodes.entityNodes.resize(source.entities.size(), -1);

    tinygltf::Node root;
    root.name = "BSP_Root";

    const auto rotation = MakeGoldSrcToGLTFRotation();
    root.rotation       = {rotation.x, rotation.y, rotation.z, rotation.w};

    if (options.scaleFactor != 1.0f)
        root.scale = {options.scaleFactor, options.scaleFactor, options.scaleFactor};

    const auto rootIndex = AddNode(gltf, std::move(root));

    tinygltf::Node worldNode;
    worldNode.name = "BSP_World";

    nlohmann::json worldExtras;

    if (worldspawn < source.entities.size())
    {
        worldExtras["goldsrc"] = MakeEntityExtras(source, worldspawn, diagnostics);
        nodes.modelEntities[0].push_back(worldspawn);
    }
    else
        worldExtras["goldsrc"] = {{"schemaVersion", ExtrasSchemaVersion}, {"entityIndex", -1}, {"bspModelIndex", 0}};

    worldNode.extras = ToGLTFValue(worldExtras);

    const auto worldIndex = AddNode(gltf, std::move(worldNode));
    gltf.nodes[rootIndex].children.push_back(worldIndex);

    if (worldspawn < source.entities.size())
        nodes.entityNodes[worldspawn] = worldIndex;

    if (!models.empty())
        AddModelMeshNodes(gltf, models[0], "World_", worldIndex);

    tinygltf::Node brushGroup;
    brushGroup.name       = "Brush_Entities";
    const auto brushIndex = AddNode(gltf, std::move(brushGroup));
    gltf.nodes[rootIndex].children.push_back(brushIndex);

    tinygltf::Node pointGroup;
    pointGroup.name       = "Point_Entities";
    const auto pointIndex = AddNode(gltf, std::move(pointGroup));
    gltf.nodes[rootIndex].children.push_back(pointIndex);

    for (std::uint32_t entity = 0; entity < source.entities.size(); ++entity)
    {
        if (entity == worldspawn)
            continue;

        const auto& info   = source.entityInfos[entity];
        const bool  bBrush = info.modelKind == BSPEntityModelKind::Inline;

        if ((bBrush && !options.bPreserveBrushEntities) || (!bBrush && !options.bPreservePointEntities))
            continue;

        tinygltf::Node node;
        node.name = MakeEntityNodeName(source, entity);
        SetEntityTransform(info, node);

        nlohmann::json extras;
        extras["goldsrc"] = MakeEntityExtras(source, entity, diagnostics);
        node.extras       = ToGLTFValue(extras);

        const auto index          = AddNode(gltf, std::move(node));
        nodes.entityNodes[entity] = index;

        if (bBrush)
        {
            gltf.nodes[brushIndex].children.push_back(index);
            nodes.modelEntities[info.modelIndex].push_back(entity);

            const auto prefix = "Model_" + std::to_string(info.modelIndex) + "_";
            AddModelMeshNodes(gltf, models[info.modelIndex], prefix.c_str(), index);

            ++nodes.brushEntityNodes;
        }
        else
        {
            gltf.nodes[pointIndex].children.push_back(index);
            ++nodes.pointEntityNodes;
        }
    }

    tinygltf::Scene scene;
    scene.name  = "BSP";
    scene.nodes = {rootIndex};

    gltf.scenes.push_back(std::move(scene));
    gltf.defaultScene = 0;

    return nodes;
}

//Read exactly one, three or four whitespace-separated finite nonnegative numbers. GoldSrc light RGB is linear;
//keep its unscaled peak separate from color so HDR channels can be represented by glTF's bounded color multiplier.
static bool ParsePointLight(const BSPEntity& entity, tinygltf::Light& light)
{
    light.color        = {1, 1, 1};
    light.intensity    = s_DefaultPointLightBrightness;
    const auto* pValue = entity.FindValue("_light");

    if (!pValue)
        return true;

    std::array<double, s_PointLightMaxComponents> values{};
    std::size_t                                   count{};
    const char*                                   pText = pValue->c_str();

    for (;;)
    {
        while (std::isspace(static_cast<unsigned char>(*pText)))
            ++pText;

        if (!*pText)
            break;

        if (count == values.size())
            return false;

        char*      pEnd{};
        const auto value = std::strtod(pText, &pEnd);

        if (pEnd == pText || !std::isfinite(value) || value < 0 || (*pEnd && !std::isspace(static_cast<unsigned char>(*pEnd))))
            return false;

        values[count++] = value;
        pText           = pEnd;
    }

    if (count == 1)
    {
        light.intensity = values[0];
        return true;
    }

    if (count != s_PointLightColorComponents && count != s_PointLightMaxComponents)
        return false;

    const auto peak = *std::max_element(values.begin(), values.begin() + s_PointLightColorComponents);

    if (peak > 0)
    {
        for (std::size_t channel = 0; channel < s_PointLightColorComponents; ++channel)
            light.color[channel] = values[channel] / peak;
    }

    light.intensity = peak;

    if (count == s_PointLightMaxComponents)
        light.intensity *= values[s_PointLightColorComponents] / s_PointLightRGBNormalization;

    if (light.intensity == 0)
        light.color.assign(s_PointLightColorComponents, 1);

    return true;
}

/*
====================
AddPointLights

Only preserved point entities of class "light" gain an identity-transform child. The original node and metadata
stay intact. Switches and lightstyles do not change these constant lights. Compensate intensity for the root scale
explicitly: KHR_lights_punctual does not scale light intensity with node transforms. All validation finishes before
any output is sent to the caller; a malformed entity is warned and kept, while overflowing final intensity fails.
====================
*/
static bool AddPointLights(const BSPConversionSource& source, const BSPSceneNodes& nodes, CGLTFDocumentBuilder& document, CConversionDiagnostics& diagnostics, std::string& error)
{
    const auto& options = source.pRequest->options;

    if (options.pointLightIntensityScale == 0)
        return true;

    auto&        gltf           = document.GetModel();
    const double scale          = options.scaleFactor;
    const double intensityScale = options.pointLightIntensityScale * scale * scale;

    for (std::size_t entity = 0; entity < source.entities.size(); ++entity)
    {
        const auto  index = nodes.entityNodes[entity];
        const auto& info  = source.entityInfos[entity];

        if (index < 0 || info.classname != "light" || info.modelKind == BSPEntityModelKind::Inline)
            continue;

        tinygltf::Light light;

        if (!ParsePointLight(source.entities[entity], light))
        {
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidEntityValue, "entity " + std::to_string(entity) + ": key \"_light\" must contain 1, 3 or 4 finite nonnegative numbers; point light skipped");
            continue;
        }

        light.intensity *= intensityScale;

        if (!std::isfinite(light.intensity) || light.intensity > std::numeric_limits<float>::max())
        {
            error = "entity " + std::to_string(entity) + ": the scaled point light intensity is too large for a float";
            return false;
        }

        light.name = gltf.nodes[index].name + "_PointLight";
        light.type = "point";
        tinygltf::Node child;
        child.name  = light.name;
        child.light = static_cast<int>(gltf.lights.size());
        gltf.lights.push_back(std::move(light));
        const auto childIndex = AddNode(gltf, std::move(child));
        gltf.nodes[index].children.push_back(childIndex);
    }

    return true;
}

static nlohmann::json MakeVectorJSON(const Vector3& vector)
{
    return nlohmann::json::array({vector.x, vector.y, vector.z});
}

static std::string MakeOptionsKey(const BSPConversionOptions& options)
{
    std::uint32_t scaleBits{};
    std::memcpy(&scaleBits, &options.scaleFactor, sizeof(scaleBits));
    std::uint32_t emissiveBits{};
    std::memcpy(&emissiveBits, &options.emissiveScale, sizeof(emissiveBits));
    std::uint32_t pointLightBits{};
    std::memcpy(&pointLightBits, &options.pointLightIntensityScale, sizeof(pointLightBits));

    const bool flags[] = {options.bPreserveSkyGeometry, options.bPreserveBrushEntities, options.bPreservePointEntities, options.bPreserveLightmaps, options.bPreserveLightstyles,
                          options.bPreserveSurfaceMetadata, options.bPreserveEntityKeyValues, options.bEmbedTextures, options.bEmbedBSPVisibilityData, options.bEmbedBSPCollisionData};

    std::string key = options.format == OutputFormat::GLB ? "glb;" : "gltf;";

    for (const auto bFlag : flags)
        key.push_back(bFlag ? '1' : '0');

    char szScale[sizeof(scaleBits) * 2 + 1]{};
    std::snprintf(szScale, sizeof(szScale), "%08x", scaleBits);
    char szEmissive[sizeof(emissiveBits) * 2 + 1]{};
    std::snprintf(szEmissive, sizeof(szEmissive), "%08x", emissiveBits);
    char szPointLight[sizeof(pointLightBits) * 2 + 1]{};
    std::snprintf(szPointLight, sizeof(szPointLight), "%08x", pointLightBits);

    return key + ";scale=" + szScale + ";emissive=" + szEmissive + ";pointLight=" + szPointLight +
        ";areaLights=" + (options.bExportAreaLights ? "1" : "0") + ";areaBudget=" + std::to_string(options.maxAreaLightsPerGroup);
}

//Face counts of the statistics, and diagnostics for the faces that are not exported.
static void CountFaces(const std::vector<BSPModelGeometry>& models, ConversionStatistics& statistics, CConversionDiagnostics& diagnostics)
{
    for (const auto& geometry : models)
    {
        for (std::uint32_t kind = 0; kind < BSPGeometryKindCount; ++kind)
        {
            std::uint32_t faceCount = 0;

            for (const auto& primitive : geometry.primitives[kind])
                faceCount += static_cast<std::uint32_t>(primitive.faces.size());

            if (geometry.model == 0)
                statistics.worldFaceCount += faceCount;

            if (static_cast<BSPGeometryKind>(kind) == BSPGeometryKind::Sky)
                statistics.skyFaceCount += faceCount;
            else if (static_cast<BSPGeometryKind>(kind) == BSPGeometryKind::Water)
                statistics.waterFaceCount += faceCount;
        }

        statistics.degenerateFaceCount += static_cast<std::uint32_t>(geometry.degenerateFaces.size());

        const auto label = "model " + std::to_string(geometry.model) + ": ";

        if (!geometry.degenerateFaces.empty())
            diagnostics.Report(DiagnosticSeverity::Info, DiagnosticCode::DegenerateFace, label + std::to_string(geometry.degenerateFaces.size()) + " faces have fewer than 3 edges and no area; they are listed in the model's degenerateFaces");

        if (!geometry.unexportedFaces.empty())
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::UnexportedFace, label + std::to_string(geometry.unexportedFaces.size()) + " faces are neither drawn, water nor sky surfaces; they are listed in the model's unexportedFaces");

        if (!geometry.omittedSkyFaces.empty())
            diagnostics.Report(DiagnosticSeverity::Info, DiagnosticCode::UnexportedFace, label + std::to_string(geometry.omittedSkyFaces.size()) + " sky faces are left out by the options; they are listed in the model's omittedSkyFaces");
    }
}

//The models table of the asset's extras: bounds, origin, face range, meshes, the entities showing each model and its
//faces that are not exported. Submodels no entity shows are reported; their meshes stay in the document.
static nlohmann::json MakeModelTable(const BSPConversionSource& source, const std::vector<BSPModelGeometry>& models, const BSPSceneNodes& nodes, ConversionStatistics& statistics, CConversionDiagnostics& diagnostics)
{
    nlohmann::json table = nlohmann::json::array();

    for (const auto& geometry : models)
    {
        const auto  data          = source.reader.ReadModel(geometry.model);
        const auto& modelEntities = nodes.modelEntities[geometry.model];

        nlohmann::json model;
        model["index"]     = geometry.model;
        model["mins"]      = MakeVectorJSON(data.mins);
        model["maxs"]      = MakeVectorJSON(data.maxs);
        model["origin"]    = MakeVectorJSON(data.origin);
        model["firstFace"] = data.firstFace;
        model["faceCount"] = data.faceCount;
        model["entities"]  = modelEntities;

        nlohmann::json meshes;

        for (std::uint32_t kind = 0; kind < BSPGeometryKindCount; ++kind)
            meshes[GetGeometryKindName(static_cast<BSPGeometryKind>(kind))] = geometry.meshes[kind];

        model["meshes"] = std::move(meshes);

        if (!geometry.degenerateFaces.empty())
            model["degenerateFaces"] = geometry.degenerateFaces;

        if (!geometry.unexportedFaces.empty())
            model["unexportedFaces"] = geometry.unexportedFaces;

        if (!geometry.omittedSkyFaces.empty())
            model["omittedSkyFaces"] = geometry.omittedSkyFaces;

        if (geometry.model && modelEntities.empty())
        {
            ++statistics.unreferencedSubmodelCount;
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::UnreferencedSubmodel, "model " + std::to_string(geometry.model) + " is shown by no entity; its meshes are kept outside the scene");
        }

        table.push_back(std::move(model));
    }

    return table;
}

static void ConvertBSPInternal(const BSPConversionRequestView& request, CConversionDiagnostics& diagnostics, BSPConversionSummary& result)
{
    std::string error;

    if (!ValidateOptions(request.options, error))
        return Fail(result, ConversionError::InvalidRequest, error);

    if (!request.asset.pFile || !request.asset.fileBytes)
        return Fail(result, ConversionError::InvalidRequest, "the request has no BSP file");

    if (!request.pOutputSink)
        return Fail(result, ConversionError::InvalidRequest, "the request has no output sink");

    if (!ValidRequestString(request.szResourceName) || !ValidRequestString(request.szSourcePath))
        return Fail(result, ConversionError::InvalidRequest, "the resource name or the source path is longer than " + std::to_string(MaxConversionStringLength) + " bytes");

    const auto resourceName = GetResourceName(request);

    if (!IsValidResourceDirectory(resourceName))
        return Fail(result, ConversionError::InvalidRequest, "the resource name \"" + resourceName + "\" cannot name the asset directory");

    static const char s_InvalidBSPMessage[] = "the BSP file failed validation (truncated file, lump out of range, invalid index or face)";

    BSPConversionSource source;
    source.pRequest = &request;

    std::vector<std::uint32_t> miptexSlots;

    if (!ScanBSPMiptexSlots(request.asset.pFile, request.asset.fileBytes, miptexSlots))
        return Fail(result, ConversionError::InvalidBSP, s_InvalidBSPMessage);

    if (!BindEveryMiptex(source, miptexSlots, error))
        return Fail(result, ConversionError::InvalidRequest, error);

    auto asset         = request.asset;
    asset.pTextures    = source.bindings.data();
    asset.textureCount = static_cast<std::uint32_t>(source.bindings.size());

    //The whole file is checked before anything is read from it, with the bindings the conversion uses: lump table,
    //record sizes, every index and offset, face extents, lightmap ranges, the world tree and every visibility row (see
    //GoldSrcRender::ValidateBSPFile).
    auto code = GoldSrcRender::ValidateBSPAsset(asset);

    if (!GoldSrcRender::Succeeded(code))
        return Fail(result, ConversionError::InvalidBSP, s_InvalidBSPMessage);

    source.reader.Open(request.asset.pFile, request.asset.fileBytes, request.asset.layout);

    code = GoldSrcRender::BuildBSPWorld(asset, source.world);

    if (code == GoldSrcRender::ErrorCode::LimitExceeded)
        return Fail(result, ConversionError::LimitExceeded, "the lightmaps need more than " + std::to_string(GoldSrcRender::BSPMaxLightmapPages) + " pages");

    if (!GoldSrcRender::Succeeded(code))
        return Fail(result, ConversionError::InvalidBSP, "the BSP world could not be built");

    if (!ParseBSPEntityLump(source.reader.ReadEntityText(), source.entities, error))
        return Fail(result, ConversionError::InvalidEntityLump, "entity lump: " + error);

    ReadEntities(source, diagnostics);

    const auto worldspawn = FindWorldspawn(source, diagnostics);
    const auto atlas      = MakeLightmapAtlasLayout(request.options.bPreserveLightmaps && !source.world.bFullbright ? source.world.lightmapPages : 0);

    CBSPMaterialRegistry          materials;
    std::vector<BSPModelGeometry> models;
    BuildBSPModelGeometry(source, atlas, materials, models);

    auto& statistics = result.statistics;
    CountFaces(models, statistics, diagnostics);

    const auto fileName = resourceName + (request.options.format == OutputFormat::GLB ? ".glb" : ".gltf");

    CGLTFDocumentBuilder document(request.options.bEmbedTextures, s_ExternalImageDirectory);
    CBSPTextureConverter textures(source, document, diagnostics);

    AddMaterials(source, materials, textures, document);
    textures.RetainEmissiveImages(document.GetModel());

    const auto lightmaps = AddLightmaps(source, atlas, document, statistics);

    AddMeshes(source, models, document, diagnostics, statistics);

    const auto nodes = AddSceneNodes(source, models, worldspawn, document, diagnostics);

    if (!AddPointLights(source, nodes, document, diagnostics, error))
        return Fail(result, ConversionError::InvalidRequest, error);

    statistics.modelCount           = static_cast<std::uint32_t>(models.size());
    statistics.brushModelCount      = models.empty() ? 0 : statistics.modelCount - 1;
    statistics.entityCount          = static_cast<std::uint32_t>(source.entities.size());
    statistics.brushEntityNodeCount = nodes.brushEntityNodes;
    statistics.pointEntityNodeCount = nodes.pointEntityNodes;
    statistics.materialCount        = static_cast<std::uint32_t>(materials.GetKeys().size());

    nlohmann::json texInfos = nlohmann::json::array();

    for (std::uint32_t i = 0; i < source.reader.GetRecordCount(GoldSrcRender::BSPTexInfoLump, BSPTexInfoBytes); ++i)
    {
        const auto texInfo = source.reader.ReadTexInfo(i);
        texInfos.push_back(nlohmann::json::array({texInfo.miptex, texInfo.flags}));
    }

    bool bLatin1{};

    nlohmann::json goldsrc;
    goldsrc["schemaVersion"]    = ExtrasSchemaVersion;
    goldsrc["converterVersion"] = ConverterVersion;
    goldsrc["assetType"]        = "BSP";
    goldsrc["sourceFormat"]     = request.asset.layout == GoldSrcRender::BSPLayout::BlueShift ? "BSP30_BlueShift" : "BSP30";
    goldsrc["coordinateSystem"] = "GoldSrc_Z_UP";
    goldsrc["unitScale"]        = request.options.scaleFactor;
    goldsrc["resourceName"]     = resourceName;

    if (request.szSourcePath && *request.szSourcePath)
        goldsrc["sourcePath"] = MakeJSONString(request.szSourcePath, bLatin1);

    goldsrc["faceRecordFields"] = s_FaceRecordFields;
    goldsrc["worldspawnEntity"] = worldspawn < source.entities.size() ? nlohmann::json(worldspawn) : nlohmann::json(-1);
    goldsrc["entityCount"]      = source.entities.size();
    goldsrc["models"]           = MakeModelTable(source, models, nodes, statistics, diagnostics);
    goldsrc["texInfos"]         = std::move(texInfos);
    goldsrc["lightmaps"]        = lightmaps;

    //The sky geometry is kept whatever the skybox images, which only the resolver can provide.
    if (worldspawn < source.entities.size())
    {
        if (const auto* pSkyName = source.entities[worldspawn].FindValue("skyname"))
        {
            nlohmann::json sky;
            sky["skyname"]   = MakeJSONString(*pSkyName, bLatin1);
            sky["faceOrder"] = nlohmann::json::array({"rt", "bk", "lf", "ft", "up", "dn"});

            if (request.pTextureResolver && !pSkyName->empty())
                sky["faceTextures"] = textures.GetSkyboxTextures(*pSkyName);

            goldsrc["sky"] = std::move(sky);
        }
    }

    const auto spatial = AppendBSPSpatialData(source, document, statistics);

    if (!spatial.is_null())
        goldsrc["spatialData"] = spatial;

    statistics.textureCount        = textures.GetTextureCount() + statistics.lightmapCount;
    statistics.missingTextureCount = textures.GetMissingCount();

    CSHA256 sourceHash;
    sourceHash.Update(request.asset.pFile, request.asset.fileBytes);

    //The lightmaps derive from the BSP itself; the texture digest covers the resolved images only.
    const auto sourceDigest  = sourceHash.FinishHex();
    const auto textureDigest = textures.FinishHash();
    const auto cacheKey      = "bsp:" + sourceDigest + ":" + textureDigest + ":converter" + std::to_string(ConverterVersion) + ":schema" + std::to_string(ExtrasSchemaVersion) + ":" + MakeOptionsKey(request.options);

    if (sourceDigest.size() >= SHA256HexCapacity || textureDigest.size() >= SHA256HexCapacity || cacheKey.size() >= CacheKeyCapacity)
        return Fail(result, ConversionError::InternalError, "the digests or the cache key do not fit the summary");

    CopyTruncatedString(result.szSourceHash, sourceDigest);
    CopyTruncatedString(result.szTextureHash, textureDigest);
    CopyTruncatedString(result.szCacheKey, cacheKey);

    goldsrc["sourceHash"]  = sourceDigest;
    goldsrc["textureHash"] = textureDigest;

    auto& gltf           = document.GetModel();
    gltf.asset.version   = "2.0";
    gltf.asset.generator = std::string(s_GeneratorName) + " " + std::to_string(ConverterVersion);

    nlohmann::json assetExtras;
    assetExtras["goldsrc"] = std::move(goldsrc);
    gltf.asset.extras      = ToGLTFValue(assetExtras);

    std::vector<std::uint8_t> file;

    nlohmann::json areaLights;
    const auto     areaError = BuildBSPAreaLights(source, models, textures, gltf, areaLights, error);
    if (areaError != ConversionError::None)
        return Fail(result, areaError, error);

    if (!document.Write(request.options.format, file, error))
        return Fail(result, ConversionError::InternalError, error);

    CSHA256 modelHash;
    modelHash.Update(file.data(), file.size());
    areaLights["modelFile"]        = fileName;
    areaLights["modelSha256"]      = modelHash.FinishHex();
    areaLights["cacheKey"]         = cacheKey;
    areaLights["converterVersion"] = ConverterVersion;
    const auto areaText            = areaLights.dump(2);

    //Everything goes into the asset directory, the render manifest beside it, last: it is only written once the files
    //it names have been accepted.
    const auto assetDirectory      = resourceName + "/";
    const auto assetPath           = assetDirectory + fileName;
    const auto lightProbeDirectory = assetDirectory + resourceName + s_LightProbeDirectoryExtension;
    const auto areaPath            = lightProbeDirectory + s_AreaLightFileName;
    const auto manifestPath        = resourceName + s_ManifestExtension;
    const auto manifestText        = MakeRenderManifest(request.options, assetDirectory, assetPath, lightProbeDirectory, sourceDigest, !document.GetExternalFiles().empty()).dump(2);

    if (!request.pOutputSink->WriteFile(assetPath.c_str(), file.data(), file.size()))
        return Fail(result, ConversionError::OutputFailed, "the output sink rejected " + assetPath);

    for (const auto& external : document.GetExternalFiles())
    {
        const auto externalPath = assetDirectory + external.relativePath;

        if (!request.pOutputSink->WriteFile(externalPath.c_str(), external.bytes.data(), external.bytes.size()))
            return Fail(result, ConversionError::OutputFailed, "the output sink rejected " + externalPath);
    }

    if (!request.pOutputSink->WriteFile(areaPath.c_str(), reinterpret_cast<const std::uint8_t*>(areaText.data()), areaText.size()))
        return Fail(result, ConversionError::OutputFailed, "the output sink rejected " + areaPath);

    if (!request.pOutputSink->WriteFile(manifestPath.c_str(), reinterpret_cast<const std::uint8_t*>(manifestText.data()), manifestText.size()))
        return Fail(result, ConversionError::OutputFailed, "the output sink rejected " + manifestPath);

    result.error = ConversionError::None;
}

BSPConversionSummary ConvertBSP(const BSPConversionRequestView& request) noexcept
{
    BSPConversionSummary   summary;
    CConversionDiagnostics diagnostics(request.pDiagnosticSink);

    summary.converterVersion    = ConverterVersion;
    summary.extrasSchemaVersion = ExtrasSchemaVersion;

    try
    {
        ConvertBSPInternal(request, diagnostics, summary);
    }
    catch (const std::bad_alloc&)
    {
        Fail(summary, ConversionError::OutOfMemory, "out of memory");
    }
    catch (const std::exception& exception)
    {
        Fail(summary, ConversionError::InternalError, exception.what());
    }
    catch (...)
    {
        Fail(summary, ConversionError::InternalError, "unknown exception");
    }

    summary.warningCount = diagnostics.GetCount();

    return summary;
}
} // namespace GoldSrcAssetConverter
