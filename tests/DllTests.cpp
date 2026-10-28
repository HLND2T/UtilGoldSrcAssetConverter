//Tests of UtilGoldSrcAssetConverter.dll through its published interface only: the module given on the command line is
//loaded as a game would load it, the interface resolved through CreateInterface, and a conversion run with
//caller-implemented resolver and sinks. The conversion itself is covered by the BSP tests, which link the converter.
#include "BSPConverterTestFile.h"
#include "IUtilGoldSrcAssetConverter.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace GoldSrcConverterTest;
using namespace GoldSrcAssetConverter;

static void Require(bool bCondition, const char* pMessage)
{
    if (!bCondition)
    {
        std::cerr << "FAILED: " << pMessage << '\n';
        std::exit(EXIT_FAILURE);
    }
}

//"glTF", the first word of a .glb file.
constexpr std::uint32_t s_GLBMagic = 0x46546C67;

constexpr std::size_t s_SHA256HexLength = SHA256HexCapacity - 1;

//Files of a conversion with embedded images: the asset, the area light descriptor and the render manifest.
constexpr std::size_t s_ConvertedFileCount = 3;

class CWADTextureResolver final : public ITextureResolver
{
public:
    CWADTextureResolver(IUtilGoldSrcAssetConverter* pConverter, const std::vector<std::uint8_t>& wad) :
        m_pConverter(pConverter), m_Wad(wad)
    {
    }

    bool ResolveTexture(const TextureRequest& request, ResolvedTexture* pTexture) noexcept override
    {
        return request.kind == TextureKind::Miptex && m_pConverter->FindWAD3Texture(m_Wad.data(), m_Wad.size(), request.szName, pTexture);
    }

private:
    IUtilGoldSrcAssetConverter*      m_pConverter{};
    const std::vector<std::uint8_t>& m_Wad;
};

//Keeps the name and bytes of every file, or rejects them all.
class CCollectingOutputSink final : public IConversionOutputSink
{
public:
    explicit CCollectingOutputSink(bool bAccept) :
        m_bAccept(bAccept)
    {
    }

    bool WriteFile(const char* szRelativePath, const std::uint8_t* pData, std::uint64_t bytes) noexcept override
    {
        if (!m_bAccept)
            return false;

        m_Names.emplace_back(szRelativePath);
        m_Files.emplace_back(pData, pData + bytes);

        return true;
    }

    std::vector<std::string>               m_Names;
    std::vector<std::vector<std::uint8_t>> m_Files;

private:
    bool m_bAccept{};
};

class CCountingDiagnosticSink final : public IDiagnosticSink
{
public:
    void OnDiagnostic(const DiagnosticView& diagnostic) noexcept override
    {
        if (diagnostic.szMessage && diagnostic.szMessage[0])
            ++m_Count;
    }

    std::uint32_t m_Count{};
};

