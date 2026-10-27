#include "BSPEntityLumpParser.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace GoldSrcAssetConverter
{
//ED_ParseEntity turns "angle" -1 into straight up and other negative values (-2) into straight down.
constexpr float BSPAngleUp         = -1.0f;
constexpr float BSPUpPitch         = -90.0f;
constexpr float BSPDownPitch       = 90.0f;
constexpr char  BSPInlineModelMark = '*';
constexpr int   BSPDecimalBase     = 10;

//Components of an origin or angles vector.
constexpr std::size_t BSPVectorComponents = 3;

//Brush entity classes whose HLSDK spawn function turns their angles into a movement direction and clears them (see
//BSPEntityAngleUsage); every trigger_* class does so in CBaseTrigger::InitTrigger.
static const char* const s_MoveDirectionClasses[] = {"func_door", "func_water", "momentary_door", "func_button", "func_conveyor", "func_plat", "func_platrot"};
static const char        s_TriggerClassPrefix[]   = "trigger_";

const std::string* BSPEntity::FindValue(const char* szKey) const
{
    const std::string* pValue = nullptr;
    const auto         length = std::strlen(szKey);

    for (const auto& pair : keyValues)
    {
        auto keyLength = pair.key.size();

        while (keyLength && pair.key[keyLength - 1] == ' ')
            --keyLength;

        if (keyLength == length && pair.key.compare(0, keyLength, szKey) == 0)
            pValue = &pair.value;
    }

    return pValue;
}

//COM_Parse over the entity text: the next token, false at the end of the text.
class CEntityTokenizer
{
public:
    explicit CEntityTokenizer(const std::string& text) :
        m_Text(text)
    {
    }

    bool Next(std::string& token, bool& bQuoted)
    {
        token.clear();
        bQuoted = false;

        for (;;)
        {
            while (m_Position < m_Text.size() && static_cast<unsigned char>(m_Text[m_Position]) <= ' ')
                ++m_Position;

            if (m_Position >= m_Text.size())
                return false;

            if (m_Text.compare(m_Position, 2, "//") != 0)
                break;

            while (m_Position < m_Text.size() && m_Text[m_Position] != '\n')
                ++m_Position;
        }

        const auto c = m_Text[m_Position];

        if (c == '"')
        {
            bQuoted = true;
            ++m_Position;

            while (m_Position < m_Text.size() && m_Text[m_Position] != '"')
                token.push_back(m_Text[m_Position++]);

            if (m_Position < m_Text.size())
                ++m_Position;

            return true;
        }

        if (c == '{' || c == '}')
        {
            token.push_back(c);
            ++m_Position;
            return true;
        }

        while (m_Position < m_Text.size() && static_cast<unsigned char>(m_Text[m_Position]) > ' ' && m_Text[m_Position] != '{' && m_Text[m_Position] != '}' && m_Text[m_Position] != '"')
            token.push_back(m_Text[m_Position++]);

        return true;
    }

private:
    const std::string& m_Text;
    std::size_t        m_Position{};
};

static bool IsBrace(const std::string& token, bool bQuoted, char brace)
{
    return !bQuoted && token.size() == 1 && token[0] == brace;
}

bool ParseBSPEntityLump(const std::string& text, std::vector<BSPEntity>& entities, std::string& error)
{
    entities.clear();

    CEntityTokenizer tokenizer(text);
    std::string      token;
    bool             bQuoted{};

    while (tokenizer.Next(token, bQuoted))
    {
        if (!IsBrace(token, bQuoted, '{'))
        {
            error = "entity " + std::to_string(entities.size()) + ": found \"" + token + "\" when expecting {";
            return false;
        }

        BSPEntity entity;
        entity.index = static_cast<std::uint32_t>(entities.size());

        for (;;)
        {
            if (!tokenizer.Next(token, bQuoted))
            {
                error = "entity " + std::to_string(entity.index) + ": end of text without closing brace";
                return false;
            }

            if (IsBrace(token, bQuoted, '}'))
                break;

            BSPEntityKeyValue pair;
            pair.key = token;

            if (!tokenizer.Next(token, bQuoted))
            {
                error = "entity " + std::to_string(entity.index) + ": end of text without closing brace";
                return false;
            }

            if (IsBrace(token, bQuoted, '}'))
            {
                error = "entity " + std::to_string(entity.index) + ": closing brace without data for key \"" + pair.key + "\"";
                return false;
            }

            pair.value = token;
            entity.keyValues.push_back(std::move(pair));
        }

        entities.push_back(std::move(entity));
    }

    return true;
}

//Up to three numbers, as sscanf("%f %f %f") reads a vector: those missing are 0. False when the text holds anything
//that is not a number.
static bool ParseVector(const std::string& text, GoldSrcRender::Vector3& vector)
{
    float       values[BSPVectorComponents]{};
    const char* pText = text.c_str();

    for (auto& value : values)
    {
        char*      pEnd   = nullptr;
        const auto number = std::strtod(pText, &pEnd);

        if (pEnd == pText)
            break;

        value = static_cast<float>(number);
        pText = pEnd;
    }

    vector = {values[0], values[1], values[2]};

    while (*pText && std::isspace(static_cast<unsigned char>(*pText)))
        ++pText;

    return !*pText && std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

static bool ParseInlineModelIndex(const std::string& model, std::uint32_t& index)
{
    if (model.size() < 2 || model[0] != BSPInlineModelMark)
        return false;

    std::uint64_t value = 0;

    for (std::size_t i = 1; i < model.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(model[i])))
            return false;

        value = value * BSPDecimalBase + static_cast<std::uint64_t>(model[i] - '0');

        if (value > std::numeric_limits<std::uint32_t>::max())
            return false;
    }

    index = static_cast<std::uint32_t>(value);

    return true;
}

