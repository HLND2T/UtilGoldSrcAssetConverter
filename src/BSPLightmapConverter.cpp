#include "BSPConverterInternal.h"

#include <cmath>

namespace GoldSrcAssetConverter
{
constexpr std::uint32_t s_LightmapRGBChannels = 3;

BSPLightmapAtlasLayout MakeLightmapAtlasLayout(std::uint32_t pageCount)
{
    BSPLightmapAtlasLayout layout;
    layout.pageCount = pageCount;

    if (!pageCount)
        return layout;

    //A square grid: at most GoldSrcRender::BSPMaxLightmapPages pages of 128 luxels, 4096 luxels on a side.
    layout.columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(pageCount))));
    layout.rows    = (pageCount + layout.columns - 1) / layout.columns;
    layout.width   = layout.columns * GoldSrcRender::BSPLightmapPageSize;
    layout.height  = layout.rows * GoldSrcRender::BSPLightmapPageSize;

    return layout;
}

Vector2 ToLightmapAtlasTexcoord(const BSPLightmapAtlasLayout& layout, std::uint32_t page, const Vector2& texcoord)
{
    const auto column = page % layout.columns;
    const auto row    = page / layout.columns;

    return {
        static_cast<float>((static_cast<double>(column) + texcoord.x) / layout.columns),
        static_cast<float>((static_cast<double>(row) + texcoord.y) / layout.rows),
    };
}

void BuildLightmapAtlas(const GoldSrcRender::BSPWorld& world, std::uint32_t layer, const BSPLightmapAtlasLayout& layout, std::vector<std::uint8_t>& texels)
{
    texels.assign(static_cast<std::size_t>(layout.width) * layout.height * s_LightmapRGBChannels, 0);

    const auto& pages = world.lightmaps[layer];

    for (std::uint32_t page = 0; page < layout.pageCount; ++page)
    {
        const auto column = page % layout.columns;
        const auto row    = page / layout.columns;

        for (std::uint32_t y = 0; y < GoldSrcRender::BSPLightmapPageSize; ++y)
        {
            const auto* pSource = pages.data() + ((static_cast<std::size_t>(page) * GoldSrcRender::BSPLightmapPageSize + y) * GoldSrcRender::BSPLightmapPageSize) * GoldSrcRender::RGBABytes;
            auto*       pDest   = texels.data() + ((static_cast<std::size_t>(row) * GoldSrcRender::BSPLightmapPageSize + y) * layout.width + static_cast<std::size_t>(column) * GoldSrcRender::BSPLightmapPageSize) * s_LightmapRGBChannels;

            for (std::uint32_t x = 0; x < GoldSrcRender::BSPLightmapPageSize; ++x, pSource += GoldSrcRender::RGBABytes, pDest += s_LightmapRGBChannels)
            {
                pDest[0] = pSource[0];
                pDest[1] = pSource[1];
                pDest[2] = pSource[2];
            }
        }
    }
}
} // namespace GoldSrcAssetConverter
