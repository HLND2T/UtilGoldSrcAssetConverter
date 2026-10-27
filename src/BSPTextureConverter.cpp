#include "BSPConverterInternal.h"
#include "PNGEncoder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace GoldSrcAssetConverter
{
//Miptex record layout (BSP texture lump and WAD3 texture lumps): name, size, offsets of four mips, then after the last
//mip a 16-bit color count and the palette.
constexpr std::uint32_t s_MiptexWidthOffset      = 16;
constexpr std::uint32_t s_MiptexHeightOffset     = 20;
constexpr std::uint32_t s_MiptexMipOffsetsOffset = 24;
constexpr std::uint32_t s_MiptexLastMip          = BSPMiptexMips - 1;
constexpr std::uint32_t s_MiptexLastMipShift     = 3;
constexpr std::uint32_t s_PaletteCountBytes      = 2;
constexpr std::uint32_t s_PaletteColors          = 256;
constexpr std::uint32_t s_PaletteColorBytes      = 3;
constexpr std::uint8_t  s_TransparentIndex       = 255;
constexpr std::uint8_t  s_OpaqueAlpha            = 255;
constexpr char          s_MaskedTexturePrefix    = '{';
constexpr std::uint32_t s_RGBChannels            = 3;

//WAD3 layout (the engine's wadinfo_t and lumpinfo_t); TYP_MIPTEX lumps hold miptex records.
constexpr char         s_WAD3Magic[]        = "WAD3";
constexpr std::size_t  s_WADHeaderBytes     = 12;
constexpr std::size_t  s_WADLumpCountOffset = 4;
constexpr std::size_t  s_WADDirectoryOffset = 8;
constexpr std::size_t  s_WADLumpBytes       = 32;
constexpr std::size_t  s_WADLumpPosOffset   = 0;
constexpr std::size_t  s_WADLumpDiskSize    = 4;
constexpr std::size_t  s_WADLumpTypeOffset  = 12;
constexpr std::size_t  s_WADLumpCompression = 13;
constexpr std::size_t  s_WADLumpNameOffset  = 16;
constexpr std::size_t  s_WADLumpNameBytes   = 16;
constexpr std::uint8_t s_WADTypeMiptex      = 0x43;

//Suffixes of the skybox faces, in GoldSrc's R_LoadSkys order.
static const char* const s_SkyboxSuffixes[GoldSrcRender::SkyFaceCount] = {"rt", "bk", "lf", "ft", "up", "dn"};

static std::uint32_t ReadUInt32(const std::uint8_t* pBytes, std::size_t offset)
{
    return GoldSrcRender::ReadUInt32(pBytes, offset);
}

static bool ValidRecordRange(std::uint64_t recordBytes, std::uint64_t offset, std::uint64_t bytes)
{
    return offset <= recordBytes && bytes <= recordBytes - offset;
}

bool DecodeMiptex(const std::uint8_t* pRecord, std::size_t recordBytes, std::vector<std::uint8_t>& texels, std::uint32_t& width, std::uint32_t& height, std::string& error)
{
    if (!pRecord || recordBytes < BSPMiptexBytes)
    {
        error = "the miptex record is shorter than its header";
        return false;
    }

    width  = ReadUInt32(pRecord, s_MiptexWidthOffset);
    height = ReadUInt32(pRecord, s_MiptexHeightOffset);

    if (!width || !height || width > GoldSrcRender::MaxTextureDimension || height > GoldSrcRender::MaxTextureDimension)
    {
        error = "the miptex size is invalid";
        return false;
    }

    const auto pixelOffset   = ReadUInt32(pRecord, s_MiptexMipOffsetsOffset);
    const auto lastMipOffset = ReadUInt32(pRecord, s_MiptexMipOffsetsOffset + s_MiptexLastMip * sizeof(std::uint32_t));
    const auto pixelCount    = static_cast<std::uint64_t>(width) * height;
    const auto lastMipBytes  = static_cast<std::uint64_t>(width >> s_MiptexLastMipShift) * (height >> s_MiptexLastMipShift);
    const auto paletteOffset = static_cast<std::uint64_t>(lastMipOffset) + lastMipBytes + s_PaletteCountBytes;

    if (!pixelOffset || !ValidRecordRange(recordBytes, pixelOffset, pixelCount))
    {
        error = "the miptex record holds no pixels";
        return false;
    }

    if (!ValidRecordRange(recordBytes, paletteOffset, s_PaletteColors * s_PaletteColorBytes))
    {
        error = "the miptex palette runs past the record";
        return false;
    }

    const auto* pPixels  = pRecord + pixelOffset;
    const auto* pPalette = pRecord + paletteOffset;
    const bool  bMasked  = pRecord[0] == s_MaskedTexturePrefix;

    texels.resize(static_cast<std::size_t>(pixelCount) * GoldSrcRender::RGBABytes);

    for (std::size_t i = 0; i < pixelCount; ++i)
    {
        const auto index  = pPixels[i];
        auto*      pTexel = texels.data() + i * GoldSrcRender::RGBABytes;

        if (bMasked && index == s_TransparentIndex)
        {
            std::fill(pTexel, pTexel + GoldSrcRender::RGBABytes, 0);
            continue;
        }

        pTexel[0] = pPalette[index * s_PaletteColorBytes];
        pTexel[1] = pPalette[index * s_PaletteColorBytes + 1];
        pTexel[2] = pPalette[index * s_PaletteColorBytes + 2];
        pTexel[3] = s_OpaqueAlpha;
    }

    return true;
}

bool FindWAD3Texture(const std::uint8_t* pWad, std::uint64_t wadBytes, const char* szName, ResolvedTexture* pTexture) noexcept
{
    if (!pWad || !szName || !pTexture || wadBytes < s_WADHeaderBytes || std::memcmp(pWad, s_WAD3Magic, sizeof(s_WAD3Magic) - 1) != 0)
        return false;

    const auto lumpCount       = static_cast<std::uint64_t>(ReadUInt32(pWad, s_WADLumpCountOffset));
    const auto directoryOffset = static_cast<std::uint64_t>(ReadUInt32(pWad, s_WADDirectoryOffset));

    if (!ValidRecordRange(wadBytes, directoryOffset, lumpCount * s_WADLumpBytes))
        return false;

    for (std::uint64_t i = 0; i < lumpCount; ++i)
    {
        const auto* pLump = pWad + directoryOffset + i * s_WADLumpBytes;

        if (pLump[s_WADLumpTypeOffset] != s_WADTypeMiptex || pLump[s_WADLumpCompression])
            continue;

        char szLumpName[s_WADLumpNameBytes + 1]{};
        std::memcpy(szLumpName, pLump + s_WADLumpNameOffset, s_WADLumpNameBytes);

        std::size_t c = 0;

        while (szLumpName[c] && szName[c] && std::tolower(static_cast<unsigned char>(szLumpName[c])) == std::tolower(static_cast<unsigned char>(szName[c])))
            ++c;

        if (szLumpName[c] || szName[c])
            continue;

        const auto position = static_cast<std::uint64_t>(ReadUInt32(pLump, s_WADLumpPosOffset));
        const auto size     = static_cast<std::uint64_t>(ReadUInt32(pLump, s_WADLumpDiskSize));

        if (!ValidRecordRange(wadBytes, position, size) || size < BSPMiptexBytes)
            return false;

        pTexture->format    = ResolvedTextureFormat::Miptex;
        pTexture->pData     = pWad + position;
        pTexture->dataBytes = size;
        pTexture->width     = ReadUInt32(pTexture->pData, s_MiptexWidthOffset);
        pTexture->height    = ReadUInt32(pTexture->pData, s_MiptexHeightOffset);

        return true;
    }

    return false;
}

CBSPTextureConverter::CBSPTextureConverter(const BSPConversionSource& source, CGLTFDocumentBuilder& document, CConversionDiagnostics& diagnostics) :
    m_Source(source), m_Document(document), m_Diagnostics(diagnostics)
{
}

//Converts what a resolver returned into RGBA8 texels.
static bool DecodeResolvedTexture(const ResolvedTexture& texture, std::vector<std::uint8_t>& texels, std::uint32_t& width, std::uint32_t& height, std::string& error)
{
    if (texture.dataBytes > std::numeric_limits<std::size_t>::max())
    {
        error = "the resolved texture holds more bytes than the process can address";
        return false;
    }

    const auto dataBytes = static_cast<std::size_t>(texture.dataBytes);

    if (texture.format == ResolvedTextureFormat::Miptex)
        return DecodeMiptex(texture.pData, dataBytes, texels, width, height, error);

    if (texture.format != ResolvedTextureFormat::RGBA8 || !texture.pData || !texture.width || !texture.height ||
        texture.width > GoldSrcRender::MaxTextureDimension || texture.height > GoldSrcRender::MaxTextureDimension)
    {
        error = "the resolved texture has no pixels or an invalid size";
        return false;
    }

    const auto bytes = static_cast<std::size_t>(texture.width) * texture.height * GoldSrcRender::RGBABytes;

    if (dataBytes < bytes)
    {
        error = "the resolved texture holds fewer bytes than its size needs";
        return false;
    }

    width  = texture.width;
    height = texture.height;
    texels.assign(texture.pData, texture.pData + bytes);

    return true;
}

bool CBSPTextureConverter::Resolve(const TextureRequest& request, const BSPRawMiptex* pEmbedded, std::vector<std::uint8_t>& texels, std::uint32_t& width, std::uint32_t& height, std::string& error)
{
    auto* pResolver = m_Source.pRequest->pTextureResolver;

    if (pResolver)
    {
        ResolvedTexture texture;

        if (pResolver->ResolveTexture(request, &texture))
        {
            if (DecodeResolvedTexture(texture, texels, width, height, error))
                return true;

            bool bLatin1{};
            m_Diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidTextureData, "texture \"" + MakeJSONString(request.szName, bLatin1) + "\": " + error);
        }
    }

    if (pEmbedded && pEmbedded->bEmbedded)
        return DecodeMiptex(m_Source.reader.GetFile() + pEmbedded->recordOffset, pEmbedded->recordBytes, texels, width, height, error);

    error = "the resolver has no image and the BSP embeds none";
    return false;
}

