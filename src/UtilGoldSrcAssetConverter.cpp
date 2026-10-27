#include "GoldSrcAssetConverterCore.h"

using namespace GoldSrcAssetConverter;

class CUtilGoldSrcAssetConverter : public IUtilGoldSrcAssetConverter
{
public:
    BSPConversionSummary ConvertBSP(const BSPConversionRequestView& request) noexcept override
    {
        return GoldSrcAssetConverter::ConvertBSP(request);
    }

    bool FindWAD3Texture(const std::uint8_t* pWad, std::uint64_t wadBytes, const char* szName, ResolvedTexture* pTexture) noexcept override
    {
        return GoldSrcAssetConverter::FindWAD3Texture(pWad, wadBytes, szName, pTexture);
    }
};

EXPOSE_SINGLE_INTERFACE(CUtilGoldSrcAssetConverter, IUtilGoldSrcAssetConverter, UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION);