static BSPEntityAngleUsage GetAngleUsage(const std::string& classname)
{
    if (classname.compare(0, sizeof(s_TriggerClassPrefix) - 1, s_TriggerClassPrefix) == 0)
        return BSPEntityAngleUsage::MoveDirection;

    for (const auto* szClass : s_MoveDirectionClasses)
    {
        if (classname == szClass)
            return BSPEntityAngleUsage::MoveDirection;
    }

    return BSPEntityAngleUsage::Rotation;
}

static std::string TrimKey(const std::string& key)
{
    auto length = key.size();

    while (length && key[length - 1] == ' ')
        --length;

    return key.substr(0, length);
}

BSPEntityInfo InterpretBSPEntity(const BSPEntity& entity, std::uint32_t modelCount, std::vector<std::string>& invalidValues)
{
    BSPEntityInfo info;

    if (const auto* pClassname = entity.FindValue("classname"))
        info.classname = *pClassname;

    info.bWorldspawn = info.classname == "worldspawn";
    info.angleUsage  = GetAngleUsage(info.classname);

    if (const auto* pModel = entity.FindValue("model"))
    {
        info.model = *pModel;

        std::uint32_t index{};

        if (info.model.empty())
            info.modelKind = BSPEntityModelKind::None;
        else if (info.model[0] != BSPInlineModelMark)
            info.modelKind = BSPEntityModelKind::External;
        else if (ParseInlineModelIndex(info.model, index) && index < modelCount)
        {
            info.modelKind  = BSPEntityModelKind::Inline;
            info.modelIndex = index;
        }
        else
            info.modelKind = BSPEntityModelKind::Invalid;
    }

    //The pairs are applied in order, as the game DLL receives them.
    for (const auto& pair : entity.keyValues)
    {
        const auto key = TrimKey(pair.key);

        if (key == "origin")
        {
            if (!ParseVector(pair.value, info.origin))
                invalidValues.push_back(key);

            info.bHasOrigin = true;
        }
        else if (key == "angles")
        {
            if (!ParseVector(pair.value, info.angles))
                invalidValues.push_back(key);

            info.bHasAngles = true;
        }
        else if (key == "angle")
        {
            const auto yaw = static_cast<float>(std::atof(pair.value.c_str()));

            if (!std::isfinite(yaw))
                invalidValues.push_back(key);
            else if (yaw >= 0)
                info.angles = {info.angles.x, yaw, info.angles.z};
            else if (static_cast<int>(yaw) == static_cast<int>(BSPAngleUp))
                info.angles = {BSPUpPitch, 0, 0};
            else
                info.angles = {BSPDownPitch, 0, 0};

            info.bHasAngles = true;
        }
    }

    return info;
}
} // namespace GoldSrcAssetConverter
