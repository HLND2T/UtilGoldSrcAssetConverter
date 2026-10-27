#include "GLTFDocumentBuilder.h"

#include <climits>
#include <cstring>
#include <sstream>

namespace GoldSrcAssetConverter
{
//GLB container layout (glTF 2.0 specification, section 4.4).
constexpr std::uint32_t s_GLBMagic        = 0x46546C67;
constexpr std::uint32_t s_GLBVersion      = 2;
constexpr std::uint32_t s_GLBHeaderBytes  = 12;
constexpr std::uint32_t s_GLBChunkHeader  = 8;
constexpr std::uint32_t s_GLBChunkJSON    = 0x4E4F534A;
constexpr std::uint32_t s_GLBChunkBIN     = 0x004E4942;
constexpr std::uint32_t s_GLBAlignment    = 4;
constexpr std::size_t   s_GLBLengthOffset = 8;

static std::uint32_t ReadLittleEndian32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value{};

    for (std::size_t i = 0; i < sizeof(value); ++i)
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * CHAR_BIT);

    return value;
}

bool CGLTFDocumentBuilder::Write(OutputFormat format, std::vector<std::uint8_t>& file, std::string& error)
{
    file.clear();

    //A buffer must have at least one byte.
    if (!m_Buffer.empty())
    {
        tinygltf::Buffer buffer;
        buffer.data = std::move(m_Buffer);

        m_Model.buffers.push_back(std::move(buffer));
    }

    m_Buffer = {};

    tinygltf::TinyGLTF writer;
    std::ostringstream stream(std::ios::binary);

    const bool bBinary = format == OutputFormat::GLB;

    if (!writer.WriteGltfSceneToStream(&m_Model, stream, !bBinary, bBinary))
    {
        error = "tinygltf failed to serialize the document";
        return false;
    }

    const auto text = stream.str();
    file.assign(text.begin(), text.end());

    if (bBinary)
        return ValidateGLBContainer(file, error);

    return true;
}

bool ValidateGLBContainer(const std::vector<std::uint8_t>& file, std::string& error)
{
    if (file.size() < s_GLBHeaderBytes + s_GLBChunkHeader)
    {
        error = "GLB file is shorter than its header";
        return false;
    }

    if (ReadLittleEndian32(file, 0) != s_GLBMagic || ReadLittleEndian32(file, sizeof(std::uint32_t)) != s_GLBVersion)
    {
        error = "GLB header has the wrong magic or version";
        return false;
    }

    if (ReadLittleEndian32(file, s_GLBLengthOffset) != file.size())
    {
        error = "GLB header length differs from the file size";
        return false;
    }

    std::size_t offset     = s_GLBHeaderBytes;
    std::size_t chunkIndex = 0;

    while (offset < file.size())
    {
        if (file.size() - offset < s_GLBChunkHeader)
        {
            error = "GLB chunk header runs past the file";
            return false;
        }

        const auto length = ReadLittleEndian32(file, offset);
        const auto type   = ReadLittleEndian32(file, offset + sizeof(std::uint32_t));

        if (length % s_GLBAlignment || file.size() - offset - s_GLBChunkHeader < length)
        {
            error = "GLB chunk " + std::to_string(chunkIndex) + " is misaligned or runs past the file";
            return false;
        }

        const auto expectedType = chunkIndex == 0 ? s_GLBChunkJSON : s_GLBChunkBIN;

        if (chunkIndex > 1 || type != expectedType)
        {
            error = "GLB chunk " + std::to_string(chunkIndex) + " has an unexpected type";
            return false;
        }

        offset += s_GLBChunkHeader + length;
        ++chunkIndex;
    }

    return true;
}
} // namespace GoldSrcAssetConverter
