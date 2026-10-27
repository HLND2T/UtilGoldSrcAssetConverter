//Tests of UtilGoldSrcAssetConverter's BSP conversion. The converted documents are read back by an independent reader
//(the GLB container and JSON parsed here with nlohmann::json, accessors decoded by hand), so that extras.goldsrc and
//the binary data are checked as a custom runtime reader would see them. Every converted file is also written to the
//output directory given on the command line for the glTF-Validator and DiligentTools loader tests.
#include "BSPConverterTestFile.h"
#include "GoldSrcAssetConverterCore.h"
#include "BSPConverterInternal.h"
#include "TinyGLTF.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace GoldSrcConverterTest;
using namespace GoldSrcAssetConverter;

using json = nlohmann::json;

static std::filesystem::path s_OutputDirectory;

static void Require(bool bCondition, const char* pMessage)
{
    if (!bCondition)
    {
        std::cerr << "FAILED: " << pMessage << '\n';
        std::exit(EXIT_FAILURE);
    }
}

//Tolerances of positions reconstructed through node transforms, and of unit vectors.
constexpr double s_PositionTolerance = 1e-3;
constexpr double s_UnitTolerance     = 1e-5;

//GLB container constants (glTF 2.0, section 4.4).
constexpr std::uint32_t s_GLBHeaderBytes = 12;
constexpr std::uint32_t s_GLBChunkHeader = 8;
constexpr std::uint32_t s_GLBChunkJSON   = 0x4E4F534A;
constexpr std::uint32_t s_GLBChunkBIN    = 0x004E4942;
constexpr std::uint32_t s_GLBMagic       = 0x46546C67;

//glTF component types and modes the converter writes.
constexpr int s_ComponentUnsignedShort = 5123;
constexpr int s_ComponentUnsignedInt   = 5125;
constexpr int s_ComponentFloat         = 5126;
constexpr int s_ModeTriangles          = 4;

//The directory of the external images, beside the asset.
constexpr char s_ExternalImageDirectory[] = "textures/";

static void WriteOutput(const std::string& name, const std::vector<std::uint8_t>& bytes)
{
    if (s_OutputDirectory.empty())
        return;

    const auto path = s_OutputDirectory / name;
    std::filesystem::create_directories(path.parent_path());

    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(stream.good(), "write a test output");
}

static std::uint32_t ReadUInt32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));

    return value;
}

//A converted document as a custom reader sees it.
struct Document
{
    json                      gltf;
    std::vector<std::uint8_t> bin;

    const json& Goldsrc() const
    {
        return gltf["asset"]["extras"]["goldsrc"];
    }
};

static Document ParseGLB(const std::vector<std::uint8_t>& file)
{
    Require(file.size() >= s_GLBHeaderBytes + s_GLBChunkHeader, "the GLB holds a header and a chunk");
    Require(ReadUInt32(file, 0) == s_GLBMagic && ReadUInt32(file, sizeof(std::uint32_t)) == 2, "GLB magic and version 2");
    Require(ReadUInt32(file, 2 * sizeof(std::uint32_t)) == file.size(), "GLB length is the file size");

    const auto jsonLength = ReadUInt32(file, s_GLBHeaderBytes);
    Require(ReadUInt32(file, s_GLBHeaderBytes + sizeof(std::uint32_t)) == s_GLBChunkJSON && jsonLength % 4 == 0, "the first chunk is the aligned JSON chunk");

    Document    document;
    const auto* pJSON = reinterpret_cast<const char*>(file.data() + s_GLBHeaderBytes + s_GLBChunkHeader);
    document.gltf     = json::parse(pJSON, pJSON + jsonLength);

    const auto binOffset = s_GLBHeaderBytes + s_GLBChunkHeader + jsonLength;

    if (binOffset < file.size())
    {
        const auto binLength = ReadUInt32(file, binOffset);
        Require(ReadUInt32(file, binOffset + sizeof(std::uint32_t)) == s_GLBChunkBIN && binLength % 4 == 0, "the second chunk is the aligned BIN chunk");
        Require(binOffset + s_GLBChunkHeader + binLength == file.size(), "the BIN chunk ends the file");

        document.bin.assign(file.begin() + binOffset + s_GLBChunkHeader, file.end());
        Require(document.gltf["buffers"].size() == 1 && document.gltf["buffers"][0]["byteLength"].get<std::size_t>() <= document.bin.size(), "buffer 0 is the BIN chunk");
    }

    return document;
}

//The bytes of a buffer view, checked against the buffer.
static const std::uint8_t* ViewBytes(const Document& document, int view, std::size_t& bytes)
{
    const auto& bufferView = document.gltf["bufferViews"][view];
    const auto  offset     = bufferView.value("byteOffset", std::size_t{0});
    bytes                  = bufferView["byteLength"].get<std::size_t>();

    Require(bufferView["buffer"] == 0 && offset % 4 == 0 && bytes > 0 && offset + bytes <= document.bin.size(), "buffer views are aligned, non-empty and inside the buffer");

    return document.bin.data() + offset;
}

static std::vector<float> ReadFloats(const Document& document, int accessorIndex, std::size_t components)
{
    const auto& accessor = document.gltf["accessors"][accessorIndex];
    Require(accessor["componentType"] == s_ComponentFloat, "float accessor");

    std::size_t bytes{};
    const auto* pData = ViewBytes(document, accessor["bufferView"].get<int>(), bytes);
    const auto  count = accessor["count"].get<std::size_t>();

    Require(count * components * sizeof(float) <= bytes, "the accessor fits its buffer view");

    std::vector<float> values(count * components);
    std::memcpy(values.data(), pData, values.size() * sizeof(float));

    for (const auto value : values)
        Require(std::isfinite(value), "accessor values are finite");

    return values;
}

static std::vector<std::uint32_t> ReadIndices(const Document& document, int accessorIndex)
{
    const auto& accessor = document.gltf["accessors"][accessorIndex];
    const auto  type     = accessor["componentType"].get<int>();
    const auto  count    = accessor["count"].get<std::size_t>();

    std::size_t bytes{};
    const auto* pData = ViewBytes(document, accessor["bufferView"].get<int>(), bytes);

    std::vector<std::uint32_t> indices(count);

    if (type == s_ComponentUnsignedShort)
    {
        Require(count * sizeof(std::uint16_t) <= bytes, "short indices fit their view");

        for (std::size_t i = 0; i < count; ++i)
        {
            std::uint16_t index;
            std::memcpy(&index, pData + i * sizeof(index), sizeof(index));
            indices[i] = index;
        }
    }
    else
    {
        Require(type == s_ComponentUnsignedInt && count * sizeof(std::uint32_t) <= bytes, "int indices fit their view");
        std::memcpy(indices.data(), pData, count * sizeof(std::uint32_t));
    }

    return indices;
}

struct Vec3
{
    double x{};
    double y{};
    double z{};
};

//Column-major 4x4 matrix as glTF stores node matrices.
using Matrix = std::array<double, 16>;

static Matrix Identity()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

static Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix result{};

    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int k = 0; k < 4; ++k)
                result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
        }
    }

    return result;
}

static Matrix NodeMatrix(const json& node)
{
    std::array<double, 3> t{0, 0, 0};
    std::array<double, 4> r{0, 0, 0, 1};
    std::array<double, 3> s{1, 1, 1};

    if (node.contains("translation"))
        t = node["translation"].get<std::array<double, 3>>();

    if (node.contains("rotation"))
        r = node["rotation"].get<std::array<double, 4>>();

    if (node.contains("scale"))
        s = node["scale"].get<std::array<double, 3>>();

    const auto [x, y, z, w] = r;

    Matrix m = {
        (1 - 2 * (y * y + z * z)) * s[0], (2 * (x * y + z * w)) * s[0], (2 * (x * z - y * w)) * s[0], 0,
        (2 * (x * y - z * w)) * s[1], (1 - 2 * (x * x + z * z)) * s[1], (2 * (y * z + x * w)) * s[1], 0,
        (2 * (x * z + y * w)) * s[2], (2 * (y * z - x * w)) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
        t[0], t[1], t[2], 1};

    return m;
}

static Vec3 Transform(const Matrix& m, const Vec3& v)
{
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13], m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
}

static bool Near(const Vec3& a, const Vec3& b, double tolerance = s_PositionTolerance)
{
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

//Parent of every node, -1 for roots.
static std::vector<int> NodeParents(const Document& document)
{
    const auto& nodes = document.gltf["nodes"];

    std::vector<int> parents(nodes.size(), -1);

    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        for (const auto& child : nodes[i].value("children", json::array()))
        {
            Require(parents[child.get<int>()] == -1, "a node has one parent");
            parents[child.get<int>()] = static_cast<int>(i);
        }
    }

    return parents;
}

static Matrix GlobalMatrix(const Document& document, int node)
{
    const auto parents = NodeParents(document);

    Matrix matrix = Identity();

    for (; node >= 0; node = parents[node])
        matrix = Multiply(NodeMatrix(document.gltf["nodes"][node]), matrix);

    return matrix;
}

static int FindNode(const Document& document, const std::string& name)
{
    const auto& nodes = document.gltf["nodes"];

    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        if (nodes[i].value("name", std::string()) == name)
            return static_cast<int>(i);
    }

    return -1;
}

//Node of an entity, by its extras identity, -1 when it has none.
static int FindEntityNode(const Document& document, std::uint32_t entity)
{
    const auto& nodes = document.gltf["nodes"];

    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& extras = nodes[i].value("extras", json::object());

        if (extras.contains("goldsrc") && extras["goldsrc"].contains("entityIndex") && extras["goldsrc"]["entityIndex"] == static_cast<int>(entity))
            return static_cast<int>(i);
    }

    return -1;
}

static int FindChildMesh(const Document& document, int node)
{
    for (const auto& child : document.gltf["nodes"][node].value("children", json::array()))
    {
        const auto& childNode = document.gltf["nodes"][child.get<int>()];

        if (childNode.contains("mesh"))
            return childNode["mesh"].get<int>();
    }

    return -1;
}

//A conversion as the tests write it: the request with standard library callbacks, and what the test sinks collected.
//Without outputSink, the asset (the first file the converter writes) is kept in fileName and data, and the external
//images in externalFiles.
using TestTextureResolver = std::function<bool(const TextureRequest& request, ResolvedTexture& texture)>;
using TestOutputSink      = std::function<bool(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes)>;

struct ConversionRequest
{
    BSPAssetView         asset;
    BSPConversionOptions options;
    TestTextureResolver  textureResolver;
    std::string          resourceName;
    std::string          sourcePath;
    TestOutputSink       outputSink;
};

struct TestDiagnostic
{
    DiagnosticSeverity severity{DiagnosticSeverity::Warning};
    DiagnosticCode     code{DiagnosticCode::ConversionFailure};
    std::string        message;
};

struct TestOutputFile
{
    std::string               relativePath;
    std::vector<std::uint8_t> bytes;
};

struct ConversionResult
{
    bool                        bSuccess{};
    ConversionError             error{ConversionError::None};
    std::string                 errorMessage;
    std::uint32_t               warningCount{};
    std::vector<TestDiagnostic> warnings;
    std::string                 fileName;
    std::vector<std::uint8_t>   data;
    std::vector<TestOutputFile> externalFiles;
    ConversionStatistics        statistics;
    std::uint32_t               converterVersion{};
    std::uint32_t               extrasSchemaVersion{};
    std::string                 sourceHash;
    std::string                 textureHash;
    std::string                 cacheKey;
};

class CTestTextureResolver final : public ITextureResolver
{
public:
    explicit CTestTextureResolver(const TestTextureResolver& resolve) :
        m_Resolve(resolve)
    {
    }

    bool ResolveTexture(const TextureRequest& request, ResolvedTexture* pTexture) noexcept override
    {
        return m_Resolve(request, *pTexture);
    }

private:
    const TestTextureResolver& m_Resolve;
};

class CTestOutputSink final : public IConversionOutputSink
{
public:
    CTestOutputSink(const TestOutputSink& sink, ConversionResult& result) :
        m_Sink(sink), m_Result(result)
    {
    }

    bool WriteFile(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes) noexcept override
    {
        if (m_Sink)
            return m_Sink(szRelativePath, pData, bytes);

        std::vector<std::uint8_t> file(pData, pData + bytes);

        if (m_Result.fileName.empty())
        {
            m_Result.fileName = szRelativePath;
            m_Result.data     = std::move(file);
        }
        else
            m_Result.externalFiles.push_back({szRelativePath, std::move(file)});

        return true;
    }

private:
    const TestOutputSink& m_Sink;
    ConversionResult&     m_Result;
};

