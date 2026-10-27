#pragma once

#include "GoldSrcRenderBSPTestFile.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

//The BSP the converter tests convert: GoldSrcRenderBSPTestFile.h's world (eight 64x64 quads on z = 0, see there for
//the faces) with these changes:
//
//  - the texture lump embeds the pixels of "floor" (slot 0) and "{grate" (slot 4) with a palette;
//  - models: 0 lists every face; 1 lists faces 4-7 ({grate, degenerate floor, floor2, trigger); 2 lists faces 0-1
//    (floor and sky); 3 lists faces 2-3 (!water and wall); 4 lists face 6 and no entity shows it;
//  - the entity lump (ConverterTestEntities) has a worldspawn that is not entity 0, brush entities sharing model 1,
//    point entities, an external model and an invalid inline model reference;
//  - a clipnode lump for hulls 1-3: x > 1000 is empty, otherwise z > 0 is empty and the rest solid.
namespace GoldSrcConverterTest
{
using namespace GoldSrcRenderTest;

constexpr std::uint32_t ConverterTestModelCount    = 5;
constexpr std::uint32_t ConverterTestSharedModel   = 1;
constexpr std::uint32_t ConverterTestSkyModel      = 2;
constexpr std::uint32_t ConverterTestWaterModel    = 3;
constexpr std::uint32_t ConverterTestUnusedModel   = 4;
constexpr std::uint32_t ConverterTestPaletteColors = 256;
constexpr std::uint32_t ConverterTestMiptexMips    = 4;
constexpr std::uint32_t ConverterTestClipNodeLump  = 9;
constexpr std::int32_t  ConverterTestContentsEmpty = -1;
constexpr std::int32_t  ConverterTestContentsSolid = -2;
constexpr std::uint8_t  ConverterTestTransparent   = 255;

//Entity indices in ConverterTestEntities.
constexpr std::uint32_t ConverterTestWallEntity       = 0;
constexpr std::uint32_t ConverterTestWorldspawnEntity = 1;
constexpr std::uint32_t ConverterTestDoorEntity       = 2;
constexpr std::uint32_t ConverterTestButtonEntity     = 3;
constexpr std::uint32_t ConverterTestSharedEntity     = 4;
constexpr std::uint32_t ConverterTestPlayerEntity     = 5;
constexpr std::uint32_t ConverterTestLightEntity      = 6;
constexpr std::uint32_t ConverterTestInvalidEntity    = 7;
constexpr std::uint32_t ConverterTestSpriteEntity     = 8;
constexpr std::uint32_t ConverterTestPathEntity       = 9;
constexpr std::uint32_t ConverterTestEntityCount      = 10;

//Entity 0 is a brush entity: the worldspawn is not assumed to come first. The worldspawn's message holds the Latin-1
//byte 0xE9 (not UTF-8) and a repeated key.
constexpr char ConverterTestEntities[] =
    "{\n\"classname\" \"func_wall\"\n\"model\" \"*1\"\n\"origin\" \"100 200 300\"\n\"angles\" \"10 90 30\"\n\"rendermode\" \"4\"\n\"renderamt\" \"200\"\n}\n"
    "{\n\"classname\" \"worldspawn\"\n\"wad\" \"\\valve\\halflife.wad\"\n\"skyname\" \"desert\"\n\"message\" \"caf\xE9\"\n\"_note\" \"first\"\n\"_note\" \"second\"\n\"custom_key\" \"custom value\"\n}\n"
    "{\n\"classname\" \"func_door\"\n\"model\" \"*2\"\n\"origin\" \"10 0 0\"\n\"angles\" \"0 90 0\"\n\"speed\" \"100\"\n\"wait\" \"4\"\n\"targetname\" \"door_01\"\n}\n"
    "{\n\"classname\" \"func_button\"\n\"model\" \"*3\"\n}\n"
    "{\n\"classname\" \"func_wall\"\n\"model\" \"*1\"\n\"origin\" \"-50 0 0\"\n}\n"
    "{\n\"classname\" \"info_player_start\"\n\"origin\" \"1 2 3\"\n\"angle\" \"45\"\n}\n"
    "{\n\"classname\" \"light\"\n\"origin\" \"0 0 64\"\n\"_light\" \"255 255 255 200\"\n\"style\" \"5\"\n}\n"
    "{\n\"classname\" \"func_illusionary\"\n\"model\" \"*7\"\n}\n"
    "{\n\"classname\" \"env_sprite\"\n\"model\" \"sprites/glow01.spr\"\n\"origin\" \"8 8 8\"\n}\n"
    "{\n\"classname\" \"path_corner\"\n\"origin\" \"5 5 5\"\n\"angle\" \"-1\"\n}\n";

//Palette color of index i of the embedded miptexes.
inline std::array<std::uint8_t, 3> ConverterTestPaletteColor(std::uint32_t index)
{
    constexpr std::uint32_t s_ColorMask = 0xFF;

    return {static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(s_ColorMask - index), static_cast<std::uint8_t>(index / 2)};
}

//Palette index of texel i of an embedded miptex: every index in turn, so '{' textures have transparent texels.
inline std::uint8_t ConverterTestTexel(std::uint32_t i)
{
    return static_cast<std::uint8_t>(i % ConverterTestPaletteColors);
}

//A miptex record with its four mips and palette (pixels) or the header alone.
inline std::vector<std::uint8_t> MakeConverterTestMiptex(const char* szName, std::uint32_t width, std::uint32_t height, bool bPixels)
{
    CBSPWriter record;
    record.Name(szName, TestBSPMiptexNameBytes);
    record.UInt32(width);
    record.UInt32(height);

    std::uint32_t offset = TestBSPMiptexBytes;

    for (std::uint32_t mip = 0; mip < ConverterTestMiptexMips; ++mip)
    {
        record.UInt32(bPixels ? offset : 0);
        offset += (width >> mip) * (height >> mip);
    }

    if (!bPixels)
        return record.Bytes();

    for (std::uint32_t mip = 0; mip < ConverterTestMiptexMips; ++mip)
    {
        for (std::uint32_t i = 0; i < (width >> mip) * (height >> mip); ++i)
            record.UInt8(ConverterTestTexel(i));
    }

    record.UInt16(static_cast<std::uint16_t>(ConverterTestPaletteColors));

    for (std::uint32_t i = 0; i < ConverterTestPaletteColors; ++i)
    {
        for (const auto component : ConverterTestPaletteColor(i))
            record.UInt8(component);
    }

    //Records are padded to 4 bytes, as the compilers write them.
    record.UInt16(0);

    return record.Bytes();
}

inline std::vector<std::uint8_t> MakeConverterTestTextureLump()
{
    const char* names[TestBSPMiptexCount] = {"floor", "sky", "!water", "wall", "{grate", "floor2", "trigger"};

    std::vector<std::vector<std::uint8_t>> records;

    for (std::uint32_t i = 0; i < TestBSPMiptexCount; ++i)
    {
        const auto width   = i == TestBSPFloorSlot ? TestBSPFloorWidth : TestBSPTextureSize;
        const auto height  = i == TestBSPFloorSlot ? TestBSPFloorHeight : TestBSPTextureSize;
        const bool bPixels = i == TestBSPFloorSlot || i == TestBSPGrateSlot;

        records.push_back(MakeConverterTestMiptex(names[i], width, height, bPixels));
    }

    CBSPWriter lump;
    lump.Int32(TestBSPMiptexCount);

    auto offset = static_cast<std::uint32_t>(sizeof(std::int32_t) * (1 + TestBSPMiptexCount));

    for (const auto& record : records)
    {
        lump.Int32(static_cast<std::int32_t>(offset));
        offset += static_cast<std::uint32_t>(record.size());
    }

    for (const auto& record : records)
        lump.Bytes().insert(lump.Bytes().end(), record.begin(), record.end());

    return lump.Bytes();
}

inline void WriteConverterTestModel(CBSPWriter& models, std::int32_t headNode, std::int32_t clipNode, std::int32_t visibleLeaves, std::uint32_t firstFace, std::uint32_t faceCount)
{
    models.Vector(0, 0, 0);
    models.Vector(TestBSPQuadSize, TestBSPQuadSize, 0);
    models.Vector(0, 0, 0);
    models.Int32(headNode);

    for (std::uint32_t hull = 1; hull < TestBSPModelHulls; ++hull)
        models.Int32(clipNode);

    models.Int32(visibleLeaves);
    models.Int32(static_cast<std::int32_t>(firstFace));
    models.Int32(static_cast<std::int32_t>(faceCount));
}

inline BSPTestLumps MakeConverterTestLumps(const char* szEntities = ConverterTestEntities)
{
    auto lumps = MakeTestBSPLumps();

    lumps[BSPEntityLump].assign(szEntities, szEntities + std::char_traits<char>::length(szEntities) + 1);
    lumps[BSPTextureLump] = MakeConverterTestTextureLump();

    CBSPWriter models;
    WriteConverterTestModel(models, 0, 0, TestBSPVisibleLeaves, 0, TestBSPFaceCount);
    WriteConverterTestModel(models, TestBSPSubmodelHeadNode, 0, 0, 4, 4);
    WriteConverterTestModel(models, TestBSPSubmodelHeadNode, 0, 0, 0, 2);
    WriteConverterTestModel(models, TestBSPSubmodelHeadNode, 0, 0, 2, 2);
    WriteConverterTestModel(models, TestBSPSubmodelHeadNode, 0, 0, 6, 1);
    lumps[BSPModelLump] = std::move(models.Bytes());

    //Clipnode 0 splits x at 1000 (plane 1): beyond is empty, before is clipnode 1, which splits z at 0 (plane 0).
    CBSPWriter clipNodes;
    clipNodes.Int32(1);
    clipNodes.Int16(ConverterTestContentsEmpty);
    clipNodes.Int16(1);
    clipNodes.Int32(0);
    clipNodes.Int16(ConverterTestContentsEmpty);
    clipNodes.Int16(ConverterTestContentsSolid);
    lumps[ConverterTestClipNodeLump] = std::move(clipNodes.Bytes());

    return lumps;
}

inline std::vector<std::uint8_t> MakeConverterTestBSP(BSPLayout layout = BSPLayout::GoldSrc)
{
    return AssembleTestBSP(MakeConverterTestLumps(), layout);
}

//A WAD3 file holding one miptex lump named szName.
inline std::vector<std::uint8_t> MakeConverterTestWAD(const char* szName, std::uint32_t width, std::uint32_t height)
{
    constexpr std::uint32_t s_HeaderBytes = 12;
    constexpr std::uint8_t  s_TypeMiptex  = 0x43;
    constexpr std::uint32_t s_NameBytes   = 16;

    const auto record = MakeConverterTestMiptex(szName, width, height, true);

    CBSPWriter wad;
    wad.Name("WAD3", sizeof(std::uint32_t));
    wad.Int32(1);
    wad.Int32(static_cast<std::int32_t>(s_HeaderBytes + record.size()));
    wad.Bytes().insert(wad.Bytes().end(), record.begin(), record.end());

    wad.Int32(static_cast<std::int32_t>(s_HeaderBytes));
    wad.Int32(static_cast<std::int32_t>(record.size()));
    wad.Int32(static_cast<std::int32_t>(record.size()));
    wad.UInt8(s_TypeMiptex);
    wad.UInt8(0);
    wad.UInt16(0);
    wad.Name(szName, s_NameBytes);

    return wad.Bytes();
}
} // namespace GoldSrcConverterTest
