#include "BSPConverterInternal.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>

namespace GoldSrcAssetConverter
{
//The sky of a map whose worldspawn names none (sv_skyname's default), and of a map none of whose named sky images load
//(R_LoadSkys).
constexpr char s_DefaultSkyName[] = "desert";

//The engine draws the sky box centered on the view, its faces farDistance * 0.57735 away (R_DrawSkyBox), farDistance
//being sv_zmax, 4096 by default. The exported box has that size, in GoldSrc units.
constexpr double s_DefaultFarDistance  = 4096;
constexpr double s_SkyBoxDistanceScale = 0.57735;
constexpr double s_SkyBoxHalfExtent    = s_DefaultFarDistance * s_SkyBoxDistanceScale;

constexpr char          s_SkyBoxName[]     = "Sky_Box";
constexpr char          s_UnlitExtension[] = "KHR_materials_unlit";
constexpr double        s_MissingFaceColor = 0;
constexpr std::uint32_t s_FaceCorners      = 4;
constexpr double        s_HalfTexel        = 0.5;
constexpr std::uint8_t  s_OpaqueAlpha      = 255;

using Direction = std::array<double, 3>;

//A face of a cube seen from its center: the direction it faces and the directions in which its image's columns (right)
//and rows (down) advance. The direction forward + s * right + t * down, s and t in [-1, 1], shows the image at
//((s + 1) / 2, (t + 1) / 2), texel (0, 0) being the top-left one.
struct CubeFace
{
    Direction forward;
    Direction right;
    Direction down;
};

//The faces of the engine's sky box in GoldSrc axes, in GoldSrcRender::SkyFace order (rt, bk, lf, ft, up, dn), each
//image seen from inside the box as R_DrawSkyBox maps it.
static const CubeFace s_SkyBoxFaces[GoldSrcRender::SkyFaceCount] = {
    {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
    {{0, -1, 0}, {-1, 0, 0}, {0, 0, -1}},
    {{0, 0, 1}, {0, -1, 0}, {1, 0, 0}},
    {{0, 0, -1}, {0, -1, 0}, {-1, 0, 0}}};

//The faces of a Direct3D cube map (+x, -x, +y, -y, +z, -z), whose texel addressing Vulkan shares, in the axes of the
//converted scene: BSP_Root turns GoldSrc's (x, y, z) into (x, z, -y), so +y is up.
static const CubeFace s_CubeMapFaces[GoldSrcRender::SkyFaceCount] = {
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},
    {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},
    {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}}};

//DDS file layout: the magic, DDS_HEADER with its DDS_PIXELFORMAT, and the DDS_HEADER_DXT10 extension that names the
//DXGI format and marks the texture as a cube map.
constexpr std::uint32_t s_DDSMagic                = 0x20534444; //"DDS "
constexpr std::uint32_t s_DDSHeaderBytes          = 124;
constexpr std::uint32_t s_DDSPixelFormatBytes     = 32;
constexpr std::uint32_t s_DDSHeaderReservedWords  = 11;
constexpr std::uint32_t s_DDSPixelFormatMaskWords = 5;
constexpr std::uint32_t s_DDSCapsReservedWords    = 3;
constexpr std::uint32_t s_DDSFlagCaps             = 0x1;
constexpr std::uint32_t s_DDSFlagHeight           = 0x2;
constexpr std::uint32_t s_DDSFlagWidth            = 0x4;
constexpr std::uint32_t s_DDSFlagPitch            = 0x8;
constexpr std::uint32_t s_DDSFlagPixelFormat      = 0x1000;
constexpr std::uint32_t s_DDSFlagMipMapCount      = 0x20000;
constexpr std::uint32_t s_DDSPixelFormatFourCC    = 0x4;
constexpr std::uint32_t s_DDSFourCCDX10           = 0x30315844; //"DX10"
constexpr std::uint32_t s_DDSCapsComplex          = 0x8;
constexpr std::uint32_t s_DDSCapsTexture          = 0x1000;
constexpr std::uint32_t s_DDSCaps2CubeMapAllFaces = 0xFE00;
constexpr std::uint32_t s_DXGIFormatRGBA8SRGB     = 29;
constexpr std::uint32_t s_D3D10ResourceTexture2D  = 3;
constexpr std::uint32_t s_D3D10ResourceMiscCube   = 0x4;
constexpr std::uint32_t s_DDSCubeCount            = 1;
constexpr std::uint32_t s_DDSMipCount             = 1;
constexpr std::uint32_t s_DDSAlphaModeUnknown     = 0;