class CTestDiagnosticSink final : public IDiagnosticSink
{
public:
    explicit CTestDiagnosticSink(std::vector<TestDiagnostic>& diagnostics) :
        m_Diagnostics(diagnostics)
    {
    }

    void OnDiagnostic(const DiagnosticView& diagnostic) noexcept override
    {
        m_Diagnostics.push_back({diagnostic.severity, diagnostic.code, diagnostic.szMessage});
    }

private:
    std::vector<TestDiagnostic>& m_Diagnostics;
};

static ConversionResult ConvertRequest(const ConversionRequest& request)
{
    ConversionResult result;

    CTestTextureResolver resolver(request.textureResolver);
    CTestOutputSink      outputSink(request.outputSink, result);
    CTestDiagnosticSink  diagnosticSink(result.warnings);

    BSPConversionRequestView view;
    view.asset            = request.asset;
    view.options          = request.options;
    view.pTextureResolver = request.textureResolver ? &resolver : nullptr;
    view.szResourceName   = request.resourceName.c_str();
    view.szSourcePath     = request.sourcePath.c_str();
    view.pOutputSink      = &outputSink;
    view.pDiagnosticSink  = &diagnosticSink;

    const auto summary = ConvertBSP(view);

    result.bSuccess            = summary.error == ConversionError::None;
    result.error               = summary.error;
    result.errorMessage        = summary.szErrorMessage;
    result.warningCount        = summary.warningCount;
    result.statistics          = summary.statistics;
    result.converterVersion    = summary.converterVersion;
    result.extrasSchemaVersion = summary.extrasSchemaVersion;
    result.sourceHash          = summary.szSourceHash;
    result.textureHash         = summary.szTextureHash;
    result.cacheKey            = summary.szCacheKey;

    return result;
}

//Converts with the standard test resolver: floor2 and trigger as RGBA8 images, !water from a WAD, wall never
//resolved; floor and {grate fall back to their embedded pixels.
struct ConversionRun
{
    ConversionResult         result;
    std::vector<std::string> requested;
};

static const std::vector<std::uint8_t>& TestWAD()
{
    static const auto wad = MakeConverterTestWAD("!WATER", TestBSPTextureSize, TestBSPTextureSize);

    return wad;
}

constexpr std::uint8_t s_ResolvedTexel = 128;

static ConversionRun Convert(const std::vector<std::uint8_t>& file, const BSPConversionOptions& options, BSPLayout layout = BSPLayout::GoldSrc)
{
    static const std::vector<std::uint8_t> rgba(TestBSPTextureSize * TestBSPTextureSize * GoldSrcRender::RGBABytes, s_ResolvedTexel);

    ConversionRun run;

    ConversionRequest request;
    request.asset.szIdentifier = "maps/converter_test.bsp";
    request.asset.pFile        = file.data();
    request.asset.fileBytes    = static_cast<std::uint32_t>(file.size());
    request.asset.layout       = layout;
    request.options            = options;
    request.sourcePath         = "maps/converter_test.bsp";

    request.textureResolver = [&run](const TextureRequest& textureRequest, ResolvedTexture& texture) {
        run.requested.push_back(textureRequest.szName);

        const std::string name = textureRequest.szName;

        if (textureRequest.kind == TextureKind::Miptex && (name == "floor2" || name == "trigger"))
        {
            texture.format    = ResolvedTextureFormat::RGBA8;
            texture.width     = TestBSPTextureSize;
            texture.height    = TestBSPTextureSize;
            texture.pData     = rgba.data();
            texture.dataBytes = rgba.size();
            return true;
        }

        if (textureRequest.kind == TextureKind::Miptex && name == "!water")
            return FindWAD3Texture(TestWAD().data(), TestWAD().size(), textureRequest.szName, &texture);

        return false;
    };

    run.result = ConvertRequest(request);

    return run;
}

static bool HasDiagnostic(const ConversionResult& result, DiagnosticCode code)
{
    return std::any_of(result.warnings.begin(), result.warnings.end(), [code](const TestDiagnostic& diagnostic) { return diagnostic.code == code; });
}

//Face records of every primitive of a mesh, by face index, with the primitive they belong to.
struct FaceLocation
{
    std::size_t primitive{};
    json        record;
};

static std::multimap<std::uint32_t, FaceLocation> MeshFaces(const Document& document, int mesh)
{
    std::multimap<std::uint32_t, FaceLocation> faces;

    if (mesh < 0)
        return faces;

    const auto& primitives = document.gltf["meshes"][mesh]["primitives"];

    for (std::size_t i = 0; i < primitives.size(); ++i)
    {
        for (const auto& record : primitives[i]["extras"]["goldsrc"]["faces"])
            faces.insert({record[0].get<std::uint32_t>(), {i, record}});
    }

    return faces;
}

static int ModelMesh(const Document& document, std::uint32_t model, const char* szKind)
{
    return document.Goldsrc()["models"][model]["meshes"][szKind].get<int>();
}

//The structural rules every converted document must follow: valid references, triangle primitives with matching
//attribute counts, indices inside the vertices, unit normals, positions inside their bounds, one parent per node.
static void RequireValidDocument(const Document& document)
{
    const auto& gltf = document.gltf;

    Require(gltf["asset"]["version"] == "2.0", "glTF 2.0 asset");
    Require(gltf["scenes"].size() == 1 && gltf["scene"] == 0, "one default scene");

    NodeParents(document);

    for (const auto& mesh : gltf["meshes"])
    {
        for (const auto& primitive : mesh["primitives"])
        {
            Require(primitive.value("mode", s_ModeTriangles) == s_ModeTriangles, "triangle primitives");
            Require(primitive["material"].get<std::size_t>() < gltf["materials"].size(), "primitives reference a material");

            const auto& attributes = primitive["attributes"];
            const auto  positions  = ReadFloats(document, attributes["POSITION"].get<int>(), 3);
            const auto  normals    = ReadFloats(document, attributes["NORMAL"].get<int>(), 3);
            const auto  texcoords  = ReadFloats(document, attributes["TEXCOORD_0"].get<int>(), 2);
            const auto  vertices   = positions.size() / 3;

            Require(normals.size() == positions.size() && texcoords.size() / 2 == vertices, "attributes have one value per vertex");

            if (attributes.contains("TEXCOORD_1"))
                Require(ReadFloats(document, attributes["TEXCOORD_1"].get<int>(), 2).size() / 2 == vertices, "lightmap coordinates have one value per vertex");

            for (std::size_t i = 0; i < vertices; ++i)
            {
                const auto length = std::sqrt(normals[i * 3] * normals[i * 3] + normals[i * 3 + 1] * normals[i * 3 + 1] + normals[i * 3 + 2] * normals[i * 3 + 2]);
                Require(std::fabs(length - 1) < s_UnitTolerance, "normals are unit vectors");
            }

            const auto& position = gltf["accessors"][attributes["POSITION"].get<int>()];
            const auto  mins     = position["min"].get<std::vector<double>>();
            const auto  maxs     = position["max"].get<std::vector<double>>();

            for (std::size_t i = 0; i < positions.size(); ++i)
                Require(positions[i] >= mins[i % 3] && positions[i] <= maxs[i % 3], "positions lie inside their accessor bounds");

            const auto indices = ReadIndices(document, primitive["indices"].get<int>());
            Require(!indices.empty() && indices.size() % 3 == 0, "whole triangles");

            for (const auto index : indices)
                Require(index < vertices, "indices reference existing vertices");

            for (std::size_t i = 0; i < indices.size(); i += 3)
            {
                const auto a = indices[i] * 3;
                const auto b = indices[i + 1] * 3;
                const auto c = indices[i + 2] * 3;
                const Vec3 ab{static_cast<double>(positions[b]) - positions[a], static_cast<double>(positions[b + 1]) - positions[a + 1], static_cast<double>(positions[b + 2]) - positions[a + 2]};
                const Vec3 ac{static_cast<double>(positions[c]) - positions[a], static_cast<double>(positions[c + 1]) - positions[a + 1], static_cast<double>(positions[c + 2]) - positions[a + 2]};
                const Vec3 cross{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};

                Require(cross.x * normals[a] + cross.y * normals[a + 1] + cross.z * normals[a + 2] >= 0, "triangle winding agrees with the exported face normal");
            }
        }
    }

    for (const auto& texture : gltf.value("textures", json::array()))
        Require(texture["source"].get<std::size_t>() < gltf["images"].size(), "textures reference an image");

    for (const auto& image : gltf.value("images", json::array()))
    {
        if (!image.contains("bufferView"))
            continue;

        std::size_t bytes{};
        const auto* pPNG = ViewBytes(document, image["bufferView"].get<int>(), bytes);

        int width{}, height{}, channels{};
        Require(image["mimeType"] == "image/png" && stbi_info_from_memory(pPNG, static_cast<int>(bytes), &width, &height, &channels), "embedded images are PNG files");
    }
}

static Document ConvertAndParse(const std::vector<std::uint8_t>& file, const BSPConversionOptions& options, ConversionRun& run, BSPLayout layout = BSPLayout::GoldSrc)
{
    run = Convert(file, options, layout);
    Require(run.result.bSuccess && run.result.error == ConversionError::None, "the test BSP converts");

    auto document = ParseGLB(run.result.data);
    RequireValidDocument(document);

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader([](tinygltf::Image*, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*) { return true; }, nullptr);

    tinygltf::Model model;
    std::string     error;
    std::string     warning;
    Require(loader.LoadBinaryFromMemory(&model, &error, &warning, run.result.data.data(), static_cast<unsigned int>(run.result.data.size())), "tinygltf loads the GLB");

    return document;
}

//16.1 The world's drawn surfaces are model 0's geometry, every face of the model accounted for.
static void TestWorldGeometry()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);
    WriteOutput("converter_default.glb", run.result.data);

    const auto surfaces = MeshFaces(document, ModelMesh(document, 0, "surface"));
    const auto sky      = MeshFaces(document, ModelMesh(document, 0, "sky"));
    const auto water    = MeshFaces(document, ModelMesh(document, 0, "water"));

    //floor, wall, {grate, floor2 and trigger are drawn; !water is water, sky is sky, face 5 is degenerate.
    for (const std::uint32_t face : {0u, 3u, 4u, 6u, 7u})
        Require(surfaces.count(face) == 1, "drawn faces are world geometry");

    Require(surfaces.size() == 5 && sky.size() == 1 && water.size() == 1, "each face exported once");
    Require(document.Goldsrc()["models"][0]["degenerateFaces"] == json::array({TestBSPDegenerateFace}), "the degenerate face is listed");
    Require(run.result.statistics.worldFaceCount == 7 && run.result.statistics.degenerateFaceCount == 2, "world face statistics");

    const auto worldNode = FindNode(document, "BSP_World");
    Require(worldNode >= 0 && document.gltf["nodes"][worldNode]["extras"]["goldsrc"]["entityIndex"] == ConverterTestWorldspawnEntity, "BSP_World is the worldspawn, which is not entity 0");
    Require(FindNode(document, "World_Geometry") >= 0 && FindNode(document, "World_Sky") >= 0 && FindNode(document, "World_Water") >= 0, "world mesh nodes");

    //Every vertex of the world's surfaces lies on the quad, as in the file.
    const auto& primitives = document.gltf["meshes"][ModelMesh(document, 0, "surface")]["primitives"];

    for (const auto& primitive : primitives)
    {
        const auto positions = ReadFloats(document, primitive["attributes"]["POSITION"].get<int>(), 3);

        for (std::size_t i = 0; i < positions.size(); i += 3)
            Require(positions[i + 2] == 0 && positions[i] >= 0 && positions[i] <= TestBSPQuadSize && positions[i + 1] >= 0 && positions[i + 1] <= TestBSPQuadSize, "world vertices keep the file's coordinates");
    }

    //The floor's diffuse coordinates are its texinfo's, divided by its 64x32 miptex.
    const auto& floor     = surfaces.find(0)->second;
    const auto& primitive = primitives[floor.primitive];
    const auto  positions = ReadFloats(document, primitive["attributes"]["POSITION"].get<int>(), 3);
    const auto  texcoords = ReadFloats(document, primitive["attributes"]["TEXCOORD_0"].get<int>(), 2);
    const auto  indices   = ReadIndices(document, primitive["indices"].get<int>());

    for (auto i = floor.record[1].get<std::uint32_t>(); i < floor.record[1].get<std::uint32_t>() + floor.record[2].get<std::uint32_t>(); ++i)
    {
        const auto vertex = indices[i];
        Require(texcoords[vertex * 2] == positions[vertex * 3] / TestBSPFloorWidth && texcoords[vertex * 2 + 1] == positions[vertex * 3 + 1] / TestBSPFloorHeight, "diffuse coordinates follow the texinfo");
    }
}

