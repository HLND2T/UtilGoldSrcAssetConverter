#include "PNGEncoder.h"

//stb_image_write (DiligentTools/ThirdParty/stb) with internal linkage, so that it cannot clash with another copy
//linked into the same module.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace GoldSrcAssetConverter
{
static void AppendPNGBytes(void* pContext, void* pData, int size)
{
    auto*       pPNG   = static_cast<std::vector<std::uint8_t>*>(pContext);
    const auto* pBytes = static_cast<const std::uint8_t*>(pData);

    pPNG->insert(pPNG->end(), pBytes, pBytes + size);
}

bool EncodePNG(const std::uint8_t* pPixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels, std::vector<std::uint8_t>& png)
{
    png.clear();

    const auto rowBytes = static_cast<int>(width * channels);

    return stbi_write_png_to_func(AppendPNGBytes, &png, static_cast<int>(width), static_cast<int>(height), static_cast<int>(channels), pPixels, rowBytes) != 0;
}
} // namespace GoldSrcAssetConverter
