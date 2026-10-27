#if !defined(UTIL_GOLDSRC_ASSET_CONVERTER_TINY_GLTF_H)
#    define UTIL_GOLDSRC_ASSET_CONVERTER_TINY_GLTF_H

#    ifdef _WIN32
#        pragma once
#    endif

//tinygltf (DiligentTools/ThirdParty/tinygltf) writes the glTF documents. DiligentTools.lib compiles its own copy, so
//this one lives in a namespace of its own: a module may link both the converter and the DiligentTools GLTF loader.
//Images are encoded by the converter (see PNGEncoder.h) and never loaded from files.
#    define tinygltf UtilGoldSrcAssetConverterTinyGLTF
#    define TINYGLTF_NO_STB_IMAGE
#    define TINYGLTF_NO_STB_IMAGE_WRITE
#    define TINYGLTF_NO_EXTERNAL_IMAGE
#    include "tiny_gltf.h"

#endif