static void AppendUInt32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (std::uint32_t i = 0; i < sizeof(value); ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * CHAR_BIT)));
}

static void AppendZeros(std::vector<std::uint8_t>& bytes, std::uint32_t words)
{
    for (std::uint32_t i = 0; i < words; ++i)
        AppendUInt32(bytes, 0);
}

static double Dot(const Direction& a, const Direction& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static Direction ToGoldSrc(const Direction& scene)
{
    return {scene[0], -scene[2], scene[1]};
}

//Image coordinate of s (or t) in [-1, 1] on a face size texels wide, the nearest texel.
static std::uint32_t FaceTexel(double s, std::uint32_t size)
{
    const auto texel = static_cast<std::int64_t>(std::floor((s + 1) / 2 * size));

    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(texel, 0, static_cast<std::int64_t>(size) - 1));
}

//The texel of the sky box that a GoldSrc direction shows: the face it points at most, black when it has no image.
static const std::uint8_t* SampleSkyBox(const BSPSkyboxImages& images, const Direction& direction)
{
    static const std::uint8_t s_Black[GoldSrcRender::RGBABytes] = {0, 0, 0, s_OpaqueAlpha};

    std::uint32_t face = 0;

    for (std::uint32_t i = 1; i < GoldSrcRender::SkyFaceCount; ++i)
    {
        if (Dot(direction, s_SkyBoxFaces[i].forward) > Dot(direction, s_SkyBoxFaces[face].forward))
            face = i;
    }

    const auto& image = images.images[face];

    if (images.textures[face] < 0)
        return s_Black;

    const auto distance = Dot(direction, s_SkyBoxFaces[face].forward);
    const auto x        = FaceTexel(Dot(direction, s_SkyBoxFaces[face].right) / distance, image.width);
    const auto y        = FaceTexel(Dot(direction, s_SkyBoxFaces[face].down) / distance, image.height);

    return image.texels.data() + (static_cast<std::size_t>(y) * image.width + x) * GoldSrcRender::RGBABytes;
}

/*
====================
MakeSkyEnvironmentMap

Every texel of each face of the cube map shows the sky box texel its center's direction points at: the direction in
the converted scene's axes, turned back into GoldSrc's, picks the sky box face and its image coordinates. The scene
axes are those of BSP_Root's rotation, so a renderer that samples the cube map with a scene direction sees what the
engine draws in the same GoldSrc direction. Faces of equal size map texel to texel; other sizes take the nearest texel.
====================
*/
std::vector<std::uint8_t> MakeSkyEnvironmentMap(const BSPSkyboxImages& images)
{
    std::uint32_t size = 0;

    for (std::uint32_t i = 0; i < GoldSrcRender::SkyFaceCount; ++i)
    {
        if (images.textures[i] >= 0)
            size = std::max({size, images.images[i].width, images.images[i].height});
    }

    if (!size)
        return {};

    const auto pitch = size * GoldSrcRender::RGBABytes;

    std::vector<std::uint8_t> file;
    file.reserve(sizeof(s_DDSMagic) + s_DDSHeaderBytes + static_cast<std::size_t>(pitch) * size * GoldSrcRender::SkyFaceCount);

    AppendUInt32(file, s_DDSMagic);
    AppendUInt32(file, s_DDSHeaderBytes);
    AppendUInt32(file, s_DDSFlagCaps | s_DDSFlagHeight | s_DDSFlagWidth | s_DDSFlagPitch | s_DDSFlagPixelFormat | s_DDSFlagMipMapCount);
    AppendUInt32(file, size);
    AppendUInt32(file, size);
    AppendUInt32(file, pitch);
    AppendUInt32(file, 0);
    AppendUInt32(file, s_DDSMipCount);
    AppendZeros(file, s_DDSHeaderReservedWords);
    AppendUInt32(file, s_DDSPixelFormatBytes);
    AppendUInt32(file, s_DDSPixelFormatFourCC);
    AppendUInt32(file, s_DDSFourCCDX10);
    AppendZeros(file, s_DDSPixelFormatMaskWords);
    AppendUInt32(file, s_DDSCapsComplex | s_DDSCapsTexture);
    AppendUInt32(file, s_DDSCaps2CubeMapAllFaces);
    AppendZeros(file, s_DDSCapsReservedWords);
    AppendUInt32(file, s_DXGIFormatRGBA8SRGB);
    AppendUInt32(file, s_D3D10ResourceTexture2D);
    AppendUInt32(file, s_D3D10ResourceMiscCube);
    AppendUInt32(file, s_DDSCubeCount);
    AppendUInt32(file, s_DDSAlphaModeUnknown);

    for (const auto& face : s_CubeMapFaces)
    {
        for (std::uint32_t y = 0; y < size; ++y)
        {
            const auto t = (2.0 * y + 1) / size - 1;

            for (std::uint32_t x = 0; x < size; ++x)
            {
                const auto s = (2.0 * x + 1) / size - 1;

                Direction direction;

                for (std::size_t c = 0; c < direction.size(); ++c)
                    direction[c] = face.forward[c] + s * face.right[c] + t * face.down[c];

                const auto* pTexel = SampleSkyBox(images, ToGoldSrc(direction));
                file.insert(file.end(), pTexel, pTexel + GoldSrcRender::RGBABytes - 1);
                file.push_back(s_OpaqueAlpha);
            }
        }
    }

    return file;
}

