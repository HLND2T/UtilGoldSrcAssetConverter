#if !defined(PNG_ENCODER_H)
#    define PNG_ENCODER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include <cstdint>
#    include <vector>

namespace GoldSrcAssetConverter
{
//Encodes width * height texels of channels 8-bit channels (3: RGB, 4: RGBA), rows top to bottom, as a PNG file.
//The output is deterministic. Returns false when stb_image_write fails.
bool EncodePNG(const std::uint8_t* pPixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels, std::vector<std::uint8_t>& png);
} // namespace GoldSrcAssetConverter

#endif