//glTF fronts agree with the face normal for either polygon order and either BSP plane side, for every geometry kind.
static void TestTriangleWinding()
{
    constexpr std::uint32_t s_QuadEdges      = 4;
    constexpr std::uint32_t s_FaceSideOffset = sizeof(std::uint16_t);

    for (const bool bBack : {false, true})
    {
        for (const bool bReverse : {false, true})
        {
            auto lumps = MakeConverterTestLumps();

            if (bReverse)
            {
                CBSPWriter surfEdges;

                for (std::int32_t edge = s_QuadEdges; edge > 0; --edge)
                    surfEdges.Int32(-edge);

                lumps[BSPSurfEdgeLump] = std::move(surfEdges.Bytes());
            }

            for (std::uint32_t face = 0; face < TestBSPFaceCount; ++face)
                PatchUInt16(lumps[BSPFaceLump], face * TestBSPFaceBytes + s_FaceSideOffset, bBack ? 1 : 0);

            ConversionRun run;
            const auto    document = ConvertAndParse(AssembleTestBSP(lumps), {}, run);
            const auto    name     = std::string("converter_winding_") + (bReverse ? "cw" : "ccw") + (bBack ? "_back.glb" : "_front.glb");
            WriteOutput(name, run.result.data);

            Require(run.result.statistics.skyFaceCount == 2 && run.result.statistics.waterFaceCount == 2, "winding checks cover world and brush sky and water");

            for (const auto& material : document.gltf["materials"])
                Require(material.value("doubleSided", false) == (material["extras"]["goldsrc"]["surfaceType"] == "water"), "only water is double sided; ordinary faces rely on correct winding");
        }
    }
}

//16.2 Sky faces become real triangles in the model that lists them: the world's and submodel 2's.
static void TestSkyGeometry()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    Require(run.result.statistics.skyFaceCount == 2, "the sky face of the world and of submodel 2 are exported");

    for (const std::uint32_t model : {0u, ConverterTestSkyModel})
    {
        const auto mesh = ModelMesh(document, model, "sky");
        Require(mesh >= 0, "the model has a sky mesh");

        const auto faces = MeshFaces(document, mesh);
        Require(faces.size() == 1 && faces.count(TestBSPSkyFace) == 1, "the sky face keeps its face index");

        const auto& primitive = document.gltf["meshes"][mesh]["primitives"][0];
        const auto  positions = ReadFloats(document, primitive["attributes"]["POSITION"].get<int>(), 3);
        const auto  normals   = ReadFloats(document, primitive["attributes"]["NORMAL"].get<int>(), 3);
        const auto  indices   = ReadIndices(document, primitive["indices"].get<int>());

        Require(positions.size() == 12 && indices.size() == 6 && !primitive["attributes"].contains("TEXCOORD_1"), "a quad of two triangles without lightmap coordinates");
        Require(normals[2] == 1, "the sky keeps its plane normal");

        //The fan keeps the file's winding: (0,0) (64,0) (64,64) is counterclockwise seen from +z.
        Require(positions[3] == TestBSPQuadSize && positions[4] == 0 && positions[6] == TestBSPQuadSize && positions[7] == TestBSPQuadSize, "the sky keeps the corners in the file's order");

        const auto& material = document.gltf["materials"][primitive["material"].get<int>()];
        Require(material["extras"]["goldsrc"]["surfaceType"] == "sky" && material["extras"]["goldsrc"]["isGoldSrcSky"] == true, "the sky material is marked as GoldSrc sky");
        Require(material["extensions"].contains("KHR_materials_unlit") && !material["pbrMetallicRoughness"].contains("baseColorTexture"), "the sky material is an unlit placeholder");
    }

    Require(document.Goldsrc()["sky"]["skyname"] == "desert", "the worldspawn skyname is kept");
    Require(HasDiagnostic(run.result, DiagnosticCode::MissingSkyboxTexture), "missing skybox images are reported, the geometry kept");

    //Without sky geometry the faces are listed instead.
    BSPConversionOptions options;
    options.bPreserveSkyGeometry = false;

    ConversionRun noSkyRun;
    const auto    noSky = ConvertAndParse(MakeConverterTestBSP(), options, noSkyRun);
    Require(ModelMesh(noSky, 0, "sky") < 0 && noSky.Goldsrc()["models"][0]["omittedSkyFaces"] == json::array({TestBSPSkyFace}), "omitted sky faces are listed");
}

//16.3 / 16.4 / 16.5 Every entity has a node; brush entities share their submodel's meshes.
static void TestEntityNodes()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    for (std::uint32_t entity = 0; entity < ConverterTestEntityCount; ++entity)
        Require(FindEntityNode(document, entity) >= 0, "every entity has a node");

    const auto brushGroup = FindNode(document, "Brush_Entities");
    const auto pointGroup = FindNode(document, "Point_Entities");
    const auto parents    = NodeParents(document);

    for (const auto entity : {ConverterTestWallEntity, ConverterTestDoorEntity, ConverterTestButtonEntity, ConverterTestSharedEntity})
        Require(parents[FindEntityNode(document, entity)] == brushGroup, "brush entities are under Brush_Entities");

    for (const auto entity : {ConverterTestPlayerEntity, ConverterTestLightEntity, ConverterTestInvalidEntity, ConverterTestSpriteEntity, ConverterTestPathEntity})
    {
        const auto node = FindEntityNode(document, entity);
        Require(parents[node] == pointGroup && FindChildMesh(document, node) < 0 && !document.gltf["nodes"][node].contains("mesh"), "point entities are nodes without mesh");
    }

    Require(document.gltf["nodes"][FindEntityNode(document, ConverterTestWallEntity)]["name"] == "ent_0000_func_wall", "node names show the entity index and class");
    Require(run.result.statistics.brushEntityNodeCount == 4 && run.result.statistics.pointEntityNodeCount == 5 && run.result.statistics.entityCount == ConverterTestEntityCount, "entity node statistics");

    //Entities 0 and 4 show model 1: one mesh, two nodes with their own transforms.
    const auto wall   = FindEntityNode(document, ConverterTestWallEntity);
    const auto shared = FindEntityNode(document, ConverterTestSharedEntity);
    const auto mesh   = FindChildMesh(document, wall);

    Require(mesh >= 0 && mesh == FindChildMesh(document, shared) && mesh == ModelMesh(document, ConverterTestSharedModel, "surface"), "brush entities of one model share its mesh");
    Require(document.gltf["nodes"][wall]["translation"] != document.gltf["nodes"][shared]["translation"], "the sharing nodes keep their own transforms");
    Require(document.Goldsrc()["models"][ConverterTestSharedModel]["entities"] == json::array({ConverterTestWallEntity, ConverterTestSharedEntity}), "the model lists the entities showing it");

    //The door, button and wall show models 2, 3 and 1; the door's model also has a sky mesh, the button's water.
    Require(FindChildMesh(document, FindEntityNode(document, ConverterTestDoorEntity)) == ModelMesh(document, ConverterTestSkyModel, "surface"), "the door shows model 2");
    Require(ModelMesh(document, ConverterTestWaterModel, "water") >= 0, "submodel water stays with its submodel");

    //Model 4 is shown by no entity: its mesh stays in the document, out of the scene, and is reported.
    const auto unused = ModelMesh(document, ConverterTestUnusedModel, "surface");
    Require(unused >= 0 && run.result.statistics.unreferencedSubmodelCount == 1 && HasDiagnostic(run.result, DiagnosticCode::UnreferencedSubmodel), "unreferenced submodels are kept and reported");

    for (const auto& node : document.gltf["nodes"])
        Require(node.value("mesh", -1) != unused, "no node shows the unreferenced submodel");

    //Entity 7 references *7 of a five-model file: reported, kept as a point entity.
    Require(HasDiagnostic(run.result, DiagnosticCode::InvalidModelReference), "invalid model references are reported");
    Require(document.gltf["nodes"][FindEntityNode(document, ConverterTestInvalidEntity)]["extras"]["goldsrc"]["modelKind"] == "invalid", "the invalid reference is marked");
    Require(document.gltf["nodes"][FindEntityNode(document, ConverterTestSpriteEntity)]["extras"]["goldsrc"]["originalModelString"] == "sprites/glow01.spr", "external model paths are kept");
}

//9.x Key values are kept in order with duplicates, unknown keys and non-UTF-8 text.
static void TestEntityKeyValues()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    const auto& world     = document.gltf["nodes"][FindNode(document, "BSP_World")]["extras"]["goldsrc"];
    const auto& keyValues = world["keyvalues"];

    Require(world["classname"] == "worldspawn" && world["bspModelIndex"] == 0, "worldspawn identity");
    Require(keyValues.size() == 7 && keyValues[0] == json::array({"classname", "worldspawn"}) && keyValues[1] == json::array({"wad", "\\valve\\halflife.wad"}), "key values in file order");
    Require(keyValues[3] == json::array({"message", "caf\xC3\xA9", "latin1"}), "non-UTF-8 text is decoded as Latin-1 and marked");
    Require(keyValues[4] == json::array({"_note", "first"}) && keyValues[5] == json::array({"_note", "second"}), "duplicate keys are kept");
    Require(keyValues[6] == json::array({"custom_key", "custom value"}), "unknown keys are kept");
    Require(HasDiagnostic(run.result, DiagnosticCode::NonUTF8Text), "non-UTF-8 text is reported");
    Require(run.result.warnings.size() == run.result.warningCount, "the sink sees every diagnostic");

    const auto& door = document.gltf["nodes"][FindEntityNode(document, ConverterTestDoorEntity)]["extras"]["goldsrc"];
    Require(door["entityIndex"] == ConverterTestDoorEntity && door["classname"] == "func_door" && door["originalModelString"] == "*2" && door["bspModelIndex"] == 2, "brush entity identity");
    Require(door["originalOrigin"] == json::array({10.0, 0.0, 0.0}) && door["originalAngles"] == json::array({0.0, 90.0, 0.0}), "original origin and angles");

    const auto& wall = document.gltf["nodes"][FindEntityNode(document, ConverterTestWallEntity)]["extras"]["goldsrc"];
    Require(wall["render"]["rendermode"] == 4 && wall["render"]["renderamt"] == 200, "render properties");

    //"angle" turns into angles as ED_ParseEntity does: a yaw, or -1 for straight up.
    const auto& player = document.gltf["nodes"][FindEntityNode(document, ConverterTestPlayerEntity)]["extras"]["goldsrc"];
    const auto& path   = document.gltf["nodes"][FindEntityNode(document, ConverterTestPathEntity)]["extras"]["goldsrc"];
    Require(player["originalAngles"] == json::array({0.0, 45.0, 0.0}) && path["originalAngles"] == json::array({-90.0, 0.0, 0.0}), "the angle key");

    //Without key values, the identity remains.
    BSPConversionOptions options;
    options.bPreserveEntityKeyValues = false;

    ConversionRun noKeysRun;
    const auto    noKeys = ConvertAndParse(MakeConverterTestBSP(), options, noKeysRun);
    const auto&   bare   = noKeys.gltf["nodes"][FindEntityNode(noKeys, ConverterTestDoorEntity)]["extras"]["goldsrc"];
    Require(!bare.contains("keyvalues") && bare["entityIndex"] == ConverterTestDoorEntity, "identity without key values");
}

//GoldSrc's rotation of angles (pitch, yaw, roll): yaw about z, pitch about y, roll about x (R_GetEntityTransform).
static Vec3 RotateGoldSrc(const Vec3& angles, const Vec3& v)
{
    constexpr double s_DegreesToRadians = 3.14159265358979323846 / 180.0;

    auto rotate = [](double angle, int axis, const Vec3& p) -> Vec3 {
        const auto c = std::cos(angle);
        const auto s = std::sin(angle);

        if (axis == 2)
            return {c * p.x - s * p.y, s * p.x + c * p.y, p.z};

        if (axis == 1)
            return {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};

        return {p.x, c * p.y - s * p.z, s * p.y + c * p.z};
    };

    //Matrix Rz * Ry * Rx applied to v: roll first.
    auto p = rotate(angles.z * s_DegreesToRadians, 0, v);
    p      = rotate(angles.x * s_DegreesToRadians, 1, p);

    return rotate(angles.y * s_DegreesToRadians, 2, p);
}

//GoldSrc to glTF coordinates as the root node converts them: (x, y, z) -> (x, z, -y), then the scale.
static Vec3 ToGLTF(const Vec3& v, double scale)
{
    return {v.x * scale, v.z * scale, -v.y * scale};
}

