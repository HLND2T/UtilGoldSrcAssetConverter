#include "BSPConverterInternal.h"

#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace GoldSrcAssetConverter
{
//UTF-8 lead bytes and the continuation bytes that follow them (RFC 3629).
constexpr unsigned char s_ASCIILimit          = 0x80;
constexpr unsigned char s_ContinuationMask    = 0xC0;
constexpr unsigned char s_ContinuationPattern = 0x80;
constexpr unsigned char s_TwoBytePattern      = 0xC0;
constexpr unsigned char s_TwoByteMask         = 0xE0;
constexpr unsigned char s_TwoByteMin          = 0xC2;
constexpr unsigned char s_ThreeBytePattern    = 0xE0;
constexpr unsigned char s_ThreeByteMask       = 0xF0;
constexpr unsigned char s_FourBytePattern     = 0xF0;
constexpr unsigned char s_FourByteMask        = 0xF8;
constexpr unsigned char s_FourByteMax         = 0xF4;
constexpr unsigned char s_Latin1LeadBase      = 0xC0;
constexpr unsigned char s_Latin1PayloadBits   = 6;
constexpr unsigned char s_Latin1PayloadMask   = 0x3F;

//Miptex name prefixes: '{' textures are alpha tested, '-' animations tile at random.
constexpr char s_MaskedTexturePrefix = '{';
constexpr char s_RandomTilingPrefix  = '-';
constexpr int  s_DecimalBase         = 10;

//Highest lightstyle of a light: the engine animates 256 styles.
constexpr int s_MaxLightStyle = 255;

//Entity key of the optional Forward+ cutoff of a light, and the room its supported interval takes in a diagnostic.
constexpr char        s_ForwardPlusCutoffKey[] = "_forwardplus_cutoff_lux";
constexpr std::size_t s_IntervalCapacity       = 64;

//Overlong three-byte sequences (E0 followed by less than A0) and surrogates (ED followed by A0 or more) are invalid,
//as are four-byte sequences beyond U+10FFFF (F4 followed by 90 or more) or overlong (F0 followed by less than 90).
constexpr unsigned char s_ThreeByteOverlongLead = 0xE0;
constexpr unsigned char s_SurrogateLead         = 0xED;
constexpr unsigned char s_SecondByteBoundary    = 0xA0;
constexpr unsigned char s_FourByteOverlongLead  = 0xF0;
constexpr unsigned char s_FourByteBoundary      = 0x90;

static bool IsUTF8(const std::string& bytes)
{
    for (std::size_t i = 0; i < bytes.size();)
    {
        const auto lead = static_cast<unsigned char>(bytes[i]);

        std::size_t length{};

        if (lead < s_ASCIILimit)
            length = 1;
        else if ((lead & s_TwoByteMask) == s_TwoBytePattern && lead >= s_TwoByteMin)
            length = 2;
        else if ((lead & s_ThreeByteMask) == s_ThreeBytePattern)
            length = 3;
        else if ((lead & s_FourByteMask) == s_FourBytePattern && lead <= s_FourByteMax)
            length = 4;
        else
            return false;

        if (bytes.size() - i < length)
            return false;

        for (std::size_t j = 1; j < length; ++j)
        {
            if ((static_cast<unsigned char>(bytes[i + j]) & s_ContinuationMask) != s_ContinuationPattern)
                return false;
        }

        if (length > 1)
        {
            const auto second = static_cast<unsigned char>(bytes[i + 1]);

            if ((lead == s_ThreeByteOverlongLead && second < s_SecondByteBoundary) || (lead == s_SurrogateLead && second >= s_SecondByteBoundary) ||
                (lead == s_FourByteOverlongLead && second < s_FourByteBoundary) || (lead == s_FourByteMax && second >= s_FourByteBoundary))
                return false;
        }

        i += length;
    }

    return true;
}

std::string MakeJSONString(const std::string& bytes, bool& bLatin1)
{
    bLatin1 = !IsUTF8(bytes);

    if (!bLatin1)
        return bytes;

    std::string text;

    for (const auto c : bytes)
    {
        const auto value = static_cast<unsigned char>(c);

        if (value < s_ASCIILimit)
            text.push_back(c);
        else
        {
            text.push_back(static_cast<char>(s_Latin1LeadBase | (value >> s_Latin1PayloadBits)));
            text.push_back(static_cast<char>(s_ContinuationPattern | (value & s_Latin1PayloadMask)));
        }
    }

    return text;
}

std::string GetMiptexName(const BSPConversionSource& source, std::uint32_t miptex)
{
    const auto& miptexes = source.reader.GetMiptexes();

    if (miptex >= miptexes.size() || !miptexes[miptex].bPresent)
        return "missing";

    bool bLatin1{};

    return MakeJSONString(miptexes[miptex].name, bLatin1);
}

static nlohmann::json MakeVector(const Vector3& vector)
{
    return nlohmann::json::array({vector.x, vector.y, vector.z});
}

//JSON index or -1 for none.
static nlohmann::json MakeIndex(std::uint32_t index, std::uint32_t none)
{
    return index == none ? nlohmann::json(-1) : nlohmann::json(index);
}

static const char* GetModelKindName(BSPEntityModelKind kind)
{
    switch (kind)
    {
        case BSPEntityModelKind::Inline:
            return "inline";
        case BSPEntityModelKind::External:
            return "external";
        case BSPEntityModelKind::Invalid:
            return "invalid";
        case BSPEntityModelKind::None:
            break;
    }

    return "none";
}

//Render keys parsed as the game DLL reads them (atoi, and a vector for rendercolor), for readers that do not parse
//the key values themselves.
static nlohmann::json MakeRenderProperties(const BSPEntity& entity)
{
    nlohmann::json render = nlohmann::json::object();

    const char* const integerKeys[] = {"rendermode", "renderamt", "renderfx"};

    for (const auto* szKey : integerKeys)
    {
        if (const auto* pValue = entity.FindValue(szKey))
            render[szKey] = std::atoi(pValue->c_str());
    }

    if (const auto* pValue = entity.FindValue("rendercolor"))
    {
        nlohmann::json color   = nlohmann::json::array();
        const char*    pText   = pValue->c_str();
        constexpr int  s_Count = 3;

        for (int i = 0; i < s_Count; ++i)
        {
            char* pEnd = nullptr;
            color.push_back(static_cast<int>(std::strtol(pText, &pEnd, s_DecimalBase)));
            pText = pEnd;
        }

        render["rendercolor"] = color;
    }

    return render;
}

//A conservative zero test: malformed, overflowing and non-finite values must not opt into static ownership.
static bool IsZeroRenderProperty(const BSPEntity& entity, const char* szKey)
{
    const auto* pValue = entity.FindValue(szKey);
    if (!pValue)
        return true;

    char* pEnd{};
    errno            = 0;
    const auto value = std::strtod(pValue->c_str(), &pEnd);
    if (pEnd == pValue->c_str() || errno == ERANGE || !std::isfinite(value) || value != 0)
        return false;

    while (std::isspace(static_cast<unsigned char>(*pEnd)))
        ++pEnd;

    return !*pEnd;
}

static bool IsStaticProp(const BSPEntity& entity, const BSPEntityInfo& info)
{
    const bool bClass = info.classname == "func_wall" || info.classname == "func_illusionary";
    if (!bClass || info.modelKind != BSPEntityModelKind::Inline)
        return false;

    for (const auto* szKey : {"targetname", "globalname"})
    {
        const auto* pValue = entity.FindValue(szKey);
        if (pValue && !pValue->empty())
            return false;
    }

    for (const auto* szKey : {"rendermode", "renderfx", "effects", "frame", "spawnflags"})
    {
        if (!IsZeroRenderProperty(entity, szKey))
            return false;
    }

    return true;
}

std::vector<std::uint32_t> MakeEntityRenderFlags(const BSPConversionSource& source, CConversionDiagnostics& diagnostics)
{
    std::vector<std::uint32_t> flags(source.entities.size());
    std::vector<bool>          staticModels(source.world.submodels.size()), dynamicModels(source.world.submodels.size());

    for (std::size_t i = 0; i < source.entities.size(); ++i)
    {
        const auto& info = source.entityInfos[i];
        if (info.modelKind != BSPEntityModelKind::Inline)
            continue;

        if (IsStaticProp(source.entities[i], info))
        {
            flags[i]                      = BSPRenderFlagStaticProp;
            staticModels[info.modelIndex] = true;
        }
        else
        {
            dynamicModels[info.modelIndex] = true;
        }
    }

    for (std::size_t model = 1; model < staticModels.size(); ++model)
    {
        if (staticModels[model] && dynamicModels[model])
            diagnostics.Report(DiagnosticSeverity::Info, DiagnosticCode::MixedRenderOwnership, "submodel " + std::to_string(model) + ": static and runtime entity references share geometry; all references remain runtime-owned");
    }

    for (std::size_t i = 0; i < flags.size(); ++i)
    {
        if (flags[i] && dynamicModels[source.entityInfos[i].modelIndex])
            flags[i] = 0;
    }

    return flags;
}

nlohmann::json MakeEntityExtras(const BSPConversionSource& source, std::uint32_t entityIndex, CConversionDiagnostics& diagnostics)
{
    const auto& entity  = source.entities[entityIndex];
    const auto& info    = source.entityInfos[entityIndex];
    const auto& options = source.pRequest->options;

    nlohmann::json extras;
    extras["schemaVersion"] = ExtrasSchemaVersion;
    extras["entityIndex"]   = entity.index;
    extras["renderFlags"]   = 0;

    bool bLatin1{};
    extras["classname"] = MakeJSONString(info.classname, bLatin1);

    if (!info.model.empty())
        extras["originalModelString"] = MakeJSONString(info.model, bLatin1);

    extras["modelKind"]     = GetModelKindName(info.modelKind);
    extras["bspModelIndex"] = info.bWorldspawn ? nlohmann::json(0) : info.modelKind == BSPEntityModelKind::Inline ? nlohmann::json(info.modelIndex) :
                                                                                                                    nlohmann::json(-1);

    if (info.bHasOrigin)
        extras["originalOrigin"] = MakeVector(info.origin);

    if (info.bHasAngles)
        extras["originalAngles"] = MakeVector(info.angles);

    extras["angleUsage"] = info.angleUsage == BSPEntityAngleUsage::MoveDirection ? "moveDirection" : "rotation";

    const auto render = MakeRenderProperties(entity);

    if (!render.empty())
        extras["render"] = render;

    if (!options.bPreserveEntityKeyValues)
        return extras;

    //Pairs in file order, duplicates included; a third element "latin1" marks a pair whose bytes are not UTF-8 and were
    //decoded as Latin-1.
    nlohmann::json keyValues = nlohmann::json::array();

    for (const auto& pair : entity.keyValues)
    {
        bool       bKeyLatin1{};
        bool       bValueLatin1{};
        const auto key   = MakeJSONString(pair.key, bKeyLatin1);
        const auto value = MakeJSONString(pair.value, bValueLatin1);

        auto element = nlohmann::json::array({key, value});

        if (bKeyLatin1 || bValueLatin1)
        {
            element.push_back("latin1");
            diagnostics.Report(DiagnosticSeverity::Info, DiagnosticCode::NonUTF8Text, "entity " + std::to_string(entity.index) + " key \"" + key + "\": text is not UTF-8 and is stored decoded as Latin-1");
        }

        keyValues.push_back(std::move(element));
    }

    extras["keyvalues"] = std::move(keyValues);

    return extras;
}

//The closed interval [min, max] in the shortest %g form, for diagnostics.
static std::string FormatInterval(double min, double max)
{
    char szInterval[s_IntervalCapacity]{};
    std::snprintf(szInterval, sizeof(szInterval), "[%g, %g]", min, max);
    return szInterval;
}

nlohmann::json MakeLightExtras(const BSPConversionSource& source, std::uint32_t entityIndex, CConversionDiagnostics& diagnostics)
{
    const auto& entity = source.entities[entityIndex];

    nlohmann::json extras;
    extras["schemaVersion"] = ExtrasSchemaVersion;

    //The style CLight::KeyValue reads with atoi: qcsg's SetLightStyles writes the one it gives the targetname, the
    //mapper the preset animations; the engine has styles 0 to s_MaxLightStyle.
    int style{};

    if (const auto* pValue = entity.FindValue("style"))
        style = std::atoi(pValue->c_str());

    if (style < 0 || style > s_MaxLightStyle)
    {
        diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidEntityValue, "entity " + std::to_string(entity.index) + ": key \"style\" must be 0 to " + std::to_string(s_MaxLightStyle) + "; the light keeps style 0");
        style = 0;
    }

    extras["lightStyle"] = style;

    //The cutoff is a renderer policy in scene meters, independent of the light's intensity and root scale. Invalid
    //policy metadata retains the light and inherits the renderer's global value rather than discarding illumination.
    const auto* pCutoff = entity.FindValue(s_ForwardPlusCutoffKey);
    if (source.entityInfos[entityIndex].classname == "light" && pCutoff)
    {
        const char* pBegin = pCutoff->c_str();
        char*       pEnd{};
        errno               = 0;
        const double cutoff = std::strtod(pBegin, &pEnd);
        while (std::isspace(static_cast<unsigned char>(*pEnd)))
            ++pEnd;
        const bool bValidCutoff = pEnd != pBegin && *pEnd == '\0' && errno != ERANGE && std::isfinite(cutoff) && cutoff >= MinForwardPlusCutoffLux && cutoff <= MaxForwardPlusCutoffLux;
        if (bValidCutoff)
        {
            extras["forwardPlus"]["cutoffLux"] = cutoff;
        }
        else
        {
            diagnostics.Report(DiagnosticSeverity::Warning, DiagnosticCode::InvalidEntityValue, "entity " + std::to_string(entity.index) + ": key \"" + s_ForwardPlusCutoffKey + "\" must be one finite number in " + FormatInterval(MinForwardPlusCutoffLux, MaxForwardPlusCutoffLux) + "; omitted to inherit the global cutoff");
        }
    }

    if (const auto* pValue = entity.FindValue("targetname"); pValue && !pValue->empty())
    {
        bool bLatin1{};
        extras["targetname"] = MakeJSONString(*pValue, bLatin1);
    }

    return extras;
}

