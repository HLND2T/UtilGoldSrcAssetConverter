//Loads the converter's test outputs with the DiligentTools GLTF loader (the prebuilt DiligentEngine of the main build),
//without a render device: the documents must be readable by GLTF::Model as DiligentFX's GLTF_PBR_Renderer and the
//RTXPT scene graph consume them. The converter tests write the files first (CTest fixture).
#include "DiligentTools/AssetLoader/interface/GLTFLoader.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace Diligent;

static void Require(bool bCondition, const char* pMessage)
{
    if (!bCondition)
    {
        std::cerr << "FAILED: " << pMessage << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr float s_Tolerance = 1e-3f;

//What the converter writes for the test BSP (see BSPConverterTestFile.h and BSPConverterTests.cpp): 5 models, 9
//meshes (model 0: surface, sky, water; model 1: surface; model 2: surface, sky; model 3: surface, water; model 4:
//surface) and 23 nodes (root, world and its 3 mesh nodes, 2 groups, 9 entity nodes, 7 brush mesh nodes and a light). The loader
//only builds the meshes the scene's nodes show, so model 4's (no entity shows it) is left out.
constexpr size_t s_TestSceneMeshCount = 8;
constexpr size_t s_TestNodeCount      = 23;

static const GLTF::Node* FindNode(const GLTF::Model& model, const char* szName)
{
    for (const auto& node : model.Nodes)
    {
        if (node.Name == szName)
            return &node;
    }

    return nullptr;
}

static void TestDocument(const std::filesystem::path& path)
{
    const auto fileName = path.string();

    GLTF::ModelCreateInfo createInfo;
    createInfo.FileName = fileName.c_str();

    //A null device and context load the scene graph, meshes and materials without GPU resources.
    GLTF::Model model{nullptr, nullptr, createInfo};

    Require(model.Meshes.size() == s_TestSceneMeshCount && model.Nodes.size() == s_TestNodeCount, "meshes and nodes are loaded");
    Require(model.Scenes.size() == 1 && model.Scenes[0].RootNodes.size() == 1 && model.Scenes[0].RootNodes[0]->Name == "BSP_Root", "one scene rooted at BSP_Root");
    //Without a device no texture objects are created: the materials' texture references are checked instead.
    const auto baseColorAttribute = model.GetTextureAttributeIndex(GLTF::BaseColorTextureName);
    Require(!model.Materials.empty() && baseColorAttribute >= 0, "materials are loaded");

    size_t texturedMaterialCount = 0;

    for (const auto& material : model.Materials)
    {
        if (material.GetTextureId(static_cast<Uint32>(baseColorAttribute)) >= 0)
            ++texturedMaterialCount;
    }

    Require(texturedMaterialCount > 0, "materials reference their base color textures");

    for (const auto& mesh : model.Meshes)
    {
        Require(!mesh.Primitives.empty(), "every mesh has primitives");

        for (const auto& primitive : mesh.Primitives)
        {
            Require(primitive.IndexCount > 0 && primitive.IndexCount % 3 == 0 && primitive.VertexCount > 0 && primitive.MaterialId < model.Materials.size(), "primitives are triangles with a material");

            const auto& material = model.Materials[primitive.MaterialId];
            const auto  workflow = mesh.Name.find("_Sky") != std::string::npos ? GLTF::Material::PBR_WORKFLOW_UNLIT : GLTF::Material::PBR_WORKFLOW_METALL_ROUGH;
            Require(material.Attribs.Workflow == workflow, "ordinary surfaces load as metallic-roughness PBR; sky placeholders remain unlit");
        }
    }

    //The wall entity: origin (100, 200, 300) under the root's -90 degree rotation about x.
    const auto* pWall = FindNode(model, "ent_0000_func_wall");
    Require(pWall && pWall->Children.size() == 1 && pWall->Children[0]->pMesh, "the brush entity node shows its mesh through a child");
    Require(std::fabs(pWall->Translation.x - 100) < s_Tolerance && std::fabs(pWall->Translation.y - 200) < s_Tolerance && std::fabs(pWall->Translation.z - 300) < s_Tolerance, "entity translation");

    const auto* pShared = FindNode(model, "ent_0004_func_wall");
    Require(pShared && pShared->Children[0]->pMesh == pWall->Children[0]->pMesh, "entities of one model share its mesh");

    GLTF::ModelTransforms transforms;
    model.ComputeTransforms(0, transforms);

    //GoldSrc (100, 200, 300) is glTF (100, 300, -200).
    const auto& global = transforms.NodeGlobalMatrices[pWall->Index];
    Require(std::fabs(global._41 - 100) < s_Tolerance && std::fabs(global._42 - 300) < s_Tolerance && std::fabs(global._43 + 200) < s_Tolerance, "the root turns GoldSrc coordinates into glTF's");

    Require(FindNode(model, "ent_0005_info_player_start") && !FindNode(model, "ent_0005_info_player_start")->pMesh, "point entities load as nodes without mesh");

    const auto* pLightEntity = FindNode(model, "ent_0006_light");
    const auto* pLightNode   = FindNode(model, "ent_0006_light_PointLight");
    Require(model.Lights.size() == 1 && pLightEntity && !pLightEntity->pLight && pLightEntity->Children.size() == 1 && pLightEntity->Children[0] == pLightNode,
            "the original light entity remains empty with one light child");
    Require(pLightNode && pLightNode->pLight == &model.Lights[0] && pLightNode->pLight->Type == GLTF::Light::TYPE::POINT, "the light child loads its punctual point light");
    constexpr float s_TestLightIntensity = 200;
    const auto&     light                = *pLightNode->pLight;
    Require(std::fabs(s_TestLightIntensity - light.Intensity) < s_Tolerance && std::fabs(1 - light.Color.x) < s_Tolerance && std::fabs(1 - light.Color.y) < s_Tolerance && std::fabs(1 - light.Color.z) < s_Tolerance,
            "the loader preserves point light color and intensity");
    const auto&     lightGlobal       = transforms.NodeGlobalMatrices[pLightNode->Index];
    constexpr float s_TestLightHeight = 64;
    Require(std::fabs(lightGlobal._41) < s_Tolerance && std::fabs(s_TestLightHeight - lightGlobal._42) < s_Tolerance && std::fabs(lightGlobal._43) < s_Tolerance, "the light inherits the converted entity position");
}

static void TestColoredPointLight(const std::filesystem::path& path)
{
    const auto            fileName = path.string();
    GLTF::ModelCreateInfo createInfo;
    createInfo.FileName = fileName.c_str();
    GLTF::Model model{nullptr, nullptr, createInfo};
    const auto* pNode = FindNode(model, "ent_0010_light_PointLight");
    Require(model.Lights.size() == 2 && pNode && pNode->pLight == &model.Lights[1], "multiple light children load their independent definitions");
    constexpr float s_GreenLightIntensity = 100;
    const auto&     light                 = *pNode->pLight;
    Require(light.Type == GLTF::Light::TYPE::POINT && std::fabs(s_GreenLightIntensity - light.Intensity) < s_Tolerance &&
                std::fabs(light.Color.x) < s_Tolerance && std::fabs(1 - light.Color.y) < s_Tolerance && std::fabs(light.Color.z) < s_Tolerance,
            "the loader preserves a colored point light");
}

static void TestTextureLight(const std::filesystem::path& path)
{
    const auto            fileName = path.string();
    GLTF::ModelCreateInfo createInfo;
    createInfo.FileName = fileName.c_str();
    GLTF::Model model{nullptr, nullptr, createInfo};
    const auto  emissiveAttribute = model.GetTextureAttributeIndex(GLTF::EmissiveTextureName);
    Require(emissiveAttribute >= 0, "the loader has an emissive texture attribute");

    //The test floor uses SDK +0~LIGHT2A: RGB (255,251,129), intensity 2000.
    constexpr float s_RADScale             = 255;
    constexpr float s_RADIntensity         = 2000;
    constexpr float s_RADGreen             = 251;
    constexpr float s_RADBlue              = 129;
    constexpr float s_DefaultEmissiveScale = 4;
    bool            bFound{};

    for (const auto& material : model.Materials)
    {
        if (material.GetTextureId(static_cast<Uint32>(emissiveAttribute)) < 0)
            continue;

        const auto& factor = material.Attribs.EmissiveFactor;
        Require(material.Attribs.Workflow == GLTF::Material::PBR_WORKFLOW_METALL_ROUGH, "emitting surfaces remain PBR");
        Require(std::fabs(s_DefaultEmissiveScale * s_RADIntensity / s_RADScale - factor.x) < s_Tolerance &&
                    std::fabs(s_DefaultEmissiveScale * s_RADGreen * s_RADIntensity / (s_RADScale * s_RADScale) - factor.y) < s_Tolerance &&
                    std::fabs(s_DefaultEmissiveScale * s_RADBlue * s_RADIntensity / (s_RADScale * s_RADScale) - factor.z) < s_Tolerance,
                "the loader preserves HDR emission color and strength");
        bFound = true;
    }

    Require(bFound, "the loader finds a texture light");
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: UtilGoldSrcAssetConverterDiligentLoaderTests <converter test output directory>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path directory = argv[1];

    for (const auto* szFile : {"converter_default.glb", "converter_blueshift.glb", "converter_visibility.glb", "converter_collision.glb", "converter_embedded.gltf", "external/converter_external.glb"})
        TestDocument(directory / szFile);

    TestDocument(directory / "converter_emissive.glb");
    TestTextureLight(directory / "converter_emissive.glb");
    TestColoredPointLight(directory / "converter_point_lights.glb");

    std::cout << "DiligentTools GLTF loader tests passed\n";

    return EXIT_SUCCESS;
}