//16.6 Converted vertices reach the same world position as GoldSrc places them.
static void TestTransforms()
{
    constexpr float s_Scale = 0.0254f;

    BSPConversionOptions options;
    options.scaleFactor = s_Scale;

    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), options, run);

    struct Expectation
    {
        std::uint32_t entity;
        Vec3          origin;
        Vec3          angles;
    };

    //The wall rotates by its angles; the door's angles are its movement direction (SetMovedir), so it is unrotated.
    const Expectation expectations[] = {
        {ConverterTestWallEntity, {100, 200, 300}, {10, 90, 30}},
        {ConverterTestSharedEntity, {-50, 0, 0}, {0, 0, 0}},
        {ConverterTestDoorEntity, {10, 0, 0}, {0, 0, 0}},
        {ConverterTestButtonEntity, {0, 0, 0}, {0, 0, 0}},
    };

    for (const auto& expectation : expectations)
    {
        const auto entityNode = FindEntityNode(document, expectation.entity);

        for (const auto& child : document.gltf["nodes"][entityNode]["children"])
        {
            const auto  meshNode  = child.get<int>();
            const auto  global    = GlobalMatrix(document, meshNode);
            const auto& mesh      = document.gltf["meshes"][document.gltf["nodes"][meshNode]["mesh"].get<int>()];
            const auto  positions = ReadFloats(document, mesh["primitives"][0]["attributes"]["POSITION"].get<int>(), 3);

            for (std::size_t i = 0; i < positions.size(); i += 3)
            {
                const Vec3 source{positions[i], positions[i + 1], positions[i + 2]};
                const auto rotated = RotateGoldSrc(expectation.angles, source);
                const Vec3 world{rotated.x + expectation.origin.x, rotated.y + expectation.origin.y, rotated.z + expectation.origin.z};

                Require(Near(Transform(global, source), ToGLTF(world, s_Scale), s_PositionTolerance * s_Scale), "converted vertices reach GoldSrc's world position");
            }
        }
    }

    const auto& door = document.gltf["nodes"][FindEntityNode(document, ConverterTestDoorEntity)];
    Require(!door.contains("rotation") && door["extras"]["goldsrc"]["angleUsage"] == "moveDirection", "move direction angles do not rotate");

    //Point entities are placed by their origin too; the world is not moved.
    const auto player = FindEntityNode(document, ConverterTestPlayerEntity);
    Require(Near(Transform(GlobalMatrix(document, player), {0, 0, 0}), ToGLTF({1, 2, 3}, s_Scale), s_PositionTolerance * s_Scale), "point entities are placed by their origin");
    Require(Near(Transform(GlobalMatrix(document, FindNode(document, "World_Geometry")), {1, 2, 3}), ToGLTF({1, 2, 3}, s_Scale), s_PositionTolerance * s_Scale), "the world keeps its coordinates");
    Require(document.Goldsrc()["unitScale"].get<double>() == static_cast<double>(s_Scale) && document.Goldsrc()["coordinateSystem"] == "GoldSrc_Z_UP", "coordinate metadata");
}

//16.7 Lightmaps keep their layers, placement and lightstyles.
static void TestLightmaps()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    const auto& lightmaps = document.Goldsrc()["lightmaps"];
    Require(lightmaps["present"] == true && lightmaps["texCoord"] == 1 && lightmaps["layers"].size() == 2, "the two used lightstyle layers are exported");
    Require(run.result.statistics.lightmapCount == 2 && run.result.statistics.lightmapPageCount == 1, "lightmap statistics");

    const auto faces  = MeshFaces(document, ModelMesh(document, 0, "surface"));
    const auto floor2 = faces.find(6)->second.record;

    //Columns: lightmapPage 5, lightmapX 6, lightmapY 7, lightmapWidth 8, lightmapHeight 9, style0-3 10-13.
    Require(floor2[8] == TestBSPLuxelsPerSide && floor2[9] == TestBSPLuxelsPerSide, "the lightmap rectangle size");
    Require(floor2[10] == 0 && floor2[11] == TestBSPAnimatedStyle && floor2[12] == GoldSrcRender::BSPNoLightstyle && floor2[13] == GoldSrcRender::BSPNoLightstyle, "the four lightstyles");

    const auto  atlasColumns = lightmaps["atlasColumns"].get<std::uint32_t>();
    const auto  page         = floor2[5].get<std::uint32_t>();
    const auto  pixelX       = (page % atlasColumns) * GoldSrcRender::BSPLightmapPageSize + floor2[6].get<std::uint32_t>();
    const auto  pixelY       = (page / atlasColumns) * GoldSrcRender::BSPLightmapPageSize + floor2[7].get<std::uint32_t>();
    const auto& expected     = std::array<std::array<std::uint8_t, 3>, 2>{TestBSPFloor2Sample, TestBSPFloor2Style};

    for (std::uint32_t layer = 0; layer < 2; ++layer)
    {
        const auto& texture = document.gltf["textures"][lightmaps["layers"][layer]["texture"].get<int>()];
        const auto& image   = document.gltf["images"][texture["source"].get<int>()];

        std::size_t bytes{};
        const auto* pPNG = ViewBytes(document, image["bufferView"].get<int>(), bytes);

        int  width{}, height{}, channels{};
        auto pTexels = stbi_load_from_memory(pPNG, static_cast<int>(bytes), &width, &height, &channels, 3);
        Require(pTexels && static_cast<std::uint32_t>(width) == lightmaps["atlasSize"][0].get<std::uint32_t>(), "lightmap atlases decode");

        const auto* pTexel = pTexels + (pixelY * width + pixelX) * 3;
        Require(pTexel[0] == expected[layer][0] && pTexel[1] == expected[layer][1] && pTexel[2] == expected[layer][2], "each layer keeps the face's samples at its placement");

        stbi_image_free(pTexels);
    }

    //Lightmap coordinates of the face's vertices fall inside its rectangle of the atlas.
    const auto& primitive = document.gltf["meshes"][ModelMesh(document, 0, "surface")]["primitives"][faces.find(6)->second.primitive];
    const auto  uv1       = ReadFloats(document, primitive["attributes"]["TEXCOORD_1"].get<int>(), 2);
    const auto  atlasSize = lightmaps["atlasSize"].get<std::array<double, 2>>();

    for (std::size_t i = 0; i < uv1.size(); i += 2)
    {
        const auto x = uv1[i] * atlasSize[0];
        const auto y = uv1[i + 1] * atlasSize[1];
        Require(x >= pixelX && x <= pixelX + TestBSPLuxelsPerSide && y >= pixelY && y <= pixelY + TestBSPLuxelsPerSide, "lightmap coordinates address the face's rectangle");
    }

    //The unlit trigger has no lightmap coordinates.
    const auto& trigger = document.gltf["meshes"][ModelMesh(document, 0, "surface")]["primitives"][faces.find(TestBSPTriggerFace)->second.primitive];
    Require(!trigger["attributes"].contains("TEXCOORD_1"), "TEX_SPECIAL faces have no lightmap coordinates");
}

static const json& FaceMaterial(const Document& document, int mesh, std::uint32_t face)
{
    const auto faces = MeshFaces(document, mesh);
    const auto index = document.gltf["meshes"][mesh]["primitives"][faces.find(face)->second.primitive]["material"].get<int>();

    return document.gltf["materials"][index];
}

//Ordinary and alpha-tested surfaces accept new scene lighting without using the BSP's baked lightmaps.
static void TestPBRMaterials()
{
    constexpr std::size_t s_EmissiveComponents = 3;
    constexpr double      s_AlphaCutoff        = 0.5;
    ConversionRun         run;
    const auto            document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    for (const auto& material : document.gltf["materials"])
    {
        if (material["extras"]["goldsrc"]["surfaceType"] == "sky")
            continue;

        Require(!material.value("extensions", json::object()).contains("KHR_materials_unlit"), "ordinary surfaces use lit PBR materials");
        const auto& pbr = material["pbrMetallicRoughness"];
        Require(pbr.value("metallicFactor", 1.0) == 0 && pbr.value("roughnessFactor", 1.0) == 1, "ordinary surfaces are rough dielectrics");
        Require(!material.contains("emissiveTexture") && material.value("emissiveFactor", std::vector<double>(s_EmissiveComponents, 0)) == std::vector<double>(s_EmissiveComponents, 0), "unlisted textures do not emit light");
    }

    const auto& grate = FaceMaterial(document, ModelMesh(document, 0, "surface"), TestBSPGrateSlot);
    Require(grate["alphaMode"] == "MASK" && grate.value("alphaCutoff", s_AlphaCutoff) == s_AlphaCutoff && grate["pbrMetallicRoughness"].contains("baseColorTexture"), "lit grates retain alpha clipping and their base texture");

    BSPConversionOptions options;
    options.bPreserveSkyGeometry = false;
    options.bPreserveLightmaps   = false;
    const auto noSky             = ConvertAndParse(MakeConverterTestBSP(), options, run);
    Require(noSky.gltf.value("extensionsUsed", std::vector<std::string>{}) == std::vector<std::string>{"KHR_lights_punctual"}, "a map with no sky or texture lights needs only the punctual light extension");
}

static std::vector<std::uint8_t> MakeBSPWithFloorTexture(const char* szName)
{
    constexpr std::size_t s_NameBytes   = 16;
    constexpr std::size_t s_MiptexTable = sizeof(std::int32_t);
    auto                  lumps         = MakeConverterTestLumps();
    auto&                 lump          = lumps[BSPTextureLump];
    const auto            record        = ReadUInt32(lump, s_MiptexTable + TestBSPFloorSlot * sizeof(std::int32_t));
    std::fill(lump.begin() + record, lump.begin() + record + s_NameBytes, std::uint8_t{0});
    std::copy(szName, szName + std::strlen(szName), lump.begin() + record);
    return AssembleTestBSP(lumps);
}

//SDK hl_lights.rad entries match the stored miptex name, ignoring case and preserving '~'.
static void TestTextureLights()
{
    constexpr std::size_t s_ColorComponents = 3;
    struct TextureLightCase
    {
        const char* szName;
        double      color[s_ColorComponents];
        bool        bEmissive;
    };

    const TextureLightCase cases[] = {
        {"+0~lIgHt2a", {255, 251, 129}, true},
        {"~LiGhT3a", {190, 20, 20}, true},
        {"rEd", {255, 0, 0}, true},
        {"+0~LIGHT2AX", {}, false}};
    constexpr double s_RADIntensity         = 2000;
    constexpr double s_RADScale             = 255;
    constexpr double s_DefaultEmissiveScale = 4;
    for (const auto& test : cases)
    {
        ConversionRun run;
        const auto    document   = ConvertAndParse(MakeBSPWithFloorTexture(test.szName), {}, run);
        const auto&   material   = FaceMaterial(document, ModelMesh(document, 0, "surface"), 0);
        const auto    extensions = material.value("extensions", json::object());

        if (!test.bEmissive)
        {
            Require(!material.contains("emissiveTexture") && !extensions.contains("KHR_materials_emissive_strength"), "a prefix match does not make an unlisted texture emit light");
            continue;
        }

        Require(!extensions.contains("KHR_materials_unlit") && extensions.contains("KHR_materials_emissive_strength"), "texture lights retain PBR and use HDR emission");
        Require(material["emissiveTexture"]["index"] == material["pbrMetallicRoughness"]["baseColorTexture"]["index"] && material["emissiveTexture"].value("texCoord", 0) == 0, "emission reuses the base image and texture UVs");

        for (std::size_t channel = 0; channel < std::size(test.color); ++channel)
            Require(std::fabs(test.color[channel] / s_RADScale - material["emissiveFactor"][channel].get<double>()) < s_UnitTolerance, "RAD color is normalized without clamping the intensity");

        Require(std::fabs(s_DefaultEmissiveScale * s_RADIntensity / s_RADScale - extensions["KHR_materials_emissive_strength"]["emissiveStrength"].get<double>()) < s_UnitTolerance, "default emission uses four times the normalized RAD intensity");
        const auto used = document.gltf.value("extensionsUsed", std::vector<std::string>{});
        Require(std::count(used.begin(), used.end(), "KHR_materials_emissive_strength") == 1, "the emission extension is declared once");

        if (&test == &cases[0])
            WriteOutput("converter_emissive.glb", run.result.data);
    }
}

static json AreaSidecar(const ConversionResult& result)
{
    for (const auto& file : result.externalFiles)
        if (file.relativePath.find(".sh9/area-lights.json") != std::string::npos)
            return json::parse(file.bytes);
    Require(false, "area light sidecar exists");
    return {};
}