//An unlit material per face showing its image, black without one.
static int AddSkyBoxMaterial(const BSPSkyboxImages& images, std::uint32_t face, tinygltf::Model& gltf)
{
    tinygltf::Material material;
    material.name                                 = std::string(s_SkyBoxName) + "_" + SkyboxFaceSuffixes[face];
    material.pbrMetallicRoughness.metallicFactor  = 0;
    material.pbrMetallicRoughness.roughnessFactor = 1;
    material.extensions[s_UnlitExtension]         = tinygltf::Value(tinygltf::Value::Object());

    if (images.textures[face] >= 0)
    {
        material.pbrMetallicRoughness.baseColorTexture.index    = images.textures[face];
        material.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
    }
    else
        material.pbrMetallicRoughness.baseColorFactor = {s_MissingFaceColor, s_MissingFaceColor, s_MissingFaceColor, 1.0};

    nlohmann::json extras;
    extras["goldsrc"] = {{"schemaVersion", ExtrasSchemaVersion}, {"surfaceType", "skybox"}, {"skyFace", SkyboxFaceSuffixes[face]}, {"missingTexture", images.textures[face] < 0}};
    material.extras   = ToGLTFValue(extras);

    gltf.materials.push_back(std::move(material));

    return static_cast<int>(gltf.materials.size()) - 1;
}

//A quad per face, seen from inside: counterclockwise from the center, its normal pointing there. Texture coordinates
//keep half a texel from the image's edges, as the engine's do, so that no face filters into the next.
static tinygltf::Primitive MakeSkyBoxFace(const BSPSkyboxImages& images, std::uint32_t face, int material, CGLTFDocumentBuilder& document)
{
    const auto& axes  = s_SkyBoxFaces[face];
    const auto& image = images.images[face];

    const double insetS = images.textures[face] >= 0 ? s_HalfTexel / image.width : 0;
    const double insetT = images.textures[face] >= 0 ? s_HalfTexel / image.height : 0;

    //Top-left, bottom-left, bottom-right and top-right corners as (s, t).
    static const double s_Corners[s_FaceCorners][2] = {{-1, -1}, {-1, 1}, {1, 1}, {1, -1}};

    std::vector<GoldSrcRender::Vector3> positions;
    std::vector<GoldSrcRender::Vector3> normals;
    std::vector<GoldSrcRender::Vector2> texcoords;

    for (const auto& corner : s_Corners)
    {
        GoldSrcRender::Vector3 position;
        position.x = static_cast<float>(s_SkyBoxHalfExtent * (axes.forward[0] + corner[0] * axes.right[0] + corner[1] * axes.down[0]));
        position.y = static_cast<float>(s_SkyBoxHalfExtent * (axes.forward[1] + corner[0] * axes.right[1] + corner[1] * axes.down[1]));
        position.z = static_cast<float>(s_SkyBoxHalfExtent * (axes.forward[2] + corner[0] * axes.right[2] + corner[1] * axes.down[2]));
        positions.push_back(position);

        GoldSrcRender::Vector3 normal;
        normal.x = static_cast<float>(-axes.forward[0]);
        normal.y = static_cast<float>(-axes.forward[1]);
        normal.z = static_cast<float>(-axes.forward[2]);
        normals.push_back(normal);

        GoldSrcRender::Vector2 texcoord;
        texcoord.x = static_cast<float>(corner[0] < 0 ? insetS : 1 - insetS);
        texcoord.y = static_cast<float>(corner[1] < 0 ? insetT : 1 - insetT);
        texcoords.push_back(texcoord);
    }

    const std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3};

    tinygltf::Primitive primitive;
    primitive.mode                     = TINYGLTF_MODE_TRIANGLES;
    primitive.material                 = material;
    primitive.attributes["POSITION"]   = document.AddVec3Accessor(positions, true);
    primitive.attributes["NORMAL"]     = document.AddVec3Accessor(normals, false);
    primitive.attributes["TEXCOORD_0"] = document.AddVec2Accessor(texcoords);
    primitive.indices                  = document.AddIndexAccessor(indices, s_FaceCorners);

    return primitive;
}