int main(int argc, char** argv)
{
    Require(argc > 1, "the converter module path is given");

    auto* hModule = Sys_LoadModule(argv[1]);
    Require(hModule, "the converter module loads");

    const auto factory = Sys_GetFactory(hModule);
    Require(factory, "the converter module exports CreateInterface");
    Require(!factory("UtilGoldSrcAssetConverterAPI_000", nullptr), "an unknown interface version is refused");
    Require(!factory("UtilGoldSrcAssetConverterAPI_001", nullptr), "the request layout without emissiveScale is refused");
    Require(!factory("UtilGoldSrcAssetConverterAPI_002", nullptr), "the request layout without pointLightIntensityScale is refused");
    Require(!factory("UtilGoldSrcAssetConverterAPI_003", nullptr), "the request layout without area light options is refused");

    auto* pConverter = static_cast<IUtilGoldSrcAssetConverter*>(factory(UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION, nullptr));
    Require(pConverter, "the converter interface is published");

    //WAD lookup ignores case, as the engine's does.
    const auto wad = MakeConverterTestWAD("!WATER", TestBSPTextureSize, TestBSPTextureSize);

    ResolvedTexture texture;
    Require(pConverter->FindWAD3Texture(wad.data(), wad.size(), "!water", &texture) && texture.format == ResolvedTextureFormat::Miptex && texture.width == TestBSPTextureSize,
            "a WAD texture is found through the interface");
    Require(!pConverter->FindWAD3Texture(wad.data(), wad.size(), "missing", &texture), "a missing WAD texture is not found");

    const auto file = MakeConverterTestBSP();

    CWADTextureResolver     resolver(pConverter, wad);
    CCollectingOutputSink   outputSink(true);
    CCountingDiagnosticSink diagnosticSink;

    BSPConversionRequestView request;
    request.asset.szIdentifier = "maps/converter_test.bsp";
    request.asset.pFile        = file.data();
    request.asset.fileBytes    = static_cast<std::uint32_t>(file.size());
    request.pTextureResolver   = &resolver;
    request.szSourcePath       = "maps/converter_test.bsp";
    request.pOutputSink        = &outputSink;
    request.pDiagnosticSink    = &diagnosticSink;

    const auto summary = pConverter->ConvertBSP(request);
    Require(summary.error == ConversionError::None && !summary.szErrorMessage[0], "the test BSP converts through the interface");
    Require(outputSink.m_Names.size() == s_ConvertedFileCount && outputSink.m_Names.front() == "converter_test/converter_test.glb" && outputSink.m_Names[1] == "converter_test/converter_test.sh9/area-lights.json" &&
                outputSink.m_Names.back() == "converter_test.manifest.json",
            "the sink receives the asset, the area light descriptor and the render manifest");

    std::uint32_t magic{};
    Require(outputSink.m_Files.front().size() > sizeof(magic), "the asset has a header");
    std::memcpy(&magic, outputSink.m_Files.front().data(), sizeof(magic));
    Require(magic == s_GLBMagic, "the asset is a .glb file");

    Require(summary.warningCount > 0 && summary.warningCount == diagnosticSink.m_Count, "every diagnostic reaches the sink");
    Require(summary.converterVersion > 0 && summary.extrasSchemaVersion > 0, "the summary carries the module's versions");
    Require(std::strlen(summary.szSourceHash) == s_SHA256HexLength && std::strlen(summary.szTextureHash) == s_SHA256HexLength, "the summary carries both digests");
    Require(std::strstr(summary.szCacheKey, summary.szSourceHash) != nullptr, "the cache key holds the source digest");
    Require(summary.statistics.modelCount == ConverterTestModelCount, "the summary carries the statistics");

    const std::string defaultAsset(outputSink.m_Files.front().begin(), outputSink.m_Files.front().end());
    Require(defaultAsset.find("KHR_lights_punctual") != std::string::npos, "the real DLL exports a punctual light by default");
    CCollectingOutputSink disabledLightSink(true);
    request.pOutputSink                      = &disabledLightSink;
    request.options.pointLightIntensityScale = 0;
    const auto disabledLights                = pConverter->ConvertBSP(request);
    Require(disabledLights.error == ConversionError::None && disabledLightSink.m_Files.size() == s_ConvertedFileCount, "the DLL accepts the new point light scale option");
    const std::string disabledAsset(disabledLightSink.m_Files.front().begin(), disabledLightSink.m_Files.front().end());
    Require(disabledAsset.find("KHR_lights_punctual") == std::string::npos && std::strcmp(summary.szCacheKey, disabledLights.szCacheKey) != 0, "the DLL option disables lights and changes the cache key");
    request.options.pointLightIntensityScale = 1;

    //Requests without output sink fail with a message.
    request.pOutputSink = nullptr;

    const auto noSink = pConverter->ConvertBSP(request);
    Require(noSink.error == ConversionError::InvalidRequest && noSink.szErrorMessage[0], "a request without output sink is rejected");

    //A rejected file fails the conversion; the message naming it is cut to the summary's capacity.
    const std::string longName(MaxConversionStringLength, 'a');

    CCollectingOutputSink rejectingSink(false);
    request.pOutputSink    = &rejectingSink;
    request.szResourceName = longName.c_str();

    const auto rejected = pConverter->ConvertBSP(request);
    Require(rejected.error == ConversionError::OutputFailed && std::strlen(rejected.szErrorMessage) == ErrorMessageCapacity - 1, "a rejected file fails with a truncated message");

    Sys_UnloadModule(hModule);

    std::cout << "UtilGoldSrcAssetConverter DLL tests passed\n";

    return EXIT_SUCCESS;
}