static void TestAreaLightExport()
{
    const auto file     = MakeBSPWithFloorTexture("+0~LIGHT2A");
    const auto baseline = Convert(file, {});
    const auto area     = AreaSidecar(baseline.result);
    Require(baseline.result.bSuccess && !area["lights"].empty(), "emissive faces produce area lights");
    Require(area == AreaSidecar(Convert(file, {}).result), "area light generation is deterministic");
    CSHA256 hash;
    hash.Update(baseline.result.data.data(), baseline.result.data.size());
    Require(area["modelSha256"] == hash.FinishHex(), "sidecar binds to exact model bytes");
    Require(area["coordinateSystem"] == "GLTF_Y_UP" && area["schemaVersion"] == 1, "sidecar coordinate contract");
    std::set<int> entities;
    for (const auto& light : area["lights"])
    {
        entities.insert(light["entityIndex"].get<int>());
        const auto& q      = light["quaternion"];
        double      length = 0;
        for (const auto& v : q) length += v.get<double>() * v.get<double>();
        Require(std::abs(length - 1) < s_UnitTolerance, "area light orientation is normalized");
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double normalY = -2 * (y * z - x * w);
        Require(normalY > 1 - s_UnitTolerance, "floor emitters face glTF positive Y");
    }
    Require(entities.size() > 1, "shared brush models produce separate emitter instances");
    BSPConversionOptions options;
    options.scaleFactor = 0.0254f;
    const auto scaled   = AreaSidecar(Convert(file, options).result);
    Require(area["lights"].size() == scaled["lights"].size(), "scale preserves emitter count");
    for (std::size_t i = 0; i < area["lights"].size(); ++i)
    {
        Require(area["lights"][i]["radianceRGB"] == scaled["lights"][i]["radianceRGB"], "scale preserves radiance");
        Require(std::abs(area["lights"][i]["width"].get<double>() * options.scaleFactor - scaled["lights"][i]["width"].get<double>()) < s_PositionTolerance, "scale applies once to emitter width");
        for (int c = 0; c < 3; ++c)
            Require(std::abs(area["lights"][i]["position"][c].get<double>() * options.scaleFactor - scaled["lights"][i]["position"][c].get<double>()) < s_PositionTolerance, "entity translations use root scale once");
    }
    options                   = {};
    options.bExportAreaLights = false;
    const auto disabled       = Convert(file, options);
    Require(AreaSidecar(disabled.result)["lights"].empty() && !AreaSidecar(disabled.result)["enabled"].get<bool>() && baseline.result.cacheKey != disabled.result.cacheKey, "disable writes current empty descriptor and changes cache key");
    options               = {};
    options.emissiveScale = 0;
    Require(AreaSidecar(Convert(file, options).result)["lights"].empty(), "zero emission has no area lights");
    options         = {};
    options.format  = OutputFormat::GLTF;
    const auto text = Convert(file, options);
    Require(text.result.bSuccess && AreaSidecar(text.result)["modelFile"] == "converter_test.gltf", "text glTF has matching sidecar");
    for (const auto count : {0u, 65u})
    {
        options.maxAreaLightsPerGroup = count;
        const auto invalid            = Convert(file, options);
        Require(invalid.result.error == ConversionError::InvalidRequest && invalid.result.data.empty(), "invalid area budgets fail before publishing");
    }
    Require(AreaSidecar(Convert(MakeConverterTestBSP(), {}).result)["lights"].empty(), "ordinary and unresolved textures do not emit");
}

static void TestAreaLightFitting()
{
    constexpr std::uint32_t s_ImageSize    = 16;
    constexpr double        s_FitTolerance = 0.01;
    BSPDecodedImage         image{std::vector<std::uint8_t>(s_ImageSize * s_ImageSize * 4, 255), s_ImageSize, s_ImageSize};
    tinygltf::Material      material;
    material.emissiveFactor                                = {1, 1, 1};
    material.extensions["KHR_materials_emissive_strength"] = ToGLTFValue({{"emissiveStrength", 1.0}});
    const auto quad                                        = [](BSPPrimitive& p, float x, float y, float width, float height, float uvScale = 1.0f) {
        const auto    base = static_cast<std::uint32_t>(p.positions.size());
        BSPFaceRecord face;
        face.face       = static_cast<std::uint32_t>(p.faces.size());
        face.firstIndex = static_cast<std::uint32_t>(p.indices.size());
        face.indexCount = 6;
        p.faces.push_back(face);
        for (const auto& v : std::vector<Vector3>{{x, y, 0}, {x + width, y, 0}, {x + width, y + height, 0}, {x, y + height, 0}}) p.positions.push_back(v);
        for (const auto& uv : std::vector<Vector2>{{0, 0}, {uvScale, 0}, {uvScale, uvScale}, {0, uvScale}}) p.texcoords.push_back(uv);
        for (auto i : {0u, 1u, 2u, 0u, 2u, 3u}) p.indices.push_back(base + i);
    };
    const auto fit = [&](const BSPPrimitive& p, std::uint32_t budget = 16) {
        json stats = {{"emissiveFaceCount", 0}, {"groupCount", 0}, {"limitedGroups", json::array()}};
        return FitBSPAreaLightPrimitive(p, material, image, budget, stats, 0);
    };
    const auto energy = [](const json& lights) {
        double sum = 0;
        for (const auto& light : lights) sum += light["width"].get<double>() * light["height"].get<double>() * light["radianceRGB"][0].get<double>();
        return sum;
    };
    BSPPrimitive rectangle;
    quad(rectangle, 0, 0, 1, 1);
    auto result = fit(rectangle);
    Require(result.size() == 1 && std::abs(energy(result) - 1) < s_FitTolerance, "uniform quad fits one energy-preserving rectangle");
    BSPPrimitive tJunction;
    quad(tJunction, 0, 0, 1, 2);
    quad(tJunction, 1, 0, 1, 1);
    quad(tJunction, 1, 1, 1, 1);
    Require(fit(tJunction).size() == 1 && std::abs(energy(fit(tJunction)) - 4) < s_FitTolerance, "partial shared edges merge BSP T junctions");
    BSPPrimitive separated;
    quad(separated, 0, 0, 1, 1);
    quad(separated, 3, 0, 1, 1);
    Require(fit(separated).size() == 2, "disconnected same-material panels remain separate");
    BSPPrimitive shape;
    quad(shape, 0, 0, 1, 2);
    quad(shape, 1, 0, 1, 1);
    Require(fit(shape).size() > 1 && std::abs(energy(fit(shape)) - 3) < s_FitTolerance, "L shape splits and preserves sampled energy");
    for (std::size_t i = 0; i < image.texels.size(); i += 4) image.texels[i] = image.texels[i + 1] = image.texels[i + 2] = 128;
    const double linear = std::pow((128.0 / 255 + 0.055) / 1.055, 2.4);
    Require(std::abs(energy(fit(rectangle)) - linear) < s_FitTolerance, "emissive pixels are decoded to linear light");
    for (std::uint32_t y = 0; y < s_ImageSize; ++y)
        for (std::uint32_t x = 0; x < s_ImageSize; ++x)
        {
            const auto i     = (y * s_ImageSize + x) * 4;
            const auto value = (x >= 2 && x < 5) || (x >= 11 && x < 14) ? 255 : 0;
            image.texels[i] = image.texels[i + 1] = image.texels[i + 2] = static_cast<std::uint8_t>(value);
        }
    result = fit(rectangle);
    Require(result.size() > 1 && result.size() <= 16, "separate texture stripes split within budget");
    Require(std::abs(energy(result) - energy(fit(rectangle, 1))) < s_FitTolerance, "budget does not change sampled emission integral");
    BSPPrimitive repeated;
    quad(repeated, 0, 0, 1, 1, -2);
    Require(std::abs(energy(fit(repeated)) - energy(result)) < s_FitTolerance, "negative repeated UVs preserve average energy");
    material.alphaMode = "MASK";
    for (std::size_t i = 3; i < image.texels.size(); i += 4) image.texels[i] = 0;
    Require(fit(rectangle).empty(), "alpha-clipped samples emit no light");
    BSPPrimitive degenerate;
    quad(degenerate, 0, 0, 0, 0);
    Require(fit(degenerate).empty(), "zero-area geometry is ignored");
}

static void TestEmissiveScale()
{
    constexpr float  s_DefaultScale        = 4;
    constexpr double s_NormalizedIntensity = 2000.0 / 255;
    const auto       file                  = MakeBSPWithFloorTexture("+0~LIGHT2A");
    const auto       baseline              = Convert(file, {});
    Require(baseline.result.bSuccess, "the texture light fixture converts");
    const auto  baselineDocument = ParseGLB(baseline.result.data);
    const auto& baselineMaterial = FaceMaterial(baselineDocument, ModelMesh(baselineDocument, 0, "surface"), 0);
    const float scales[]         = {0, 1, 0.5f, 8};

    BSPConversionOptions options;
    Require(options.emissiveScale == s_DefaultScale, "the public default emissive multiplier is four");

    for (const auto scale : scales)
    {
        options.emissiveScale = scale;
        ConversionRun run;
        const auto    document = ConvertAndParse(file, options, run);
        const auto    surface  = ModelMesh(document, 0, "surface");
        const auto&   material = FaceMaterial(document, surface, 0);
        Require(baseline.result.cacheKey != run.result.cacheKey && baseline.result.sourceHash == run.result.sourceHash && baseline.result.textureHash == run.result.textureHash, "the emissive multiplier changes the cache key without changing source or texture hashes");
        Require(baselineMaterial["pbrMetallicRoughness"]["baseColorTexture"] == material["pbrMetallicRoughness"]["baseColorTexture"], "the emission multiplier preserves the base texture");

        if (scale == 0)
        {
            Require(!material.contains("emissiveTexture") && !material.value("extensions", json::object()).contains("KHR_materials_emissive_strength"), "zero disables texture emission");
            const auto used = document.gltf.value("extensionsUsed", std::vector<std::string>{});
            Require(std::count(used.begin(), used.end(), "KHR_materials_emissive_strength") == 0, "disabled emission does not declare the emission extension");
            WriteOutput("converter_emissive_off.glb", run.result.data);
        }
        else
        {
            Require(baselineMaterial["emissiveFactor"] == material["emissiveFactor"], "the multiplier preserves the emission color");
            Require(std::fabs(scale * s_NormalizedIntensity - material["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"].get<double>()) < s_UnitTolerance, "custom and fractional emission multipliers are applied");
        }

        const auto& grate = FaceMaterial(document, surface, TestBSPGrateSlot);
        Require(grate["alphaMode"] == "MASK" && !grate.contains("emissiveTexture"), "emission scaling preserves unlisted masked materials");
    }

    for (const auto invalid : {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()})
    {
        options.emissiveScale = invalid;
        const auto run        = Convert(file, options);
        Require(run.result.error == ConversionError::InvalidRequest && !run.result.errorMessage.empty() && run.result.data.empty(), "negative, nonfinite and overflowing emission scales are rejected without output");
    }
}

static std::vector<std::uint8_t> MakeBSPWithLightFields(const std::string& fields, const std::string& classname = "light")
{
    std::string       entities      = ConverterTestEntities;
    const std::string originalLight = "\"_light\" \"255 255 255 200\"\n";
    entities.replace(entities.find(originalLight), originalLight.size(), fields);
    const std::string originalClass = "\"classname\" \"light\"";
    entities.replace(entities.find(originalClass), originalClass.size(), "\"classname\" \"" + classname + "\"");

    return AssembleTestBSP(MakeConverterTestLumps(entities.c_str()));
}

static std::size_t PointLightCount(const Document& document)
{
    return document.gltf.value("extensions", json::object()).value("KHR_lights_punctual", json::object()).value("lights", json::array()).size();
}

static const json& EntityPointLight(const Document& document)
{
    const auto entity = FindEntityNode(document, ConverterTestLightEntity);
    Require(entity >= 0 && PointLightCount(document) == 1, "the original light entity and one punctual light are exported");
    const auto& node = document.gltf["nodes"][entity];
    Require(!node.contains("mesh") && !node.value("extensions", json::object()).contains("KHR_lights_punctual"), "the original entity remains an empty node");
    Require(node.value("children", json::array()).size() == 1, "the entity has one light child");
    const auto  child     = node["children"][0].get<int>();
    const auto& lightNode = document.gltf["nodes"][child];
    Require(lightNode["name"] == "ent_0006_light_PointLight" && !lightNode.contains("extras") && !lightNode.contains("mesh"), "the light child has no duplicate entity identity or mesh");
    Require(NodeMatrix(lightNode) == Identity() && GlobalMatrix(document, child) == GlobalMatrix(document, entity), "the light child inherits the entity transform exactly");
    const auto index = lightNode["extensions"]["KHR_lights_punctual"]["light"].get<int>();
    Require(index == 0, "the child references its exported light");
    const auto& light = document.gltf["extensions"]["KHR_lights_punctual"]["lights"][index];
    Require(light["type"] == "point" && !light.contains("range"), "the light is a point with inverse-square falloff and no range cutoff");

    return light;
}