//The Sky_Box node under the scene's root, a cube centered on the map's origin whose faces show the images; returns
//extras.goldsrc.sky.box.
static nlohmann::json AddSkyBoxNode(const BSPSkyboxImages& images, CGLTFDocumentBuilder& document, ConversionStatistics& statistics)
{
    auto& gltf = document.GetModel();

    tinygltf::Mesh mesh;
    mesh.name = s_SkyBoxName;

    for (std::uint32_t face = 0; face < GoldSrcRender::SkyFaceCount; ++face)
        mesh.primitives.push_back(MakeSkyBoxFace(images, face, AddSkyBoxMaterial(images, face, gltf), document));

    const auto meshIndex = static_cast<int>(gltf.meshes.size());
    gltf.meshes.push_back(std::move(mesh));
    ++statistics.meshCount;

    if (std::find(gltf.extensionsUsed.begin(), gltf.extensionsUsed.end(), s_UnlitExtension) == gltf.extensionsUsed.end())
        gltf.extensionsUsed.push_back(s_UnlitExtension);

    tinygltf::Node node;
    node.name = s_SkyBoxName;
    node.mesh = meshIndex;

    const auto nodeIndex = static_cast<int>(gltf.nodes.size());
    const auto rootIndex = gltf.scenes[gltf.defaultScene].nodes[0];
    gltf.nodes.push_back(std::move(node));
    gltf.nodes[rootIndex].children.push_back(nodeIndex);

    return {{"node", nodeIndex}, {"mesh", meshIndex}, {"halfExtent", s_SkyBoxHalfExtent}};
}

/*
====================
AddSkyBox

The engine loads a sky box for a map with a texture named sky* (Mod_LoadTextures), from the images
gfx/env/<skyname><suffix> of the worldspawn's skyname, of "desert" without one, and of "desert" again when no face of
the named sky loads; a face without image is black (R_LoadSkys). The resolver stands for the image files. Renderers
that draw the sky around the view sample the cube map in the scene's axes. The Sky_Box node, for viewers that show
the glTF as it stands, is a child of BSP_Root, beside BSP_World, so that it is not part of the world: it is a cube
centered on the map's origin with the box's default size, whose faces show the images as R_DrawSkyBox does.
====================
*/
BSPSkyBox AddSkyBox(const BSPConversionSource& source, std::uint32_t worldspawn, const std::string& environmentMapName, CBSPTextureConverter& textures, CGLTFDocumentBuilder& document, ConversionStatistics& statistics)
{
    BSPSkyBox sky;

    const auto& request  = *source.pRequest;
    const auto& miptexes = source.reader.GetMiptexes();

    const bool bSkyTexture = std::any_of(miptexes.begin(), miptexes.end(), [](const BSPRawMiptex& miptex) {
        return miptex.bPresent && IsBSPSkyTextureName(miptex.name);
    });

    const auto* pSkyName = worldspawn < source.entities.size() ? source.entities[worldspawn].FindValue("skyname") : nullptr;

    if (!bSkyTexture && !pSkyName)
        return sky;

    const std::string skyName = pSkyName ? *pSkyName : s_DefaultSkyName;

    bool bLatin1{};
    sky.extras["skyname"]   = MakeJSONString(skyName, bLatin1);
    sky.extras["faceOrder"] = SkyboxFaceSuffixes;

    if (!bSkyTexture || !request.pTextureResolver)
        return sky;

    auto images = textures.GetSkyboxImages(skyName);

    if (!images.HasImage() && skyName != s_DefaultSkyName)
        images = textures.GetSkyboxImages(s_DefaultSkyName);

    sky.extras["imageSkyname"] = MakeJSONString(images.skyName, bLatin1);
    sky.extras["faceTextures"] = images.textures;

    const bool bEnvironmentMap = request.options.bExportSkyEnvironmentMap && images.HasImage();
    const bool bSkyBoxGeometry = request.options.bExportSkyBoxGeometry && images.HasImage();

    if (bSkyBoxGeometry)
        sky.extras["box"] = AddSkyBoxNode(images, document, statistics);

    if (bEnvironmentMap)
    {
        sky.extras["environmentMap"] = environmentMapName;
        sky.environmentMap           = MakeSkyEnvironmentMap(images);
    }

    return sky;
}
} // namespace GoldSrcAssetConverter
