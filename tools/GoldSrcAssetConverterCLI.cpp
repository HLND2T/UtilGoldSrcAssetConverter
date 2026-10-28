//Command line tool of UtilGoldSrcAssetConverter: converts a BSP map to .glb or .gltf through UtilGoldSrcAssetConverter.dll,
//which is loaded from beside the executable.
//
//  GoldSrcAssetConverterCLI <map> [-basedir <directory> [-game <mod>] [-addons] [-nohdmodels]] [-o <directory>]
//                           [--wad <file.wad>]... [--wad-dir <directory>] [--gltf] [--blueshift] [--external-textures]
//                           [--visibility] [--collision] [--scale <factor>] [--emissive-scale <factor>]
//                           [--point-light-intensity-scale <factor>]
//
//Without -basedir, <map> and the WAD files are paths of the local file system. With -basedir, the tool loads
//filesystem_stdio.dll from beside the executable and mounts the game's search paths as the engine does (see
//MountGameSearchPaths): <map> is a path relative to them, or a map name looked up as maps/<name>.bsp, and the WAD files
//the worldspawn's "wad" key lists are found through them.
//Textures come from the BSP's embedded miptexes, then from the worldspawn's WAD files, then from the WAD files given
//(every *.wad of --wad-dir, in name order), searched in order. Outputs are written into a directory named after the map
//in the output directory (the current one by default), with the render manifest beside it: maps/c1a0.bsp gives
//c1a0/c1a0.glb, c1a0/textures/ with --external-textures, and c1a0.manifest.json.
#include "IUtilGoldSrcAssetConverter.h"
#include "FileSystem.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace GoldSrcAssetConverter;

#if defined(_WIN32)
static const char s_ConverterModuleName[]  = "UtilGoldSrcAssetConverter.dll";
static const char s_FileSystemModuleName[] = "filesystem_stdio.dll";
#elif defined(__APPLE__)
static const char s_ConverterModuleName[]  = "UtilGoldSrcAssetConverter.dylib";
static const char s_FileSystemModuleName[] = "filesystem_stdio.dylib";
#else
static const char s_ConverterModuleName[]  = "UtilGoldSrcAssetConverter.so";
static const char s_FileSystemModuleName[] = "filesystem_stdio.so";
#endif

//The game the engine always mounts after the one it runs (its default game directory).
static const char s_DefaultGame[] = "valve";

//BSP30 header: the version, then a table of lumps, each an offset and a length. Blue Shift swaps the entity lump with the
//plane lump.
constexpr std::size_t   s_BSPLumpTableOffset    = 4;
constexpr std::size_t   s_BSPLumpEntryBytes     = 8;
constexpr std::uint32_t s_BSPEntityLump         = 0;
constexpr std::uint32_t s_BlueShiftEntityLump   = 1;
constexpr std::size_t   s_BSPEntityLumpEndBytes = s_BSPLumpTableOffset + (s_BlueShiftEntityLump + 1) * s_BSPLumpEntryBytes;

constexpr std::size_t s_ReadChunkBytes = 64 * 1024;

struct FileCloser
{
    void operator()(FILE* pFile) const
    {
        std::fclose(pFile);
    }
};

using FilePtr = std::unique_ptr<FILE, FileCloser>;

static FilePtr OpenFile(const std::filesystem::path& path, const char* szMode)
{
#if defined(_WIN32)
    const std::wstring mode(szMode, szMode + std::strlen(szMode));

    return FilePtr(_wfopen(path.c_str(), mode.c_str()));
#else
    return FilePtr(std::fopen(path.c_str(), szMode));
#endif
}

static bool ReadLocalFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
{
    auto pFile = OpenFile(path, "rb");

    if (!pFile)
        return false;

    std::vector<std::uint8_t> chunk(s_ReadChunkBytes);
    std::size_t               read{};

    bytes.clear();

    while ((read = std::fread(chunk.data(), 1, chunk.size(), pFile.get())) > 0)
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);

    return !std::ferror(pFile.get());
}