static void TestPointLightEntities()
{
    ConversionRun baselineRun;
    const auto    baseline      = ConvertAndParse(MakeConverterTestBSP(), {}, baselineRun);
    const auto&   baselineLight = EntityPointLight(baseline);
    Require(std::fabs(baselineLight.value("intensity", 1.0) - 200) < s_UnitTolerance, "the fixture light exports its brightness");
    const auto entity = FindEntityNode(baseline, ConverterTestLightEntity);
    Require(Near(Transform(GlobalMatrix(baseline, entity), {}), {0, 64, 0}), "the point light uses the root coordinate conversion");
    const auto used     = baseline.gltf.value("extensionsUsed", std::vector<std::string>{});
    const auto required = baseline.gltf.value("extensionsRequired", std::vector<std::string>{});
    Require(std::count(used.begin(), used.end(), "KHR_lights_punctual") == 1 && std::count(required.begin(), required.end(), "KHR_lights_punctual") == 0, "the light extension is used once and optional");

    struct LightCase
    {
        const char*           fields;
        std::array<double, 3> contribution;
    };
    const LightCase cases[] = {
        {"", {300, 300, 300}},
        {"\"_light\" \"75\"\n", {75, 75, 75}},
        {"\"_light\" \"12 24 48\"\n", {12, 24, 48}},
        {"\"_light\" \"255 128 0 510\"\n", {510, 256, 0}},
        {"\"_light\" \"510 255 0 100\"\n", {200, 100, 0}},
        {"\"_light\" \"0\"\n", {0, 0, 0}},
        {"\"_light\" \"0 0 0\"\n", {0, 0, 0}},
        {"\"_light\" \"255 128 64 0\"\n", {0, 0, 0}},
        {"\"_light\" \"bad\"\n\"_light \" \" 255 128 0 510 \"\n", {510, 256, 0}},
        {"\"_light\" \"1e2\"\n\"spawnflags\" \"1\"\n\"pattern\" \"az\"\n", {100, 100, 100}}};

    for (const auto& test : cases)
    {
        ConversionRun run;
        const auto    document  = ConvertAndParse(MakeBSPWithLightFields(test.fields), {}, run);
        const auto&   light     = EntityPointLight(document);
        const auto    color     = light.value("color", std::array<double, 3>{1, 1, 1});
        const auto    intensity = light.value("intensity", 1.0);

        if (intensity == 0)
            Require(color == std::array<double, 3>{1, 1, 1}, "zero source contributions export a white zero-intensity light");

        for (std::size_t channel = 0; channel < color.size(); ++channel)
            Require(color[channel] >= 0 && color[channel] <= 1 && std::fabs(color[channel] * intensity - test.contribution[channel]) < s_UnitTolerance, "light color times intensity preserves linear source contributions");

        Require(!HasDiagnostic(run.result, DiagnosticCode::InvalidEntityValue), "valid light formats and ignored control fields cause no light warning");
        Require(run.result.statistics.entityCount == baselineRun.result.statistics.entityCount && run.result.statistics.pointEntityNodeCount == baselineRun.result.statistics.pointEntityNodeCount, "light children do not change entity statistics");
    }

    for (const auto* szInvalid : {"", "bad", "1 2", "1 2 3 4 5", "1 2 3 junk", "-1", "1 -2 3 4", "1 2 3 -4", "nan", "inf", "1e999", "1+2+3", "1 2 3 4junk"})
    {
        ConversionRun run;
        const auto    document = ConvertAndParse(MakeBSPWithLightFields(std::string("\"_light\" \"") + szInvalid + "\"\n"), {}, run);
        Require(PointLightCount(document) == 0 && FindEntityNode(document, ConverterTestLightEntity) >= 0, "invalid lights retain their original entity without a light child");
        Require(HasDiagnostic(run.result, DiagnosticCode::InvalidEntityValue), "invalid light values are reported as warnings");
    }

    for (const auto* szClass : {"light_spot", "light_environment", "Light"})
    {
        ConversionRun run;
        const auto    document = ConvertAndParse(MakeBSPWithLightFields("\"_light\" \"100\"\n", szClass), {}, run);
        Require(PointLightCount(document) == 0 && FindEntityNode(document, ConverterTestLightEntity) >= 0, "only the exact light class is converted");
    }

    std::string multipleEntities = ConverterTestEntities;
    multipleEntities += "{\n\"classname\" \"light\"\n\"origin\" \"10 20 30\"\n\"_light\" \"0 100 0\"\n}\n";
    ConversionRun multipleRun;
    const auto    multiple = ConvertAndParse(AssembleTestBSP(MakeConverterTestLumps(multipleEntities.c_str())), {}, multipleRun);
    Require(PointLightCount(multiple) == 2, "each light entity generates a separate light");
    const auto other = FindNode(multiple, "ent_0010_light_PointLight");
    Require(other >= 0 && multiple.gltf["nodes"][other]["extensions"]["KHR_lights_punctual"]["light"] == 1, "each child references its own light index");
    Require(Near(Transform(GlobalMatrix(multiple, other), {}), {10, 30, -20}), "multiple lights inherit independent entity positions");
    WriteOutput("converter_point_lights.glb", multipleRun.result.data);
}

static void TestPointLightIntensityScale()
{
    constexpr double s_FixtureBrightness = 200;
    const auto       file                = MakeConverterTestBSP();
    const auto       baseline            = Convert(file, {});
    Require(baseline.result.bSuccess, "the point light fixture converts");
    const auto           baselineDocument = ParseGLB(baseline.result.data);
    const auto&          baselineEntity   = baselineDocument.gltf["nodes"][FindEntityNode(baselineDocument, ConverterTestLightEntity)];
    BSPConversionOptions options;
    Require(options.pointLightIntensityScale == 1, "the public point light multiplier defaults to one");

    for (const auto scale : {0.0f, 0.5f, 4.0f})
    {
        options.pointLightIntensityScale = scale;
        ConversionRun run;
        const auto    document = ConvertAndParse(file, options, run);
        const auto&   entity   = document.gltf["nodes"][FindEntityNode(document, ConverterTestLightEntity)];
        Require(baselineEntity["name"] == entity["name"] && baselineEntity["extras"] == entity["extras"] && NodeMatrix(baselineEntity) == NodeMatrix(entity), "point light scaling preserves original entity identity, metadata and transform");
        Require(baseline.result.cacheKey != run.result.cacheKey && baseline.result.sourceHash == run.result.sourceHash && baseline.result.textureHash == run.result.textureHash, "the point light multiplier changes only the options part of the cache key");
        Require(baselineDocument.gltf["materials"] == document.gltf["materials"], "point light scaling is independent of materials and texture emission");

        if (scale == 0)
        {
            Require(PointLightCount(document) == 0 && !entity.contains("children"), "zero disables light conversion and keeps the entity empty");
            const auto used = document.gltf.value("extensionsUsed", std::vector<std::string>{});
            Require(std::count(used.begin(), used.end(), "KHR_lights_punctual") == 0, "disabled lights do not declare the extension");
            WriteOutput("converter_point_lights_off.glb", run.result.data);
        }
        else
            Require(std::fabs(s_FixtureBrightness * scale - EntityPointLight(document).value("intensity", 1.0)) < s_UnitTolerance, "custom light multipliers scale intensity");
    }

    options.pointLightIntensityScale = 1;

    for (const auto scale : {0.5f, 2.0f})
    {
        options.scaleFactor = scale;
        ConversionRun run;
        const auto    document = ConvertAndParse(file, options, run);
        Require(std::fabs(s_FixtureBrightness * scale * scale - EntityPointLight(document).value("intensity", 1.0)) < s_UnitTolerance, "scene scaling compensates light intensity quadratically");
        const auto child = FindNode(document, "ent_0006_light_PointLight");
        Require(Near(Transform(GlobalMatrix(document, child), {}), {0, 64 * scale, 0}), "the light position scales with the scene");
    }

    options.scaleFactor = 1;

    for (const auto invalid : {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()})
    {
        options.pointLightIntensityScale = invalid;
        const auto run                   = Convert(file, options);
        Require(run.result.error == ConversionError::InvalidRequest && !run.result.errorMessage.empty() && run.result.data.empty() && run.result.externalFiles.empty(), "invalid or overflowing point light scales fail without publishing output");
    }

    options.pointLightIntensityScale = 1;
    options.scaleFactor              = std::numeric_limits<float>::max();
    Require(Convert(file, options).result.error == ConversionError::InvalidRequest, "scene scale intensity overflow is rejected");
    options.scaleFactor = 1;
    Require(Convert(MakeBSPWithLightFields("\"_light\" \"1e300\"\n"), options).result.error == ConversionError::InvalidRequest, "valid source numbers with unrepresentable final intensity fail");

    options.bEmbedTextures         = false;
    const auto overflowingExternal = Convert(MakeBSPWithLightFields("\"_light\" \"1e300\"\n"), options);
    Require(overflowingExternal.result.error == ConversionError::InvalidRequest && overflowingExternal.result.data.empty() && overflowingExternal.result.externalFiles.empty(), "intensity failure does not publish an asset or external images");
    options.bEmbedTextures = true;

    options.pointLightIntensityScale = 0;
    ConversionRun disabledRun;
    const auto    disabled = ConvertAndParse(MakeBSPWithLightFields("\"_light\" \"bad\"\n"), options, disabledRun);
    Require(PointLightCount(disabled) == 0 && !HasDiagnostic(disabledRun.result, DiagnosticCode::InvalidEntityValue), "disabled conversion skips light parsing and warnings");
    ConversionRun overflowDisabledRun;
    ConvertAndParse(MakeBSPWithLightFields("\"_light\" \"1e300\"\n"), options, overflowDisabledRun);

    options.pointLightIntensityScale = 1;
    options.bPreserveEntityKeyValues = false;
    ConversionRun noKeysRun;
    const auto    noKeys = ConvertAndParse(file, options, noKeysRun);
    Require(PointLightCount(noKeys) == 1 && !noKeys.gltf["nodes"][FindEntityNode(noKeys, ConverterTestLightEntity)]["extras"]["goldsrc"].contains("keyvalues"), "light conversion does not require exporting raw keys");
    options.bPreservePointEntities = false;
    ConversionRun omittedRun;
    const auto    omitted = ConvertAndParse(file, options, omittedRun);
    Require(PointLightCount(omitted) == 0 && FindEntityNode(omitted, ConverterTestLightEntity) < 0, "omitting point entities also omits their lights");
}

