#if !defined(GOLDSRC_ASSET_CONVERTER_CORE_H)
#    define GOLDSRC_ASSET_CONVERTER_CORE_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "IUtilGoldSrcAssetConverter.h"

#    include <cstdint>

//The converter behind IUtilGoldSrcAssetConverter, linked into UtilGoldSrcAssetConverter.dll and the converter tests.
//Callers outside this project use the interface instead.
namespace GoldSrcAssetConverter
{
//Changes whenever the same input converts to different output; part of the cache key (see BSPConversionSummary).
constexpr std::uint32_t ConverterVersion = 10;

//Version of the extras.goldsrc schema (docs/GoldSrcBSPExtrasSchema.md).
constexpr std::uint32_t ExtrasSchemaVersion = 1;

//Version of the render manifest <resource>.manifest.json (docs/BSPConversion.md, "Render manifest").
constexpr std::uint32_t RenderManifestSchemaVersion = 2;

//See IUtilGoldSrcAssetConverter::ConvertBSP.
BSPConversionSummary ConvertBSP(const BSPConversionRequestView& request) noexcept;

//See IUtilGoldSrcAssetConverter::FindWAD3Texture.
bool FindWAD3Texture(const std::uint8_t* pWad, std::uint64_t wadBytes, const char* szName, ResolvedTexture* pTexture) noexcept;
} // namespace GoldSrcAssetConverter

#endif