static bool WriteLocalFile(const std::filesystem::path& path, const std::uint8_t* pData, std::uint64_t bytes)
{
    if (bytes > std::numeric_limits<std::size_t>::max())
        return false;

    auto pFile = OpenFile(path, "wb");

    if (!pFile)
        return false;

    const auto size     = static_cast<std::size_t>(bytes);
    const bool bWritten = std::fwrite(pData, 1, size, pFile.get()) == size;

    return std::fclose(pFile.release()) == 0 && bWritten;
}

static int Usage()
{
    std::cerr << "usage: GoldSrcAssetConverterCLI <map> [-basedir <directory> [-game <mod>] [-addons] [-nohdmodels]] [-o <directory>]\n"
                 "                                [--wad <file.wad>]... [--wad-dir <directory>] [--gltf] [--blueshift] [--external-textures]\n"
                 "                                [--visibility] [--collision] [--scale <factor>] [--emissive-scale <factor>]\n"
                 "                                [--point-light-intensity-scale <factor>]\n"
                 "  --emissive-scale: texture emission multiplier (default 4; 0 disables emission)\n"
                 "  --point-light-intensity-scale: point light multiplier (default 1; 0 keeps entities without lights)\n"
                 "  --no-area-lights: write an empty area light sidecar\n"
                 "  --area-lights-per-group <1-64>: rectangle budget per emitter (default 16)\n";

    return EXIT_FAILURE;
}

static const char* GetSeverityName(DiagnosticSeverity severity)
{
    switch (severity)
    {
        case DiagnosticSeverity::Info:
            return "info";
        case DiagnosticSeverity::Error:
            return "error";
        case DiagnosticSeverity::Warning:
            break;
    }

    return "warning";
}

//Loads the converter module for the rest of the process. Null with a message on stderr when it cannot.
static IUtilGoldSrcAssetConverter* LoadConverter()
{
    auto* hModule = Sys_LoadModule(s_ConverterModuleName);

    if (!hModule)
    {
        std::cerr << "cannot load " << s_ConverterModuleName << '\n';
        return nullptr;
    }

    const auto factory    = Sys_GetFactory(hModule);
    auto*      pConverter = factory ? static_cast<IUtilGoldSrcAssetConverter*>(factory(UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION, nullptr)) : nullptr;

    if (!pConverter)
        std::cerr << "cannot get interface \"" UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION "\" from " << s_ConverterModuleName << '\n';

    return pConverter;
}

//Loads the file system module for the rest of the process and mounts it. Null with a message on stderr when it cannot.
static IFileSystem* LoadFileSystem()
{
    auto* hModule = Sys_LoadModule(s_FileSystemModuleName);

    if (!hModule)
    {
        std::cerr << "cannot load " << s_FileSystemModuleName << '\n';
        return nullptr;
    }

    const auto factory     = Sys_GetFactory(hModule);
    auto*      pFileSystem = factory ? static_cast<IFileSystem*>(factory(FILESYSTEM_INTERFACE_VERSION, nullptr)) : nullptr;

    if (!pFileSystem)
    {
        std::cerr << "cannot get interface \"" FILESYSTEM_INTERFACE_VERSION "\" from " << s_FileSystemModuleName << '\n';
        return nullptr;
    }

    pFileSystem->Mount();

    return pFileSystem;
}