nlohmann::json MakePrimitiveExtras(const BSPConversionSource& source, std::uint32_t model, BSPGeometryKind kind, const BSPPrimitive& primitive)
{
    nlohmann::json extras;
    extras["schemaVersion"] = ExtrasSchemaVersion;
    extras["bspModelIndex"] = model;
    extras["geometryKind"]  = GetGeometryKindName(kind);
    extras["faceCount"]     = primitive.faces.size();

    if (!source.pRequest->options.bPreserveSurfaceMetadata)
        return extras;

    const bool bStyles = source.pRequest->options.bPreserveLightstyles;

    //One record per face, with the columns of the asset's faceRecordFields.
    nlohmann::json faces = nlohmann::json::array();

    for (const auto& record : primitive.faces)
    {
        nlohmann::json row = nlohmann::json::array({record.face, record.firstIndex, record.indexCount, record.texInfo, MakeIndex(record.sourceSurface, BSPNoSurface),
                                                    record.lightmapPage, record.lightmapX, record.lightmapY, record.lightmapWidth, record.lightmapHeight});

        for (std::uint32_t layer = 0; layer < GoldSrcRender::BSPLightmapStyles; ++layer)
            row.push_back(bStyles || !layer ? record.styles[layer] : GoldSrcRender::BSPNoLightstyle);

        row.push_back(record.lightOffset);
        row.push_back(record.textureMins[0]);
        row.push_back(record.textureMins[1]);

        faces.push_back(std::move(row));
    }

    extras["faces"] = std::move(faces);

    return extras;
}

