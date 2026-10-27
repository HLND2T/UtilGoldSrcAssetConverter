#if !defined(HALF_LIFE_TEXTURE_LIGHTS_H)
#    define HALF_LIFE_TEXTURE_LIGHTS_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include <cstddef>

namespace GoldSrcAssetConverter
{
constexpr std::size_t s_TextureLightColorComponents = 3;

struct BSPTextureLightDefinition
{
    const char* szName;
    double      color[s_TextureLightColorComponents];
    double      intensity;
};

//Half-Life defaults from Sven Co-op SDK mapping/compilers/hl_lights.rad ("lights.rad for hl.wad", by Hezus).
//Keep stored BSP/WAD names: the engine replaces the third character of names containing '~' after loading.
//These are reference values compiled into the converter; no SDK installation or external .rad file is needed.
static constexpr BSPTextureLightDefinition s_HalfLifeTextureLights[] = {
    {"+0~GYMLIGHT", {255, 251, 129}, 2000},
    {"+0~LIGHT1", {189, 232, 216}, 2000},
    {"+0~LIGHT2A", {255, 251, 129}, 2000},
    {"+0~LIGHT3A", {232, 59, 62}, 2000},
    {"+0~LIGHT4A", {200, 190, 130}, 2000},
    {"+0~LIGHT5A", {80, 150, 200}, 2000},
    {"+0~LIGHT6A", {150, 5, 5}, 2000},
    {"~LIGHT3A", {190, 20, 20}, 2000},
    {"~LIGHT3B", {160, 195, 232}, 2000},
    {"~LIGHT3C", {220, 210, 150}, 2000},
    {"~LIGHT3D", {190, 255, 49}, 2000},
    {"~LIGHT3E", {90, 190, 140}, 2000},
    {"~LIGHT5A", {210, 246, 255}, 2000},
    {"~LIGHT5C", {149, 255, 199}, 2000},
    {"~LIGHT5D", {165, 255, 149}, 2000},
    {"~LIGHT5E", {232, 63, 65}, 2000},
    {"~LIGHT5F", {232, 225, 122}, 2000},
    {"LITEPANEL1", {191, 213, 212}, 2000},
    {"PANELLITE1", {191, 213, 212}, 2000},
    {"SKKYLITE", {191, 213, 212}, 2000},
    {"SKKYLITE2", {191, 213, 212}, 2000},
    {"SKKYLITEDAWN", {213, 178, 196}, 2000},
    {"+0~FIFTS_LGHT01", {232, 232, 232}, 2000},
    {"+0~FIFTS_LGHT06", {232, 232, 232}, 2000},
    {"+0~FIFTS_LGHT3", {232, 232, 232}, 2000},
    {"+0~FIFTS_LGHT4", {232, 232, 232}, 2000},
    {"+0~FIFTS_LGHT5", {232, 232, 232}, 2000},
    {"+0~FIFTIES_LGT2", {232, 232, 232}, 2000},
    {"+0~TNNL_LGT1", {232, 225, 189}, 2000},
    {"+0~TNNL_LGT2", {223, 232, 221}, 2000},
    {"+0~TNNL_LGT3", {232, 229, 179}, 2000},
    {"+0~TNNL_LGT4", {170, 90, 40}, 2000},
    {"+0LAB1_W6", {150, 160, 210}, 2000},
    {"+0LAB1_W6B", {150, 160, 210}, 2000},
    {"+0LAB1_W7", {150, 160, 210}, 2000},
    {"+0~GENERIC65", {255, 255, 255}, 2000},
    {"+0~GENERIC85", {110, 140, 235}, 2000},
    {"+0~GENERIC86", {255, 230, 125}, 2000},
    {"+0~GENERIC86B", {60, 220, 170}, 2000},
    {"+0~GENERIC86R", {128, 0, 0}, 2000},
    {"GEN_VEND1", {102, 149, 100}, 2000},
    {"GENERIC87A", {102, 149, 100}, 2000},
    {"GENERIC106", {102, 149, 100}, 2000},
    {"GENERIC88A", {255, 202, 203}, 2000},
    {"GENERIC105", {255, 202, 203}, 2000},
    {"GENERIC107", {255, 202, 203}, 2000},
    {"FLATBED_HLITE2", {214, 178, 94}, 2000},
    {"FLATBED_LITE1", {214, 178, 94}, 2000},
    {"FLATBED_TLITE2", {214, 0, 3}, 2000},
    {"TECH_LITE1", {231, 217, 21}, 2000},
    {"TECH_LITE2", {231, 126, 20}, 2000},
    {"TECH_LITE3", {59, 213, 25}, 2000},
    {"+0~WHITE", {255, 255, 255}, 2000},
    {"WHITE", {255, 255, 255}, 2000},
    {"RED", {255, 0, 0}, 2000},
    {"YELLOW", {255, 251, 188}, 2000},
    {"~SPOTBLUE", {61, 194, 255}, 2000},
    {"~SPOTGREEN", {61, 255, 126}, 2000},
    {"~SPOTRED", {255, 26, 29}, 2000},
    {"~SPOTYELLOW", {255, 248, 138}, 2000},
    {"CRYS_1A", {248, 193, 255}, 2000},
    {"CRYS_1B", {248, 193, 255}, 2000},
    {"CRYS_1C", {248, 193, 255}, 2000},
    {"CRYS_1TOP", {248, 193, 255}, 2000},
    {"CRYS_2A", {169, 255, 182}, 2000},
    {"CRYS_2B", {169, 255, 182}, 2000},
    {"CRYS_2TOP", {169, 255, 182}, 2000},
    {"CRYS_3A", {255, 152, 79}, 2000},
    {"CRYS_3B", {255, 152, 79}, 2000},
    {"CRYS_3TOP", {255, 152, 79}, 2000},
    {"CRYS_4A", {255, 209, 153}, 2000},
    {"CRYS_4B", {255, 209, 153}, 2000},
    {"+0~C2A4_LGT1", {235, 214, 52}, 2000},
    {"+0~DRKMTLLGT1", {235, 194, 98}, 2000},
    {"EMERGLIGHT", {255, 251, 129}, 2000},
    {"SUBWAY_LIGHTS", {255, 248, 202}, 2000},
    {"+0BUTTONLITE", {235, 22, 22}, 2000},
    {"+ABUTTONLITE", {24, 235, 24}, 2000}};
} // namespace GoldSrcAssetConverter

#endif
