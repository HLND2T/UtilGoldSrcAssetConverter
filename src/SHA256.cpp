#include "SHA256.h"

#include <algorithm>
#include <climits>

namespace GoldSrcAssetConverter
{
//Round constants of FIPS 180-4, section 4.2.2.
static const std::uint32_t s_RoundConstants[] = {
    0x428A2F98,
    0x71374491,
    0xB5C0FBCF,
    0xE9B5DBA5,
    0x3956C25B,
    0x59F111F1,
    0x923F82A4,
    0xAB1C5ED5,
    0xD807AA98,
    0x12835B01,
    0x243185BE,
    0x550C7DC3,
    0x72BE5D74,
    0x80DEB1FE,
    0x9BDC06A7,
    0xC19BF174,
    0xE49B69C1,
    0xEFBE4786,
    0x0FC19DC6,
    0x240CA1CC,
    0x2DE92C6F,
    0x4A7484AA,
    0x5CB0A9DC,
    0x76F988DA,
    0x983E5152,
    0xA831C66D,
    0xB00327C8,
    0xBF597FC7,
    0xC6E00BF3,
    0xD5A79147,
    0x06CA6351,
    0x14292967,
    0x27B70A85,
    0x2E1B2138,
    0x4D2C6DFC,
    0x53380D13,
    0x650A7354,
    0x766A0ABB,
    0x81C2C92E,
    0x92722C85,
    0xA2BFE8A1,
    0xA81A664B,
    0xC24B8B70,
    0xC76C51A3,
    0xD192E819,
    0xD6990624,
    0xF40E3585,
    0x106AA070,
    0x19A4C116,
    0x1E376C08,
    0x2748774C,
    0x34B0BCB5,
    0x391C0CB3,
    0x4ED8AA4A,
    0x5B9CCA4F,
    0x682E6FF3,
    0x748F82EE,
    0x78A5636F,
    0x84C87814,
    0x8CC70208,
    0x90BEFFFA,
    0xA4506CEB,
    0xBEF9A3F7,
    0xC67178F2,
};

constexpr std::size_t   s_ScheduleWords = 64;
constexpr std::size_t   s_LengthBytes   = 8;
constexpr std::uint8_t  s_PaddingMarker = 0x80;
constexpr std::uint32_t s_NibbleMask    = 0xF;
constexpr std::uint32_t s_NibbleBits    = 4;
constexpr std::uint32_t s_WordBits      = 32;
constexpr char          s_HexDigits[]   = "0123456789abcdef";

static std::uint32_t RotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (s_WordBits - bits));
}

void CSHA256::ProcessBlock(const std::uint8_t* pBlock)
{
    std::uint32_t schedule[s_ScheduleWords];

    for (std::size_t i = 0; i < BlockBytes / sizeof(std::uint32_t); ++i)
    {
        const auto* pWord = pBlock + i * sizeof(std::uint32_t);
        schedule[i]       = static_cast<std::uint32_t>(pWord[0]) << 24 | static_cast<std::uint32_t>(pWord[1]) << 16 | static_cast<std::uint32_t>(pWord[2]) << 8 | pWord[3];
    }

    //The rotation and shift counts below are those of the FIPS 180-4 functions (section 4.1.2).
    for (std::size_t i = BlockBytes / sizeof(std::uint32_t); i < s_ScheduleWords; ++i)
    {
        const auto s0 = RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const auto s1 = RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i]   = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    auto state = m_State;

    for (std::size_t i = 0; i < s_ScheduleWords; ++i)
    {
        const auto s1     = RotateRight(state[4], 6) ^ RotateRight(state[4], 11) ^ RotateRight(state[4], 25);
        const auto choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
        const auto temp1  = state[7] + s1 + choice + s_RoundConstants[i] + schedule[i];
        const auto s0     = RotateRight(state[0], 2) ^ RotateRight(state[0], 13) ^ RotateRight(state[0], 22);
        const auto major  = (state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]);
        const auto temp2  = s0 + major;

        state[7] = state[6];
        state[6] = state[5];
        state[5] = state[4];
        state[4] = state[3] + temp1;
        state[3] = state[2];
        state[2] = state[1];
        state[1] = state[0];
        state[0] = temp1 + temp2;
    }

    for (std::size_t i = 0; i < StateWords; ++i)
        m_State[i] += state[i];
}

void CSHA256::Update(const void* pData, std::size_t bytes)
{
    const auto* pBytes = static_cast<const std::uint8_t*>(pData);

    m_TotalBytes += bytes;

    while (bytes)
    {
        const auto count = std::min(bytes, BlockBytes - m_BlockBytes);

        for (std::size_t i = 0; i < count; ++i)
            m_Block[m_BlockBytes + i] = pBytes[i];

        m_BlockBytes += count;
        pBytes += count;
        bytes -= count;

        if (m_BlockBytes == BlockBytes)
        {
            ProcessBlock(m_Block.data());
            m_BlockBytes = 0;
        }
    }
}

std::string CSHA256::FinishHex()
{
    const auto totalBits = m_TotalBytes * CHAR_BIT;

    m_Block[m_BlockBytes++] = s_PaddingMarker;

    if (m_BlockBytes > BlockBytes - s_LengthBytes)
    {
        while (m_BlockBytes < BlockBytes)
            m_Block[m_BlockBytes++] = 0;

        ProcessBlock(m_Block.data());
        m_BlockBytes = 0;
    }

    while (m_BlockBytes < BlockBytes - s_LengthBytes)
        m_Block[m_BlockBytes++] = 0;

    for (std::size_t i = 0; i < s_LengthBytes; ++i)
        m_Block[m_BlockBytes++] = static_cast<std::uint8_t>(totalBits >> ((s_LengthBytes - 1 - i) * CHAR_BIT));

    ProcessBlock(m_Block.data());

    std::string digest;

    for (const auto word : m_State)
    {
        for (std::uint32_t shift = s_WordBits; shift; shift -= s_NibbleBits)
            digest.push_back(s_HexDigits[(word >> (shift - s_NibbleBits)) & s_NibbleMask]);
    }

    return digest;
}
} // namespace GoldSrcAssetConverter
