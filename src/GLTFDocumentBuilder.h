#if !defined(GLTF_DOCUMENT_BUILDER_H)
#    define GLTF_DOCUMENT_BUILDER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "IUtilGoldSrcAssetConverter.h"
#    include "TinyGLTF.h"

#    include <nlohmann/json.hpp>

#    include <cstdint>
#    include <string>
#    include <vector>

//Builds a glTF 2.0 document with a single binary buffer and writes it as .glb or .gltf through tinygltf. Every buffer
//view starts on a 4-byte boundary, which every accessor component type of the document needs.
namespace GoldSrcAssetConverter
{
//A file written next to the asset, such as an external image.
struct OutputFile
{
    std::string               relativePath;
    std::vector<std::uint8_t> bytes;
};

class CGLTFDocumentBuilder
{
public:
    //Images are stored in the buffer when bEmbedImages, otherwise as external files whose paths start with
    //externalImageDirectory.
    CGLTFDocumentBuilder(bool bEmbedImages, const std::string& externalImageDirectory);

    tinygltf::Model& GetModel()
    {
        return m_Model;
    }

    //Appends bytes to the buffer as a new buffer view; target is TINYGLTF_TARGET_* or 0.
    int AddBufferView(const void* pData, std::size_t bytes, int target);

    //Accessors over new buffer views. Positions get the min and max the specification requires.
    int AddVec3Accessor(const std::vector<GoldSrcRender::Vector3>& values, bool bBounds);
    int AddVec2Accessor(const std::vector<GoldSrcRender::Vector2>& values);

    //Indices as unsigned short when every index fits, otherwise unsigned int.
    int AddIndexAccessor(const std::vector<std::uint32_t>& indices, std::uint32_t vertexCount);

    //A PNG image, embedded or external.
    int AddPNGImage(const std::string& name, std::vector<std::uint8_t>&& png);

    int AddSampler(int magFilter, int minFilter, int wrapS, int wrapT);
    int AddTexture(int image, int sampler, const std::string& name);

    std::uint64_t GetBufferBytes() const
    {
        return m_Buffer.size();
    }

    std::vector<OutputFile>& GetExternalFiles()
    {
        return m_ExternalFiles;
    }

    //Moves the buffer into the document and serializes it. The builder cannot be used afterwards.
    bool Write(OutputFormat format, std::vector<std::uint8_t>& file, std::string& error);

private:
    tinygltf::Model           m_Model;
    std::vector<std::uint8_t> m_Buffer;
    std::vector<OutputFile>   m_ExternalFiles;
    bool                      m_bEmbedImages{};
    std::string               m_ExternalImageDirectory;
};

//The same JSON value as tinygltf::Value, for extras. Numbers that fit an int stay integers.
tinygltf::Value ToGLTFValue(const nlohmann::json& json);

//Checks the container of a .glb file: header, chunk types, lengths and alignment. False with a reason otherwise.
bool ValidateGLBContainer(const std::vector<std::uint8_t>& file, std::string& error);
} // namespace GoldSrcAssetConverter

#endif