int CBSPTextureConverter::AddImage(const std::string& name, const std::vector<std::uint8_t>& texels, std::uint32_t width, std::uint32_t height, bool bAlpha)
{
    m_Hash.Update(name.data(), name.size());
    m_Hash.Update(&width, sizeof(width));
    m_Hash.Update(&height, sizeof(height));
    m_Hash.Update(texels.data(), texels.size());

    std::vector<std::uint8_t> png;

    bool bEncoded{};

    if (bAlpha)
        bEncoded = EncodePNG(texels.data(), width, height, GoldSrcRender::RGBABytes, png);
    else
    {
        const auto                pixelCount = static_cast<std::size_t>(width) * height;
        std::vector<std::uint8_t> rgb(pixelCount * s_RGBChannels);

        for (std::size_t i = 0; i < pixelCount; ++i)
            std::memcpy(rgb.data() + i * s_RGBChannels, texels.data() + i * GoldSrcRender::RGBABytes, s_RGBChannels);

        bEncoded = EncodePNG(rgb.data(), width, height, s_RGBChannels, png);
    }

    if (!bEncoded)
        throw std::runtime_error("PNG encoding of texture \"" + name + "\" failed");

    if (m_Sampler < 0)
        m_Sampler = m_Document.AddSampler(TINYGLTF_TEXTURE_FILTER_LINEAR, TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR, TINYGLTF_TEXTURE_WRAP_REPEAT, TINYGLTF_TEXTURE_WRAP_REPEAT);

    const auto image = m_Document.AddPNGImage(name, std::move(png));

    ++m_TextureCount;

    return m_Document.AddTexture(image, m_Sampler, name);
}