//Liquid of a water texture by the compilers' naming convention (ZHLT's TextureContents): "!lava"/"*lava" is lava,
//"!slime"/"*slime" slime, other turbulent textures water. The leaf contents are not read.
static const char* GetLiquidType(const std::string& name)
{
    constexpr char s_Lava[]  = "lava";
    constexpr char s_Slime[] = "slime";

    if (name.size() > 1 && name.compare(1, sizeof(s_Lava) - 1, s_Lava) == 0)
        return "lava";

    if (name.size() > 1 && name.compare(1, sizeof(s_Slime) - 1, s_Slime) == 0)
        return "slime";

    return "water";
}

nlohmann::json MakeMaterialExtras(const BSPConversionSource& source, const BSPMaterialKey& key, CBSPTextureConverter& textures)
{
    const auto& miptexes = source.reader.GetMiptexes();
    const auto* pMiptex  = key.miptex < miptexes.size() && miptexes[key.miptex].bPresent ? &miptexes[key.miptex] : nullptr;

    nlohmann::json extras;
    extras["schemaVersion"] = ExtrasSchemaVersion;
    extras["miptexIndex"]   = MakeIndex(key.miptex, NoMiptexIndex);

    bool bLatin1{};

    if (pMiptex)
    {
        extras["originalTextureName"] = MakeJSONString(pMiptex->name, bLatin1);

        if (pMiptex->rawName != pMiptex->name)
            extras["storedTextureName"] = MakeJSONString(pMiptex->rawName, bLatin1);

        extras["textureSize"] = nlohmann::json::array({pMiptex->width, pMiptex->height});
        extras["embedded"]    = pMiptex->bEmbedded;
    }

    switch (key.kind)
    {
        case BSPGeometryKind::Sky:
            extras["surfaceType"]  = "sky";
            extras["isGoldSrcSky"] = true;
            extras["placeholder"]  = true;
            break;

        case BSPGeometryKind::Water:
            extras["surfaceType"] = "water";
            extras["liquidType"]  = pMiptex ? GetLiquidType(pMiptex->name) : "water";
            extras["turbulent"]   = true;
            break;

        case BSPGeometryKind::Surface:
            extras["surfaceType"] = "regular";
            break;
    }

    const auto displayedSlot = key.binding == MissingBinding ? NoMiptexIndex : source.bindingSlots[key.binding];

    extras["lightmapped"]    = key.bLightmapped;
    extras["alphaTest"]      = pMiptex && pMiptex->name[0] == s_MaskedTexturePrefix;
    extras["missingTexture"] = key.kind != BSPGeometryKind::Sky && (displayedSlot == NoMiptexIndex || textures.GetMiptexTexture(displayedSlot) < 0);

    if (key.kind == BSPGeometryKind::Surface)
    {
        //Drawn faces without lightmap are those whose texinfo has TEX_SPECIAL (BSPWorldTextureGroup::bUnlit).
        extras["texSpecial"] = key.bTexSpecial;

        if (key.scrollWidth)
            extras["scroll"] = {{"width", key.scrollWidth}, {"tiled", true}};
    }

    if (displayedSlot != key.miptex)
        extras["displayedMiptexIndex"] = MakeIndex(displayedSlot, NoMiptexIndex);

    //Animation sequences (BSPWorldTextureGroup::animation): the frames in order, as miptex indices and glTF textures.
    if (key.animationCount)
    {
        auto makeFrames = [&](std::uint32_t first, std::uint32_t count, nlohmann::json& slots, nlohmann::json& frameTextures) {
            slots         = nlohmann::json::array();
            frameTextures = nlohmann::json::array();

            for (auto frame = first; frame < first + count; ++frame)
            {
                const auto binding = source.world.textureFrames[frame].binding;
                const auto slot    = binding == MissingBinding ? NoMiptexIndex : source.bindingSlots[binding];

                slots.push_back(MakeIndex(slot, NoMiptexIndex));
                frameTextures.push_back(slot == NoMiptexIndex ? -1 : textures.GetMiptexTexture(slot));
            }
        };

        nlohmann::json animation;
        animation["framesPerSecond"] = GoldSrcRender::BSPTextureAnimationRate;
        animation["randomTiling"]    = pMiptex && pMiptex->name[0] == s_RandomTilingPrefix;

        makeFrames(key.animationFirst, key.animationCount, animation["frames"], animation["frameTextures"]);
        makeFrames(key.alternateFirst, key.alternateCount, animation["alternateFrames"], animation["alternateFrameTextures"]);

        extras["animation"] = std::move(animation);
    }

    if (key.detailBinding != MissingBinding)
    {
        const auto& binding = source.pRequest->asset.pDetailTextures[key.detailBinding];

        nlohmann::json detail;
        detail["scale"]   = nlohmann::json::array({binding.scaleS, binding.scaleT});
        detail["assetId"] = std::to_string(binding.texture);
        detail["texture"] = textures.GetDetailTexture(key.detailBinding);

        extras["detail"] = std::move(detail);
    }

    return extras;
}
} // namespace GoldSrcAssetConverter
