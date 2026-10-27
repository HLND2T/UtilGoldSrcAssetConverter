#if !defined(UTIL_GOLDSRC_ASSET_CONVERTER_SHA256_H)
#    define UTIL_GOLDSRC_ASSET_CONVERTER_SHA256_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include <array>
#    include <cstddef>
#    include <cstdint>
#    include <string>

namespace GoldSrcAssetConverter
{
//SHA-256 (FIPS 180-4) of the bytes passed to Update, for the cache key digests.
class CSHA256
{
public:
    void Update(const void* pData, std::size_t bytes);

    //Lowercase hexadecimal digest; the hash cannot be updated afterwards.
    std::string FinishHex();

private:
    static constexpr std::size_t BlockBytes = 64;
    static constexpr std::size_t StateWords = 8;

    void ProcessBlock(const std::uint8_t* pBlock);

    std::array<std::uint32_t, StateWords> m_State{0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
    std::array<std::uint8_t, BlockBytes>  m_Block{};
    std::size_t                           m_BlockBytes{};
    std::uint64_t                         m_TotalBytes{};
};
} // namespace GoldSrcAssetConverter

#endif
