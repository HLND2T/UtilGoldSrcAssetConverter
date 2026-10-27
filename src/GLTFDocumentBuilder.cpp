#include "GLTFDocumentBuilder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace GoldSrcAssetConverter
{
//Buffer views start on this boundary, the size of the largest component type.
constexpr std::size_t s_BufferViewAlignment = 4;
constexpr std::size_t s_Vec2Components      = 2;
constexpr std::size_t s_Vec3Components      = 3;

CGLTFDocumentBuilder::CGLTFDocumentBuilder(bool bEmbedImages, const std::string& externalImageDirectory) :
    m_bEmbedImages(bEmbedImages), m_ExternalImageDirectory(externalImageDirectory)
{
}

int CGLTFDocumentBuilder::AddBufferView(const void* pData, std::size_t bytes, int target)
{
    while (m_Buffer.size() % s_BufferViewAlignment)
        m_Buffer.push_back(0);

    tinygltf::BufferView view;
    view.buffer     = 0;
    view.byteOffset = m_Buffer.size();
    view.byteLength = bytes;
    view.target     = target;

    const auto* pBytes = static_cast<const std::uint8_t*>(pData);
    m_Buffer.insert(m_Buffer.end(), pBytes, pBytes + bytes);

    m_Model.bufferViews.push_back(view);

    return static_cast<int>(m_Model.bufferViews.size()) - 1;
}

int CGLTFDocumentBuilder::AddVec3Accessor(const std::vector<GoldSrcRender::Vector3>& values, bool bBounds)
{
    static_assert(sizeof(GoldSrcRender::Vector3) == s_Vec3Components * sizeof(float), "Vector3 is three packed floats");

    tinygltf::Accessor accessor;
    accessor.bufferView    = AddBufferView(values.data(), values.size() * sizeof(GoldSrcRender::Vector3), TINYGLTF_TARGET_ARRAY_BUFFER);
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.count         = values.size();
    accessor.type          = TINYGLTF_TYPE_VEC3;

    if (bBounds && !values.empty())
    {
        auto mins = values.front();
        auto maxs = values.front();

        for (const auto& value : values)
        {
            mins = {std::min(mins.x, value.x), std::min(mins.y, value.y), std::min(mins.z, value.z)};
            maxs = {std::max(maxs.x, value.x), std::max(maxs.y, value.y), std::max(maxs.z, value.z)};
        }

        accessor.minValues = {mins.x, mins.y, mins.z};
        accessor.maxValues = {maxs.x, maxs.y, maxs.z};
    }

    m_Model.accessors.push_back(accessor);

    return static_cast<int>(m_Model.accessors.size()) - 1;
}

int CGLTFDocumentBuilder::AddVec2Accessor(const std::vector<GoldSrcRender::Vector2>& values)
{
    static_assert(sizeof(GoldSrcRender::Vector2) == s_Vec2Components * sizeof(float), "Vector2 is two packed floats");

    tinygltf::Accessor accessor;
    accessor.bufferView    = AddBufferView(values.data(), values.size() * sizeof(GoldSrcRender::Vector2), TINYGLTF_TARGET_ARRAY_BUFFER);
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.count         = values.size();
    accessor.type          = TINYGLTF_TYPE_VEC2;

    m_Model.accessors.push_back(accessor);

    return static_cast<int>(m_Model.accessors.size()) - 1;
}

int CGLTFDocumentBuilder::AddIndexAccessor(const std::vector<std::uint32_t>& indices, std::uint32_t vertexCount)
{
    tinygltf::Accessor accessor;
    accessor.count = indices.size();
    accessor.type  = TINYGLTF_TYPE_SCALAR;

    if (vertexCount <= std::numeric_limits<std::uint16_t>::max())
    {
        std::vector<std::uint16_t> shortIndices(indices.begin(), indices.end());

        accessor.bufferView    = AddBufferView(shortIndices.data(), shortIndices.size() * sizeof(std::uint16_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    }
    else
    {
        accessor.bufferView    = AddBufferView(indices.data(), indices.size() * sizeof(std::uint32_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    }

    m_Model.accessors.push_back(accessor);

    return static_cast<int>(m_Model.accessors.size()) - 1;
}

//File name characters of external images: letters, digits, '-' and '_'; others become '_', so the name is also a
//valid relative URI.
static std::string MakeFileName(const std::string& name)
{
    std::string fileName;

    for (const auto c : name)
        fileName.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_');

    return fileName;
}

int CGLTFDocumentBuilder::AddPNGImage(const std::string& name, std::vector<std::uint8_t>&& png)
{
    tinygltf::Image image;
    image.name = name;

    if (m_bEmbedImages)
    {
        image.bufferView = AddBufferView(png.data(), png.size(), 0);
        image.mimeType   = "image/png";
    }
    else
    {
        OutputFile file;
        file.relativePath = m_ExternalImageDirectory + std::to_string(m_Model.images.size()) + "_" + MakeFileName(name) + ".png";
        file.bytes        = std::move(png);

        image.uri = file.relativePath;
        m_ExternalFiles.push_back(std::move(file));
    }

    m_Model.images.push_back(std::move(image));

    return static_cast<int>(m_Model.images.size()) - 1;
}

int CGLTFDocumentBuilder::AddSampler(int magFilter, int minFilter, int wrapS, int wrapT)
{
    tinygltf::Sampler sampler;
    sampler.magFilter = magFilter;
    sampler.minFilter = minFilter;
    sampler.wrapS     = wrapS;
    sampler.wrapT     = wrapT;

    m_Model.samplers.push_back(sampler);

    return static_cast<int>(m_Model.samplers.size()) - 1;
}

int CGLTFDocumentBuilder::AddTexture(int image, int sampler, const std::string& name)
{
    tinygltf::Texture texture;
    texture.name    = name;
    texture.source  = image;
    texture.sampler = sampler;

    m_Model.textures.push_back(texture);

    return static_cast<int>(m_Model.textures.size()) - 1;
}

tinygltf::Value ToGLTFValue(const nlohmann::json& json)
{
    switch (json.type())
    {
        case nlohmann::json::value_t::boolean:
            return tinygltf::Value(json.get<bool>());

        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        {
            const auto value = json.get<double>();

            if (value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max())
                return tinygltf::Value(static_cast<int>(value));

            return tinygltf::Value(value);
        }

        case nlohmann::json::value_t::number_float:
            return tinygltf::Value(json.get<double>());

        case nlohmann::json::value_t::string:
            return tinygltf::Value(json.get<std::string>());

        case nlohmann::json::value_t::array:
        {
            tinygltf::Value::Array array;
            array.reserve(json.size());

            for (const auto& element : json)
                array.push_back(ToGLTFValue(element));

            return tinygltf::Value(std::move(array));
        }

        case nlohmann::json::value_t::object:
        {
            tinygltf::Value::Object object;

            for (auto it = json.begin(); it != json.end(); ++it)
                object.emplace(it.key(), ToGLTFValue(it.value()));

            return tinygltf::Value(std::move(object));
        }

        default:
            return tinygltf::Value();
    }
}
} // namespace GoldSrcAssetConverter