int CBSPTextureConverter::GetMiptexTexture(std::uint32_t slot)
{
    const auto it = m_MiptexTextures.find(slot);

    if (it != m_MiptexTextures.end())
        return it->second;

    const auto& miptexes = m_Source.reader.GetMiptexes();

    if (slot >= miptexes.size() || !miptexes[slot].bPresent)
    {
        m_MiptexTextures.emplace(slot, -1);
        return -1;
    }

    const auto& miptex = miptexes[slot];

    //The engine looks the miptex up in the WADs by its stored name (TEX_LoadLump) and blanks the third character of
    //names containing '~' only afterwards.
    TextureRequest request;
    request.kind        = TextureKind::Miptex;
    request.szName      = miptex.rawName.c_str();
    request.miptexIndex = slot;
    request.width       = miptex.width;
    request.height      = miptex.height;
    request.bEmbedded   = miptex.bEmbedded;

    const auto& asset = m_Source.pRequest->asset;

    for (std::uint32_t i = 0; i < asset.textureCount; ++i)
    {
        if (asset.pTextures[i].slot == slot)
            request.assetId = asset.pTextures[i].texture;
    }

    std::vector<std::uint8_t> texels;
    std::uint32_t             width{};
    std::uint32_t             height{};
    std::string               error;

    bool       bLatin1{};
    const auto name = MakeJSONString(miptex.name, bLatin1);

    if (!Resolve(request, &miptex, texels, width, height, error))
    {
        ++m_MissingCount;
        m_Diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::MissingTexture, "miptex " + std::to_string(slot) + " \"" + name + "\": " + error + "; its faces keep their geometry and use a placeholder material");
        m_MiptexTextures.emplace(slot, -1);
        return -1;
    }

    //The engine alpha tests only '{' textures (TEX_TYPE_ALPHA); their transparent texels are kept.
    const bool bAlpha  = miptex.name[0] == s_MaskedTexturePrefix;
    const auto texture = AddImage(name, texels, width, height, bAlpha);

    m_MiptexTextures.emplace(slot, texture);
    m_MiptexAlpha.emplace(slot, bAlpha);
    if (m_Source.pRequest->options.bExportAreaLights)
        m_DecodedImages.emplace(texture, BSPDecodedImage{std::move(texels), width, height});

    return texture;
}