//16.8 Special surfaces keep their GoldSrc semantics; textures come from the resolver, WADs or the BSP itself.
static void TestSpecialSurfaces()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    const auto surface = ModelMesh(document, 0, "surface");
    const auto water   = ModelMesh(document, 0, "water");

    const auto& waterMaterial = FaceMaterial(document, water, TestBSPWaterFace);
    Require(waterMaterial["extras"]["goldsrc"]["surfaceType"] == "water" && waterMaterial["extras"]["goldsrc"]["liquidType"] == "water" && waterMaterial["doubleSided"] == true, "water material");
    Require(waterMaterial["pbrMetallicRoughness"].contains("baseColorTexture"), "the water texture is resolved from the WAD");

    const auto& grate = FaceMaterial(document, surface, TestBSPGrateSlot);
    Require(grate["alphaMode"] == "MASK" && grate["extras"]["goldsrc"]["alphaTest"] == true && grate["extras"]["goldsrc"]["embedded"] == true, "masked textures are alpha tested");

    const auto& trigger = FaceMaterial(document, surface, TestBSPTriggerFace);
    Require(trigger["extras"]["goldsrc"]["texSpecial"] == true && trigger["extras"]["goldsrc"]["lightmapped"] == false && trigger["extras"]["goldsrc"]["scroll"]["width"] == TestBSPTextureSize, "TEX_SPECIAL surfaces are unlit and tiled");

    const auto& wall = FaceMaterial(document, surface, TestBSPWallSlot);
    Require(wall["extras"]["goldsrc"]["missingTexture"] == true && !wall["pbrMetallicRoughness"].contains("baseColorTexture"), "unresolved textures use a placeholder");
    Require(wall["extras"]["goldsrc"]["originalTextureName"] == "wall" && wall["extras"]["goldsrc"]["miptexIndex"] == TestBSPWallSlot, "the placeholder keeps the texture's identity");
    Require(HasDiagnostic(run.result, DiagnosticCode::MissingTexture) && run.result.statistics.missingTextureCount == 1, "the missing texture is reported");

    //The floor's embedded pixels go through its palette.
    const auto& floor   = FaceMaterial(document, surface, 0);
    const auto& texture = document.gltf["textures"][floor["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<int>()];
    const auto& image   = document.gltf["images"][texture["source"].get<int>()];

    std::size_t bytes{};
    const auto* pPNG = ViewBytes(document, image["bufferView"].get<int>(), bytes);

    int  width{}, height{}, channels{};
    auto pTexels = stbi_load_from_memory(pPNG, static_cast<int>(bytes), &width, &height, &channels, 4);
    Require(pTexels && width == static_cast<int>(TestBSPFloorWidth) && height == static_cast<int>(TestBSPFloorHeight), "the embedded floor decodes");

    constexpr std::uint32_t s_SampleTexel = 77;
    const auto              color         = ConverterTestPaletteColor(ConverterTestTexel(s_SampleTexel));
    Require(pTexels[s_SampleTexel * 4] == color[0] && pTexels[s_SampleTexel * 4 + 1] == color[1] && pTexels[s_SampleTexel * 4 + 2] == color[2] && pTexels[s_SampleTexel * 4 + 3] == 255, "palette colors");
    stbi_image_free(pTexels);

    //The grate's index 255 is transparent black.
    const auto& grateTexture = document.gltf["textures"][grate["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<int>()];
    pPNG                     = ViewBytes(document, document.gltf["images"][grateTexture["source"].get<int>()]["bufferView"].get<int>(), bytes);
    pTexels                  = stbi_load_from_memory(pPNG, static_cast<int>(bytes), &width, &height, &channels, 4);
    Require(pTexels && pTexels[ConverterTestTransparent * 4 + 3] == 0 && pTexels[ConverterTestTransparent * 4] == 0 && pTexels[4 + 3] == 255, "index 255 of '{' textures is transparent");
    stbi_image_free(pTexels);

    //Each image is shared: the floor, used by models 0 and 2, is resolved once.
    Require(std::count(run.requested.begin(), run.requested.end(), "floor") == 1, "every texture is resolved once");
}

//A TEX_SPECIAL face wider than GoldSrc's 512-unit surface extent limit (as the aaatrigger brushes of c1a3b and c1a4)
//converts although the caller binds no texture: the file is validated with every miptex slot bound, as the engine
//loads it, so the face keeps its texinfo flags.
static void TestSpecialFaceExtents()
{
    //The trigger's texture axes scaled so that its quad spans 1024 texels on both axes.
    constexpr float       s_AxisScale      = 16;
    constexpr std::size_t s_TexInfoSOffset = 0;
    constexpr std::size_t s_TexInfoTOffset = 5 * sizeof(float);
    const auto            triggerTexInfo   = static_cast<std::size_t>(TestBSPTriggerSlot) * TestBSPTexInfoBytes;

    std::uint32_t scaleBits{};
    std::memcpy(&scaleBits, &s_AxisScale, sizeof(scaleBits));

    auto lumps = MakeConverterTestLumps();
    PatchUInt32(lumps[BSPTexInfoLump], triggerTexInfo + s_TexInfoSOffset, scaleBits);
    PatchUInt32(lumps[BSPTexInfoLump], triggerTexInfo + s_TexInfoTOffset, scaleBits);

    ConversionRun run;
    const auto    document = ConvertAndParse(AssembleTestBSP(lumps), {}, run);

    const auto faces = MeshFaces(document, ModelMesh(document, ConverterTestSharedModel, "surface"));
    Require(faces.count(TestBSPTriggerFace) == 1 && FaceMaterial(document, ModelMesh(document, ConverterTestSharedModel, "surface"), TestBSPTriggerFace)["extras"]["goldsrc"]["texSpecial"] == true,
            "a TEX_SPECIAL face beyond the surface extent limit converts without caller bindings");
}

//A miptex whose name contains '~' is asked for by its stored name, as the engine finds it in the WADs, and keeps the
//engine's name (third character blanked) as its texture name.
static void TestTildeTextureName()
{
    constexpr char        s_StoredName[] = "~wall";
    constexpr char        s_EngineName[] = "~w ll";
    constexpr std::size_t s_MiptexTable  = sizeof(std::int32_t);
    constexpr std::size_t s_NameBytes    = 16;

    auto       lumps  = MakeConverterTestLumps();
    auto&      lump   = lumps[BSPTextureLump];
    const auto record = ReadUInt32(lump, s_MiptexTable + TestBSPWallSlot * sizeof(std::int32_t));

    std::fill(lump.begin() + record, lump.begin() + record + s_NameBytes, std::uint8_t{0});
    std::copy(std::begin(s_StoredName), std::end(s_StoredName) - 1, lump.begin() + record);

    const auto file = AssembleTestBSP(lumps);
    const auto wad  = MakeConverterTestWAD(s_StoredName, TestBSPTextureSize, TestBSPTextureSize);

    std::vector<std::string> requested;

    ConversionRequest request;
    request.asset.szIdentifier = "maps/converter_test.bsp";
    request.asset.pFile        = file.data();
    request.asset.fileBytes    = static_cast<std::uint32_t>(file.size());
    request.textureResolver    = [&wad, &requested](const TextureRequest& textureRequest, ResolvedTexture& texture) {
        requested.push_back(textureRequest.szName);
        return textureRequest.kind == TextureKind::Miptex && FindWAD3Texture(wad.data(), wad.size(), textureRequest.szName, &texture);
    };

    const auto result = ConvertRequest(request);
    Require(result.bSuccess && std::find(requested.begin(), requested.end(), s_StoredName) != requested.end(), "a '~' miptex is asked for by its stored name");

    const auto  document = ParseGLB(result.data);
    const auto& wall     = FaceMaterial(document, ModelMesh(document, 0, "surface"), TestBSPWallSlot);
    Require(wall["pbrMetallicRoughness"].contains("baseColorTexture") && wall["extras"]["goldsrc"]["missingTexture"] == false, "a '~' miptex resolves from the WAD");
    Require(wall["extras"]["goldsrc"]["originalTextureName"] == s_EngineName && wall["extras"]["goldsrc"]["storedTextureName"] == s_StoredName, "a '~' miptex keeps the engine's name and its stored name");
}

//16.9 By default the GLB holds no BSP spatial data, and everything that renders is still there.
static void TestDefaultSpatialDataOff()
{
    ConversionRun run;
    const auto    document = ConvertAndParse(MakeConverterTestBSP(), {}, run);

    Require(!document.Goldsrc().contains("spatialData") && run.result.statistics.visibilityDataBytes == 0 && run.result.statistics.collisionDataBytes == 0, "no spatial data by default");

    //Every buffer view is an accessor's or an image's.
    std::set<int> used;

    for (const auto& accessor : document.gltf["accessors"])
        used.insert(accessor["bufferView"].get<int>());

    for (const auto& image : document.gltf["images"])
        used.insert(image["bufferView"].get<int>());

    Require(used.size() == document.gltf["bufferViews"].size(), "no buffer view holds BSP spatial data");
    Require(!HasDiagnostic(run.result, DiagnosticCode::ConversionFailure) && run.result.statistics.skyFaceCount == 2 && run.result.statistics.waterFaceCount == 2 && run.result.statistics.brushEntityNodeCount == 4, "rendering data is complete without spatial data");
}

//Raw BSP30 records read back from an exported buffer view.
static std::int32_t ReadInt32(const std::uint8_t* pBytes)
{
    std::int32_t value;
    std::memcpy(&value, pBytes, sizeof(value));

    return value;
}

static std::int16_t ReadInt16(const std::uint8_t* pBytes)
{
    std::int16_t value;
    std::memcpy(&value, pBytes, sizeof(value));

    return value;
}

static float ReadFloat(const std::uint8_t* pBytes)
{
    float value;
    std::memcpy(&value, pBytes, sizeof(value));

    return value;
}

//A lump of the spatial data: its bytes and record count.
struct SpatialLump
{
    const std::uint8_t* pData{};
    std::size_t         bytes{};
};

static SpatialLump GetSpatialLump(const Document& document, const json& description)
{
    SpatialLump lump;

    if (description.contains("bufferView"))
    {
        lump.pData = ViewBytes(document, description["bufferView"].get<int>(), lump.bytes);
        Require(lump.bytes == description["byteLength"].get<std::size_t>(), "lump lengths match their buffer view");
    }

    return lump;
}

constexpr std::size_t s_PlaneBytes     = 20;
constexpr std::size_t s_NodeBytes      = 24;
constexpr std::size_t s_LeafBytes      = 28;
constexpr std::size_t s_ClipNodeBytes  = 8;
constexpr std::size_t s_NodeChildren   = 4;
constexpr std::size_t s_LeafVisibility = 4;
constexpr std::size_t s_LeafFirstMark  = 20;
constexpr std::size_t s_LeafMarkCount  = 22;

static double PlaneDistance(const SpatialLump& planes, std::int32_t plane, const Vec3& point)
{
    const auto* pPlane = planes.pData + plane * s_PlaneBytes;

    return ReadFloat(pPlane) * point.x + ReadFloat(pPlane + 4) * point.y + ReadFloat(pPlane + 8) * point.z - ReadFloat(pPlane + 12);
}

//16.10 Exported visibility answers PVS queries as the world built from the BSP does.
static void TestVisibilityExport()
{
    BSPConversionOptions options;
    options.bEmbedBSPVisibilityData = true;

    const auto    file = MakeConverterTestBSP();
    ConversionRun run;
    const auto    document = ConvertAndParse(file, options, run);
    WriteOutput("converter_visibility.glb", run.result.data);

    const auto& visibility = document.Goldsrc()["spatialData"]["visibility"];
    Require(visibility["present"] == true && visibility["encoding"] == "goldsrc-bsp30" && !document.Goldsrc()["spatialData"].contains("collision"), "visibility alone is exported");
    Require(run.result.statistics.visibilityDataBytes == visibility["byteLength"].get<std::uint64_t>() && run.result.statistics.visibilityDataBytes > 0, "visibility byte count");

    const auto& lumps        = visibility["lumps"];
    const auto  planes       = GetSpatialLump(document, lumps["planes"]);
    const auto  nodes        = GetSpatialLump(document, lumps["nodes"]);
    const auto  leaves       = GetSpatialLump(document, lumps["leaves"]);
    const auto  markSurfaces = GetSpatialLump(document, lumps["markSurfaces"]);
    const auto  rows         = GetSpatialLump(document, lumps["visibility"]);
    const auto  rootNode     = visibility["rootNode"].get<std::int32_t>();
    const auto  leafCount    = visibility["visibleLeafCount"].get<std::uint32_t>();

    //The reference: the BSPWorld of the same file.
    BSPAssetView asset;
    asset.pFile     = file.data();
    asset.fileBytes = static_cast<std::uint32_t>(file.size());

    BSPWorld world;
    Require(BuildBSPWorld(asset, world) == ErrorCode::None, "build the reference world");

    const Vec3 points[] = {{10, 10, 5}, {2000, 0, 0}, {10, 10, -5}};

    for (const auto& point : points)
    {
        //Leaf of the point from the exported tree.
        auto child = rootNode;

        while (child >= 0)
        {
            const auto* pNode = nodes.pData + child * s_NodeBytes;
            child             = ReadInt16(pNode + s_NodeChildren + (PlaneDistance(planes, ReadInt32(pNode), point) > 0 ? 0 : sizeof(std::int16_t)));
        }

        const auto leaf = static_cast<std::uint32_t>(-1 - child);
        Require(leaf == FindBSPLeaf(world, {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)}), "the exported tree finds the same leaf");

        if (leaf == BSPSolidLeaf)
            continue;

        //Decode the leaf's row and collect the faces its visible leaves list.
        const auto           offset = ReadInt32(leaves.pData + leaf * s_LeafBytes + s_LeafVisibility);
        std::vector<uint8_t> row((leafCount + 7) / 8);

        for (std::size_t written = 0, in = static_cast<std::size_t>(offset); written < row.size();)
        {
            const auto value = rows.pData[in++];

            if (value)
                row[written++] = value;
            else
                written += rows.pData[in++];
        }

        std::set<std::uint32_t> faces;

        for (std::uint32_t visible = 0; visible < leafCount; ++visible)
        {
            if (!((row[visible / 8] >> (visible % 8)) & 1))
                continue;

            const auto*   pLeaf = leaves.pData + (visible + 1) * s_LeafBytes;
            std::uint16_t first, count;
            std::memcpy(&first, pLeaf + s_LeafFirstMark, sizeof(first));
            std::memcpy(&count, pLeaf + s_LeafMarkCount, sizeof(count));

            for (std::uint32_t mark = first; mark < first + count; ++mark)
            {
                std::uint16_t face;
                std::memcpy(&face, markSurfaces.pData + mark * sizeof(face), sizeof(face));
                faces.insert(face);
            }
        }

        //The drawn faces among them are the world's visible surfaces.
        std::vector<std::uint8_t> visibleSurfaces;
        std::vector<std::uint8_t> visibleWater;
        FindBSPVisibleSurfaces(world, leaf, visibleSurfaces, visibleWater);

        for (std::uint32_t face = 0; face < TestBSPFaceCount; ++face)
        {
            const auto* pFace = FindBSPSubmodelFace(world, 0, face);

            if (pFace->surface != BSPNoSurface)
                Require((faces.count(face) == 1) == (visibleSurfaces[pFace->surface] != 0), "the exported PVS shows the same surfaces");
        }
    }
}

//16.11 Exported collision data keeps the clipnodes, hulls and contents.
static void TestCollisionExport()
{
    BSPConversionOptions options;
    options.bEmbedBSPCollisionData = true;

    const auto    file = MakeConverterTestBSP();
    ConversionRun run;
    const auto    document = ConvertAndParse(file, options, run);
    WriteOutput("converter_collision.glb", run.result.data);

    const auto& collision = document.Goldsrc()["spatialData"]["collision"];
    Require(collision["present"] == true && !document.Goldsrc()["spatialData"].contains("visibility") && run.result.statistics.collisionDataBytes > 0, "collision alone is exported");
    Require(collision["hulls"].size() == 4 && collision["hulls"][1]["mins"] == json::array({-16.0, -16.0, -36.0}) && collision["models"].size() == ConverterTestModelCount, "hulls and model roots");

    const auto planes    = GetSpatialLump(document, collision["lumps"]["planes"]);
    const auto clipNodes = GetSpatialLump(document, collision["lumps"]["clipNodes"]);
    Require(clipNodes.bytes == 2 * s_ClipNodeBytes, "both clipnodes are exported");

    //Hull 1 point contents through the exported clipnodes from model 0's root.
    auto contents = [&](const Vec3& point) {
        auto node = collision["models"][0]["headNodes"][1].get<std::int32_t>();

        while (node >= 0)
        {
            const auto* pClipNode = clipNodes.pData + node * s_ClipNodeBytes;
            node                  = ReadInt16(pClipNode + sizeof(std::int32_t) + (PlaneDistance(planes, ReadInt32(pClipNode), point) >= 0 ? 0 : sizeof(std::int16_t)));
        }

        return node;
    };

    Require(contents({10, 10, 5}) == ConverterTestContentsEmpty && contents({10, 10, -5}) == ConverterTestContentsSolid && contents({2000, 0, -5}) == ConverterTestContentsEmpty, "hull contents");

    //Both kinds share the lumps they have in common.
    options.bEmbedBSPVisibilityData = true;

    ConversionRun bothRun;
    const auto    both = ConvertAndParse(file, options, bothRun);
    const auto&   data = both.Goldsrc()["spatialData"];
    Require(data["visibility"]["lumps"]["planes"]["bufferView"] == data["collision"]["lumps"]["planes"]["bufferView"], "shared lumps are stored once");
}

//16.12 The Blue Shift layout converts to the same asset.
static void TestBlueShift()
{
    ConversionRun goldsrcRun;
    ConversionRun blueShiftRun;

    auto goldsrc   = ConvertAndParse(MakeConverterTestBSP(BSPLayout::GoldSrc), {}, goldsrcRun);
    auto blueShift = ConvertAndParse(MakeConverterTestBSP(BSPLayout::BlueShift), {}, blueShiftRun, BSPLayout::BlueShift);
    WriteOutput("converter_blueshift.glb", blueShiftRun.result.data);

    Require(blueShift.Goldsrc()["sourceFormat"] == "BSP30_BlueShift", "the layout is recorded");

    for (auto* pDocument : {&goldsrc, &blueShift})
    {
        auto& extras = pDocument->gltf["asset"]["extras"]["goldsrc"];
        extras.erase("sourceFormat");
        extras.erase("sourceHash");
    }

    Require(goldsrc.gltf == blueShift.gltf && goldsrc.bin == blueShift.bin, "both layouts convert to the same document");
}

//16.13 Invalid input fails with an explicit error.
static void TestInvalidInput()
{
    auto expectError = [](const std::vector<std::uint8_t>& file, ConversionError error, const char* pMessage) {
        const auto run = Convert(file, {});
        Require(!run.result.bSuccess && run.result.error == error && !run.result.errorMessage.empty() && run.result.data.empty(), pMessage);
    };

    auto truncated = MakeConverterTestBSP();
    truncated.resize(truncated.size() / 2);
    expectError(truncated, ConversionError::InvalidBSP, "a truncated file is rejected");

    auto badOffset = MakeConverterTestBSP();
    PatchUInt32(badOffset, BSPLumpTableOffset + BSPFaceLump * BSPLumpEntryBytes, static_cast<std::uint32_t>(badOffset.size()));
    expectError(badOffset, ConversionError::InvalidBSP, "a lump out of the file is rejected");

    auto lumps = MakeConverterTestLumps();
    PatchUInt16(lumps[BSPFaceLump], TestBSPFaceEdgesOffset, std::numeric_limits<std::uint16_t>::max());
    expectError(AssembleTestBSP(lumps), ConversionError::InvalidBSP, "a face with edges out of range is rejected");

    expectError(AssembleTestBSP(MakeConverterTestLumps("{\n\"classname\" \"worldspawn\"\n")), ConversionError::InvalidEntityLump, "an unterminated entity is rejected");
    expectError(AssembleTestBSP(MakeConverterTestLumps("\"classname\" \"worldspawn\"\n}\n")), ConversionError::InvalidEntityLump, "an entity without brace is rejected");
    expectError({}, ConversionError::InvalidRequest, "a request without file is rejected");

    BSPConversionOptions options;
    options.scaleFactor = 0;

    const auto file = MakeConverterTestBSP();
    Require(Convert(file, options).result.error == ConversionError::InvalidRequest, "a zero scale is rejected");

    //Caller bindings name miptex slots the texture lump holds, each once.
    GoldSrcRender::TextureBinding bindings[2];
    bindings[0].slot = TestBSPMiptexCount;

    ConversionRequest request;
    request.asset.szIdentifier = "maps/converter_test.bsp";
    request.asset.pFile        = file.data();
    request.asset.fileBytes    = static_cast<std::uint32_t>(file.size());
    request.asset.pTextures    = bindings;
    request.asset.textureCount = 1;
    Require(ConvertRequest(request).error == ConversionError::InvalidRequest, "a binding of a slot without miptex is rejected");

    bindings[0].slot = bindings[1].slot = TestBSPFloorSlot;
    request.asset.textureCount          = 2;
    Require(ConvertRequest(request).error == ConversionError::InvalidRequest, "a slot bound twice is rejected");

    request.asset.textureCount = 1;
    Require(ConvertRequest(request).bSuccess, "a caller binding is accepted");

    //The request view itself: an output sink is required and its strings are bounded.
    BSPConversionRequestView view;
    view.asset.pFile     = file.data();
    view.asset.fileBytes = static_cast<std::uint32_t>(file.size());

    const auto noSink = ConvertBSP(view);
    Require(noSink.error == ConversionError::InvalidRequest && noSink.szErrorMessage[0], "a request without output sink is rejected");

    request.asset.textureCount = 0;
    request.resourceName.assign(MaxConversionStringLength + 1, 'a');
    Require(ConvertRequest(request).error == ConversionError::InvalidRequest, "an overlong resource name is rejected");

    request.resourceName.clear();
    request.sourcePath.assign(MaxConversionStringLength + 1, 'a');
    Require(ConvertRequest(request).error == ConversionError::InvalidRequest, "an overlong source path is rejected");

    //A missing worldspawn is reported, model 0 still converted.
    ConversionRun run;
    const auto    document = ConvertAndParse(AssembleTestBSP(MakeConverterTestLumps("{\n\"classname\" \"info_null\"\n}\n")), {}, run);
    Require(HasDiagnostic(run.result, DiagnosticCode::MissingWorldspawn) && ModelMesh(document, 0, "surface") >= 0, "a missing worldspawn is reported");
}

//Output formats and delivery: deterministic GLB, .gltf, external images and the output sink.
static void TestOutputs()
{
    const auto file = MakeConverterTestBSP();

    const auto first  = Convert(file, {});
    const auto second = Convert(file, {});
    Require(first.result.data == second.result.data && first.result.cacheKey == second.result.cacheKey, "conversions are deterministic");
    Require(first.result.sourceHash.size() == 64 && first.result.cacheKey.find(first.result.sourceHash) != std::string::npos, "the cache key holds the source digest");
    Require(first.result.fileName == "converter_test.glb", "the file name comes from the identifier");
    Require(first.result.converterVersion == ConverterVersion && first.result.extrasSchemaVersion == ExtrasSchemaVersion, "the summary carries the module's versions");

    BSPConversionOptions options;
    options.format = OutputFormat::GLTF;

    const auto gltf = Convert(file, options);
    Require(gltf.result.bSuccess && gltf.result.data.front() == '{' && gltf.result.fileName == "converter_test.gltf", ".gltf output");

    const auto parsed = json::parse(gltf.result.data.begin(), gltf.result.data.end());
    Require(parsed["buffers"][0]["uri"].get<std::string>().rfind("data:application/octet-stream;base64,", 0) == 0, "the .gltf buffer is embedded");
    Require(gltf.result.cacheKey != first.result.cacheKey, "the cache key changes with the options");
    WriteOutput("converter_embedded.gltf", gltf.result.data);

    options.format         = OutputFormat::GLB;
    options.bEmbedTextures = false;

    const auto external = Convert(file, options);
    Require(external.result.bSuccess && !external.result.externalFiles.empty(), "external images are returned");

    const auto document = ParseGLB(external.result.data);

    for (const auto& image : document.gltf["images"])
        Require(image.contains("uri") && !image.contains("bufferView"), "external images are referenced by URI");

    //Whatever the resource name, the images are under textures/ beside the asset.
    for (const auto& externalFile : external.result.externalFiles)
        Require(externalFile.relativePath.rfind(s_ExternalImageDirectory, 0) == 0 || externalFile.relativePath == "converter_test.sh9/area-lights.json", "external images and area light sidecar use their adjacent directories");

    WriteOutput("external/converter_external.glb", external.result.data);

    for (const auto& externalFile : external.result.externalFiles)
        WriteOutput("external/" + externalFile.relativePath, externalFile.bytes);

    //The sink receives the asset, then its images.
    std::vector<std::string> sunk;

    ConversionRequest request;
    request.asset.pFile     = file.data();
    request.asset.fileBytes = static_cast<std::uint32_t>(file.size());
    request.options         = options;
    request.resourceName    = "sink";
    request.outputSink      = [&sunk](const char* szPath, const std::uint8_t*, std::uint64_t bytes) {
        sunk.push_back(szPath);
        return bytes > 0;
    };

    const auto sinkResult = ConvertRequest(request);
    Require(sinkResult.bSuccess && sinkResult.data.empty() && sunk.size() > 1 && sunk.front() == "sink.glb", "the output sink receives every file");

    request.outputSink = [](const char*, const std::uint8_t*, std::uint64_t) {
        return false;
    };
    Require(ConvertRequest(request).error == ConversionError::OutputFailed, "a failing sink fails the conversion");

    request.outputSink = [](const char* szPath, const std::uint8_t*, std::uint64_t) {
        return std::string(szPath).find("area-lights.json") == std::string::npos;
    };
    const auto rejectedArea = ConvertRequest(request);
    Require(rejectedArea.error == ConversionError::OutputFailed && rejectedArea.errorMessage.find("area-lights.json") != std::string::npos, "sidecar rejection fails the conversion and names the file");
    request.outputSink = [](const char*, const std::uint8_t*, std::uint64_t) {
        return false;
    };

    //The error message names the rejected file; a long one is cut to the summary's capacity.
    request.resourceName.assign(MaxConversionStringLength, 'a');

    const auto truncated = ConvertRequest(request);
    Require(truncated.error == ConversionError::OutputFailed && truncated.errorMessage.size() == ErrorMessageCapacity - 1, "a long error message is truncated");
}

int main(int argc, char** argv)
{
    if (argc > 1)
        s_OutputDirectory = argv[1];

    TestWorldGeometry();
    TestTriangleWinding();
    TestSkyGeometry();
    TestEntityNodes();
    TestEntityKeyValues();
    TestPointLightEntities();
    TestPointLightIntensityScale();
    TestTransforms();
    TestLightmaps();
    TestPBRMaterials();
    TestTextureLights();
    TestEmissiveScale();
    TestAreaLightExport();
    TestAreaLightFitting();
    TestSpecialSurfaces();
    TestSpecialFaceExtents();
    TestTildeTextureName();
    TestDefaultSpatialDataOff();
    TestVisibilityExport();
    TestCollisionExport();
    TestBlueShift();
    TestInvalidInput();
    TestOutputs();

    std::cout << "UtilGoldSrcAssetConverter BSP tests passed\n";

    return EXIT_SUCCESS;
}