//The fallback_dir of a game's liblist.gam, empty without one.
static std::string ReadFallbackGame(const std::filesystem::path& baseDirectory, const std::string& game)
{
    static const char s_FallbackKey[] = "fallback_dir";

    std::vector<std::uint8_t> file;

    if (!ReadLocalFile(baseDirectory / game / "liblist.gam", file))
        return {};

    const std::string text(file.begin(), file.end());
    std::size_t       lineStart = 0;

    while (lineStart < text.size())
    {
        auto lineEnd = text.find('\n', lineStart);

        if (lineEnd == std::string::npos)
            lineEnd = text.size();

        const auto line = text.substr(lineStart, lineEnd - lineStart);
        lineStart       = lineEnd + 1;

        if (line.size() < sizeof(s_FallbackKey) - 1 || !std::equal(s_FallbackKey, s_FallbackKey + sizeof(s_FallbackKey) - 1, line.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            continue;

        const auto start = line.find('"');
        const auto end   = start == std::string::npos ? std::string::npos : line.find('"', start + 1);

        return end == std::string::npos ? std::string() : line.substr(start + 1, end - start - 1);
    }

    return {};
}

//The search paths FileSystem_SetGameDirectory mounts for game, for an English, non-low-violence installation: the
//game's _addon (with -addons), _hd (unless -nohdmodels), the game itself and its _downloads, the fallback_dir chain of
//liblist.gam, then the default game's _hd, the base directory and the default game.
static void MountGameSearchPaths(IFileSystem* pFileSystem, const std::filesystem::path& baseDirectory, const std::string& game, bool bAddons, bool bHDModels)
{
    auto addPath = [pFileSystem, &baseDirectory](const std::string& directory, const char* szPathID) {
        pFileSystem->AddSearchPath((directory.empty() ? baseDirectory : baseDirectory / directory).string().c_str(), szPathID);
    };

    if (bAddons)
        addPath(game + "_addon", "GAME");

    if (bHDModels)
        addPath(game + "_hd", "GAME");

    addPath(game, "GAME");
    addPath(game + "_downloads", "GAMEDOWNLOAD");

    //The engine follows fallback directories until valve.
    std::set<std::string> visited{game};

    for (auto fallback = ReadFallbackGame(baseDirectory, game); !fallback.empty() && visited.insert(fallback).second; fallback = ReadFallbackGame(baseDirectory, fallback))
    {
        addPath(fallback, "GAME_FALLBACK");

        if (fallback == s_DefaultGame)
            break;
    }

    if (bHDModels)
        addPath(std::string(s_DefaultGame) + "_hd", "DEFAULTGAME");

    addPath({}, "BASE");
    addPath(s_DefaultGame, "DEFAULTGAME");
}

static bool ReadGameFile(IFileSystem* pFileSystem, const std::string& relativePath, std::vector<std::uint8_t>& bytes)
{
    auto* hFile = pFileSystem->Open(relativePath.c_str(), "rb");

    if (!hFile)
        return false;

    bytes.resize(pFileSystem->Size(hFile));

    const bool bRead = bytes.empty() || pFileSystem->Read(bytes.data(), bytes.size(), hFile) == static_cast<int>(bytes.size());

    pFileSystem->Close(hFile);

    return bRead;
}

//The next token of the entity lump as the engine's COM_Parse reads it: quoted strings, the single characters {}()':, or
//words. Empty at the end.
static std::string ParseEntityToken(const char*& pText, const char* pEnd)
{
    static const char s_SingleCharacters[] = "{}()':";

    while (pText < pEnd)
    {
        while (pText < pEnd && static_cast<unsigned char>(*pText) <= ' ')
            ++pText;

        if (pEnd - pText >= 2 && pText[0] == '/' && pText[1] == '/')
        {
            while (pText < pEnd && *pText != '\n')
                ++pText;

            continue;
        }

        break;
    }

    if (pText >= pEnd)
        return {};

    std::string token;

    if (*pText == '"')
    {
        ++pText;

        while (pText < pEnd && *pText != '"')
            token.push_back(*pText++);

        if (pText < pEnd)
            ++pText;

        return token;
    }

    if (std::strchr(s_SingleCharacters, *pText))
        return std::string(1, *pText++);

    while (pText < pEnd && static_cast<unsigned char>(*pText) > ' ' && !std::strchr(s_SingleCharacters, *pText))
        token.push_back(*pText++);

    return token;
}

//The "wad" value of the BSP's first entity, as the engine's Mod_LoadEntities finds it; empty without one.
static std::string FindWorldspawnWADList(const std::vector<std::uint8_t>& bsp, GoldSrcRender::BSPLayout layout)
{
    if (bsp.size() < s_BSPEntityLumpEndBytes)
        return {};

    const auto    lump = layout == GoldSrcRender::BSPLayout::BlueShift ? s_BlueShiftEntityLump : s_BSPEntityLump;
    std::uint32_t entry[2]{};
    std::memcpy(entry, bsp.data() + s_BSPLumpTableOffset + lump * s_BSPLumpEntryBytes, sizeof(entry));

    const std::uint64_t offset = entry[0];
    const std::uint64_t length = entry[1];

    if (offset > bsp.size() || length > bsp.size() - offset)
        return {};

    const auto* pText = reinterpret_cast<const char*>(bsp.data() + offset);
    const auto* pEnd  = pText + length;

    for (auto token = ParseEntityToken(pText, pEnd); !token.empty() && token != "}"; token = ParseEntityToken(pText, pEnd))
    {
        if (token == "wad")
            return ParseEntityToken(pText, pEnd);
    }

    return {};
}

//The WAD files of a "wad" value as the engine's TEX_InitFromWad opens them: entries separated by ';', each reduced to its
//file name with the .wad extension; the decal WADs are skipped.
static std::vector<std::string> GetWADFileNames(const std::string& wadList)
{
    std::vector<std::string> names;
    std::size_t              start = 0;

    while (start <= wadList.size())
    {
        auto end = wadList.find(';', start);

        if (end == std::string::npos)
            end = wadList.size();

        auto entry = wadList.substr(start, end - start);
        std::replace(entry.begin(), entry.end(), '\\', '/');

        const auto separator = entry.find_last_of('/');

        if (separator != std::string::npos)
            entry = entry.substr(separator + 1);

        const auto extension = entry.find_last_of('.');

        if (extension != std::string::npos)
            entry = entry.substr(0, extension);

        if (!entry.empty() && entry.find("pldecal") == std::string::npos && entry.find("tempdecal") == std::string::npos)
            names.push_back(entry + ".wad");

        start = end + 1;
    }

    return names;
}

//A map name without directory or extension is looked up as maps/<name>.bsp, as the engine's map command does.
static std::string GetGameMapPath(const std::string& map)
{
    if (map.find_first_of("/\\") != std::string::npos || std::filesystem::path(map).has_extension())
        return map;

    return "maps/" + map + ".bsp";
}

//Miptex requests are looked up in the WAD files in order; detail and skybox images are not resolved.
class CWADTextureResolver final : public ITextureResolver
{
public:
    CWADTextureResolver(IUtilGoldSrcAssetConverter* pConverter, const std::vector<std::vector<std::uint8_t>>& wads) :
        m_pConverter(pConverter), m_Wads(wads)
    {
    }

    bool ResolveTexture(const TextureRequest& request, ResolvedTexture* pTexture) noexcept override
    {
        if (request.kind != TextureKind::Miptex)
            return false;

        for (const auto& wad : m_Wads)
        {
            if (m_pConverter->FindWAD3Texture(wad.data(), wad.size(), request.szName, pTexture))
                return true;
        }

        return false;
    }

private:
    IUtilGoldSrcAssetConverter*                   m_pConverter{};
    const std::vector<std::vector<std::uint8_t>>& m_Wads;
};

//Writes every file under the output directory and remembers the asset's path, the first file written, and the
//manifest's, the last.
class CDirectoryOutputSink final : public IConversionOutputSink
{
public:
    explicit CDirectoryOutputSink(const std::filesystem::path& directory) :
        m_Directory(directory)
    {
    }

    bool WriteFile(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes) noexcept override
    {
        try
        {
            const auto path = m_Directory / std::filesystem::u8path(szRelativePath);

            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);

            if (!WriteLocalFile(path, pData, bytes))
                return false;

            if (m_AssetPath.empty())
                m_AssetPath = path;

            m_LastPath = path;

            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    const std::filesystem::path& GetAssetPath() const
    {
        return m_AssetPath;
    }

    const std::filesystem::path& GetLastPath() const
    {
        return m_LastPath;
    }

private:
    std::filesystem::path m_Directory;
    std::filesystem::path m_AssetPath;
    std::filesystem::path m_LastPath;
};

class CConsoleDiagnosticSink final : public IDiagnosticSink
{
public:
    void OnDiagnostic(const DiagnosticView& diagnostic) noexcept override
    {
        std::cerr << GetSeverityName(diagnostic.severity) << ": " << diagnostic.szMessage << '\n';
    }
};

int main(int argc, char** argv)
{
    std::string                        input;
    std::filesystem::path              outputDirectory = ".";
    std::filesystem::path              baseDirectory;
    std::string                        game = s_DefaultGame;
    bool                               bAddons{};
    bool                               bHDModels{true};
    std::vector<std::filesystem::path> wadPaths;
    BSPConversionRequestView           request;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        const bool        bValue   = i + 1 < argc;

        if (argument == "-o" && bValue)
            outputDirectory = argv[++i];
        else if (argument == "-basedir" && bValue)
            baseDirectory = argv[++i];
        else if (argument == "-game" && bValue)
            game = argv[++i];
        else if (argument == "-addons")
            bAddons = true;
        else if (argument == "-nohdmodels")
            bHDModels = false;
        else if (argument == "--wad" && bValue)
            wadPaths.emplace_back(argv[++i]);
        else if (argument == "--wad-dir" && bValue)
        {
            std::vector<std::filesystem::path> directoryWads;

            for (const auto& entry : std::filesystem::directory_iterator(argv[++i]))
            {
                auto extension = entry.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (extension == ".wad")
                    directoryWads.push_back(entry.path());
            }

            std::sort(directoryWads.begin(), directoryWads.end());
            wadPaths.insert(wadPaths.end(), directoryWads.begin(), directoryWads.end());
        }
        else if (argument == "--gltf")
            request.options.format = OutputFormat::GLTF;
        else if (argument == "--blueshift")
            request.asset.layout = GoldSrcRender::BSPLayout::BlueShift;
        else if (argument == "--external-textures")
            request.options.bEmbedTextures = false;
        else if (argument == "--visibility")
            request.options.bEmbedBSPVisibilityData = true;
        else if (argument == "--collision")
            request.options.bEmbedBSPCollisionData = true;
        else if (argument == "--no-area-lights")
            request.options.bExportAreaLights = false;
        else if (argument == "--area-lights-per-group" && bValue)
        {
            const auto* szValue = argv[++i];
            char*       pEnd{};
            errno            = 0;
            const auto count = std::strtoul(szValue, &pEnd, 10);
            if (pEnd == szValue || *pEnd || errno == ERANGE || count < 1 || count > 64)
                return Usage();
            request.options.maxAreaLightsPerGroup = static_cast<std::uint32_t>(count);
        }
        else if (argument == "--scale" && bValue)
            request.options.scaleFactor = std::strtof(argv[++i], nullptr);
        else if ((argument == "--emissive-scale" || argument == "--point-light-intensity-scale") && bValue)
        {
            const auto* szValue = argv[++i];
            char*       pEnd{};
            errno            = 0;
            const auto scale = std::strtof(szValue, &pEnd);

            if (pEnd == szValue || *pEnd || errno == ERANGE || !std::isfinite(scale) || scale < 0)
            {
                std::cerr << argument << " must be a finite nonnegative float\n";
                return EXIT_FAILURE;
            }

            if (argument == "--emissive-scale")
                request.options.emissiveScale = scale;
            else
                request.options.pointLightIntensityScale = scale;
        }
        else if (input.empty() && argument[0] != '-')
            input = argument;
        else
            return Usage();
    }

    if (input.empty())
        return Usage();

    auto* pConverter = LoadConverter();

    if (!pConverter)
        return EXIT_FAILURE;

    IFileSystem* pFileSystem{};

    if (!baseDirectory.empty())
    {
        pFileSystem = LoadFileSystem();

        if (!pFileSystem)
            return EXIT_FAILURE;

        MountGameSearchPaths(pFileSystem, std::filesystem::absolute(baseDirectory), game, bAddons, bHDModels);
    }

    const auto mapPath = pFileSystem ? GetGameMapPath(input) : input;

    std::vector<std::uint8_t> file;

    if (pFileSystem ? !ReadGameFile(pFileSystem, mapPath, file) : !ReadLocalFile(std::filesystem::u8path(mapPath), file))
    {
        std::cerr << "cannot read " << mapPath << '\n';
        return EXIT_FAILURE;
    }

    std::vector<std::vector<std::uint8_t>> wads;

    if (pFileSystem)
    {
        for (const auto& wadName : GetWADFileNames(FindWorldspawnWADList(file, request.asset.layout)))
        {
            wads.emplace_back();

            if (!ReadGameFile(pFileSystem, wadName, wads.back()))
            {
                std::cerr << "cannot read " << wadName << " from the game's search paths\n";
                wads.pop_back();
            }
        }
    }

    for (const auto& wadPath : wadPaths)
    {
        wads.emplace_back();

        if (!ReadLocalFile(wadPath, wads.back()))
        {
            std::cerr << "cannot read " << wadPath.string() << '\n';
            wads.pop_back();
        }
    }

    const auto identifier   = std::filesystem::u8path(mapPath).filename().u8string();
    const auto resourceName = std::filesystem::u8path(mapPath).stem().u8string();
    const auto sourcePath   = std::filesystem::u8path(mapPath).generic_u8string();

    //The converter puts the asset into a directory named after the resource, and the manifest beside it.
    CWADTextureResolver    resolver(pConverter, wads);
    CDirectoryOutputSink   outputSink(outputDirectory);
    CConsoleDiagnosticSink diagnosticSink;

    request.asset.szIdentifier = identifier.c_str();
    request.asset.pFile        = file.data();
    request.asset.fileBytes    = static_cast<std::uint32_t>(file.size());
    request.pTextureResolver   = &resolver;
    request.szResourceName     = resourceName.c_str();
    request.szSourcePath       = sourcePath.c_str();
    request.pOutputSink        = &outputSink;
    request.pDiagnosticSink    = &diagnosticSink;

    const auto summary = pConverter->ConvertBSP(request);

    if (summary.error != ConversionError::None)
    {
        std::cerr << "conversion failed: " << summary.szErrorMessage << '\n';
        return EXIT_FAILURE;
    }

    const auto& statistics = summary.statistics;

    std::cout << outputSink.GetAssetPath().string() << '\n'
              << "  manifest " << outputSink.GetLastPath().string() << '\n'
              << "  models " << statistics.modelCount << ", world faces " << statistics.worldFaceCount << ", sky faces " << statistics.skyFaceCount << ", water faces " << statistics.waterFaceCount
              << ", degenerate faces " << statistics.degenerateFaceCount << '\n'
              << "  entities " << statistics.entityCount << ", brush entity nodes " << statistics.brushEntityNodeCount << ", point entity nodes " << statistics.pointEntityNodeCount
              << ", unreferenced submodels " << statistics.unreferencedSubmodelCount << '\n'
              << "  meshes " << statistics.meshCount << ", materials " << statistics.materialCount << ", textures " << statistics.textureCount << " (" << statistics.missingTextureCount
              << " missing), lightmap pages " << statistics.lightmapPageCount << ", lightmap layers " << statistics.lightmapCount << '\n'
              << "  visibility bytes " << statistics.visibilityDataBytes << ", collision bytes " << statistics.collisionDataBytes << '\n'
              << "  warnings " << summary.warningCount << ", cache key " << summary.szCacheKey << '\n';

    return EXIT_SUCCESS;
}
