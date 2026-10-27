#if !defined(BSP_ENTITY_LUMP_PARSER_H)
#    define BSP_ENTITY_LUMP_PARSER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "GoldSrcRenderBaseTypes.h"

#    include <cstdint>
#    include <string>
#    include <vector>

//Reads the entities of a BSP's entity lump with every key value pair as stored: in file order, duplicates and unknown
//keys included. Interpretation (classname, model reference, origin and angles) follows the engine: GoldSrc's
//ED_ParseEntity passes the pairs in order to the game DLL, whose KeyValue lets a later pair overwrite an earlier one.
namespace GoldSrcAssetConverter
{
struct BSPEntityKeyValue
{
    std::string key;
    std::string value;
};

//index is the entity's position in the lump, the stable identity of its glTF node.
struct BSPEntity
{
    std::uint32_t                  index{};
    std::vector<BSPEntityKeyValue> keyValues;

    //Value of the last pair whose key is szKey, or null. Keys compare as the engine passes them: with case, trailing
    //spaces removed (ED_ParseEntity's "hack to fix keynames with trailing spaces").
    const std::string* FindValue(const char* szKey) const;
};

//Parses the text as GoldSrc's ED_LoadFromFile tokenizes it with COM_Parse: "//" comments, quoted strings, and '{' and
//'}' as tokens of their own. Fails with a message on text that ED_ParseEntity rejects: a missing '{', a key without
//value or an unterminated entity.
bool ParseBSPEntityLump(const std::string& text, std::vector<BSPEntity>& entities, std::string& error);

//How an entity uses the model key: None without one, Inline for a BSP submodel "*n" whose n is a model of the file,
//External for a .mdl/.spr or other path, Invalid for a "*" reference that names no model of the file.
enum class BSPEntityModelKind : std::uint32_t
{
    None,
    Inline,
    External,
    Invalid
};

//What the game DLL does with an entity's angles. Rotation: its model is drawn rotated by them, as the engine's
//R_GetEntityTransform does. MoveDirection: the entity's spawn function turns them into a movement direction and clears
//them (SetMovedir, InitTrigger and CFuncPlat::Setup of the HLSDK, valve/dlls), so the entity is drawn unrotated.
enum class BSPEntityAngleUsage : std::uint32_t
{
    Rotation,
    MoveDirection
};

struct BSPEntityInfo
{
    std::string            classname;
    std::string            model;
    BSPEntityModelKind     modelKind{BSPEntityModelKind::None};
    std::uint32_t          modelIndex{};
    bool                   bWorldspawn{};
    bool                   bHasOrigin{};
    bool                   bHasAngles{};
    GoldSrcRender::Vector3 origin;
    GoldSrcRender::Vector3 angles;
    BSPEntityAngleUsage    angleUsage{BSPEntityAngleUsage::Rotation};
};

//Interprets an entity of a file of modelCount models. origin and angles are parsed as the engine parses vectors
//(three numbers, missing ones 0), and the "angle" key turned into angles as ED_ParseEntity does. Values that are not
//numbers are appended to invalidValues as "key" names.
BSPEntityInfo InterpretBSPEntity(const BSPEntity& entity, std::uint32_t modelCount, std::vector<std::string>& invalidValues);
} // namespace GoldSrcAssetConverter

#endif