void CBSPTextureConverter::RetainEmissiveImages(const tinygltf::Model& model)
{
    for (auto it = m_DecodedImages.begin(); it != m_DecodedImages.end();)
    {
        const auto used = std::any_of(model.materials.begin(), model.materials.end(), [&](const auto& material) {
            return material.emissiveTexture.index == it->first;
        });
        if (used)
            ++it;
        else
            it = m_DecodedImages.erase(it);
    }
}

bool CBSPTextureConverter::HasAlpha(std::uint32_t slot) const
{
    const auto it = m_MiptexAlpha.find(slot);

    return it != m_MiptexAlpha.end() && it->second;
}

int CBSPTextureConverter::GetDetailTexture(std::uint32_t detailBinding)
{
    const auto it = m_DetailTextures.find(detailBinding);

    if (it != m_DetailTextures.end())
        return it->second;

    const auto& binding  = m_Source.pRequest->asset.pDetailTextures[detailBinding];
    const auto& miptexes = m_Source.reader.GetMiptexes();
    const auto& miptex   = miptexes[binding.slot];

    TextureRequest request;
    request.kind        = TextureKind::Detail;
    request.szName      = miptex.name.c_str();
    request.miptexIndex = binding.slot;
    request.assetId     = binding.texture;
    request.width       = miptex.width;
    request.height      = miptex.height;

    std::vector<std::uint8_t> texels;
    std::uint32_t             width{};
    std::uint32_t             height{};
    std::string               error;

    bool       bLatin1{};
    const auto name = MakeJSONString(miptex.name, bLatin1) + "_detail";

    if (!Resolve(request, nullptr, texels, width, height, error))
    {
        m_Diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::MissingDetailTexture, "detail texture of miptex " + std::to_string(binding.slot) + " \"" + name + "\": " + error);
        m_DetailTextures.emplace(detailBinding, -1);
        return -1;
    }

    const auto texture = AddImage(name, texels, width, height, false);
    m_DetailTextures.emplace(detailBinding, texture);

    return texture;
}

std::array<int, GoldSrcRender::SkyFaceCount> CBSPTextureConverter::GetSkyboxTextures(const std::string& skyName)
{
    std::array<int, GoldSrcRender::SkyFaceCount> textures;

    for (std::uint32_t i = 0; i < GoldSrcRender::SkyFaceCount; ++i)
    {
        const auto name = skyName + s_SkyboxSuffixes[i];

        TextureRequest request;
        request.kind   = TextureKind::SkyboxFace;
        request.szName = name.c_str();

        std::vector<std::uint8_t> texels;
        std::uint32_t             width{};
        std::uint32_t             height{};
        std::string               error;

        bool       bLatin1{};
        const auto displayName = MakeJSONString(name, bLatin1);

        if (!Resolve(request, nullptr, texels, width, height, error))
        {
            m_Diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::MissingSkyboxTexture, "skybox face \"" + displayName + "\": " + error + "; the sky geometry is kept");
            textures[i] = -1;
            continue;
        }

        textures[i] = AddImage(displayName, texels, width, height, false);
    }

    return textures;
}

std::string CBSPTextureConverter::FinishHash()
{
    return m_Hash.FinishHex();
}
} // namespace GoldSrcAssetConverter
