cmake_minimum_required(VERSION 3.20)

# UtilGoldSrcAssetConverter.dll: GoldSrc BSP -> glTF 2.0 / GLB, CPU only. It reuses the BSP reading of GoldSrcRenderCore
# and writes documents with tinygltf. Callers load the DLL and resolve UTIL_GOLDSRC_ASSET_CONVERTER_INTERFACE_VERSION
# through CreateInterface; nothing links its import library.
# Also usable as a standalone project that builds the converter, its tests and the command line tool.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Formatting check only: needs Python and clang-format, no compiler.
  option(FORMAT_VALIDATION_ONLY "Configure only the clang-format targets" OFF)
  if(FORMAT_VALIDATION_ONLY)
    project(UtilGoldSrcAssetConverterFormatValidation LANGUAGES NONE)
    include(cmake/FormatValidation.cmake)
    return()
  endif()
  project(UtilGoldSrcAssetConverter LANGUAGES CXX)
  option(UTILGOLDSRCASSETCONVERTER_BUILD_TESTS "Build the converter tests" ON)
else()
  option(UTILGOLDSRCASSETCONVERTER_BUILD_TESTS "Build the converter tests" OFF)
endif()

option(UTILGOLDSRCASSETCONVERTER_BUILD_CLI "Build the GoldSrcAssetConverterCLI command line tool" ON)

option(UTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS "Run the Khronos glTF-Validator (node + npm package gltf-validator) on the test outputs" OFF)

# Dependencies: the submodules and files under thirdparty/, which a parent project may replace with its own locations.
# GoldSrcRenderCore comes from the parent project's target when it has one, then from GSRENDERCORE_SOURCE_PATH,
# and is otherwise fetched at UTILGOLDSRCASSETCONVERTER_CORE_COMMIT.
set(GSRENDERCORE_SOURCE_PATH "$ENV{GSRENDERCORE_SOURCE_PATH}" CACHE PATH
  "GoldSrcRenderCore source tree; empty fetches the pinned commit")
set(UTILGOLDSRCASSETCONVERTER_CORE_REPOSITORY https://github.com/HLND2T/GoldSrcRenderCore.git)
set(UTILGOLDSRCASSETCONVERTER_CORE_COMMIT 7f97da178df63daef4d931f0e05278b504b42322)
set(UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ValveSDK" CACHE PATH
  "Directory of interface.h and interface.cpp, and of FileSystem.h for the command line tool")
set(UTILGOLDSRCASSETCONVERTER_TINYGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tinygltf" CACHE PATH
  "Directory of tiny_gltf.h")
set(UTILGOLDSRCASSETCONVERTER_STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb" CACHE PATH
  "Directory of stb_image.h and stb_image_write.h")

# tinygltf includes nlohmann's "json.hpp" by file name.
if(NOT NLOHMANN_JSON_INCLUDE_DIRS)
  set(NLOHMANN_JSON_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nlohmann_json/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nlohmann_json/include/nlohmann")
endif()

# A missing file usually means an uninitialized submodule: git submodule update --init
function(utilgoldsrcassetconverter_require_files variable)
  foreach(required IN LISTS ARGN)
    if(NOT EXISTS "${${variable}}/${required}")
      message(FATAL_ERROR "${variable} has no ${required}: '${${variable}}' (run git submodule update --init, or pass the directory)")
    endif()
  endforeach()
endfunction()
utilgoldsrcassetconverter_require_files(UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR interface.h interface.cpp FileSystem.h)
utilgoldsrcassetconverter_require_files(UTILGOLDSRCASSETCONVERTER_TINYGLTF_DIR tiny_gltf.h)
utilgoldsrcassetconverter_require_files(UTILGOLDSRCASSETCONVERTER_STB_DIR stb_image.h stb_image_write.h)
set(json_found FALSE)
foreach(json_dir IN LISTS NLOHMANN_JSON_INCLUDE_DIRS)
  if(EXISTS "${json_dir}/json.hpp")
    set(json_found TRUE)
  endif()
endforeach()
if(NOT json_found)
  message(FATAL_ERROR "NLOHMANN_JSON_INCLUDE_DIRS has no json.hpp: '${NLOHMANN_JSON_INCLUDE_DIRS}' (run git submodule update --init, or pass the directories)")
endif()

if(NOT TARGET GoldSrcRenderCore)
  if(GSRENDERCORE_SOURCE_PATH)
    if(NOT EXISTS "${GSRENDERCORE_SOURCE_PATH}/CMakeLists.txt")
      message(FATAL_ERROR "GSRENDERCORE_SOURCE_PATH is not a GoldSrcRenderCore source tree: '${GSRENDERCORE_SOURCE_PATH}' (leave it empty to fetch the pinned commit)")
    endif()
    add_subdirectory("${GSRENDERCORE_SOURCE_PATH}" GoldSrcRenderCore)
  else()
    include(FetchContent)
    FetchContent_Declare(GoldSrcRenderCore
      GIT_REPOSITORY "${UTILGOLDSRCASSETCONVERTER_CORE_REPOSITORY}"
      GIT_TAG "${UTILGOLDSRCASSETCONVERTER_CORE_COMMIT}"
      GIT_SUBMODULES "")
    FetchContent_MakeAvailable(GoldSrcRenderCore)
  endif()
  if(NOT TARGET GoldSrcRenderCore)
    message(FATAL_ERROR "GoldSrcRenderCore did not define the GoldSrcRenderCore target")
  endif()
endif()

# The published interface: include/Interface, the GoldSrcRenderCore asset types it uses and interface.h.
add_library(UtilGoldSrcAssetConverterInterface INTERFACE)
target_include_directories(UtilGoldSrcAssetConverterInterface INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include/Interface" "${UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR}")
target_link_libraries(UtilGoldSrcAssetConverterInterface INTERFACE GoldSrcRenderCore)

# The converter, linked into the DLL and the converter tests.
add_library(UtilGoldSrcAssetConverterCore STATIC
  src/BSPConverter.cpp
  src/BSPAreaLightConverter.cpp
  src/BSPEntityLumpParser.cpp
  src/BSPFileReader.cpp
  src/BSPGeometryConverter.cpp
  src/BSPLightmapConverter.cpp
  src/BSPMetadataWriter.cpp
  src/BSPSkyGeometryExtractor.cpp
  src/BSPSpatialDataWriter.cpp
  src/BSPTextureConverter.cpp
  src/CoordinateConverter.cpp
  src/GLTFBinaryWriter.cpp
  src/GLTFDocumentBuilder.cpp
  src/PNGEncoder.cpp
  src/SHA256.cpp
  src/TinyGLTF.cpp)
target_compile_features(UtilGoldSrcAssetConverterCore PUBLIC cxx_std_17)
target_include_directories(UtilGoldSrcAssetConverterCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PRIVATE ${NLOHMANN_JSON_INCLUDE_DIRS} "${UTILGOLDSRCASSETCONVERTER_TINYGLTF_DIR}" "${UTILGOLDSRCASSETCONVERTER_STB_DIR}")
target_link_libraries(UtilGoldSrcAssetConverterCore PUBLIC UtilGoldSrcAssetConverterInterface GoldSrcRenderCore)
set_target_properties(UtilGoldSrcAssetConverterCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(UtilGoldSrcAssetConverterCore PRIVATE /bigobj /utf-8)
endif()

add_library(UtilGoldSrcAssetConverter SHARED
  src/UtilGoldSrcAssetConverter.cpp
  "${UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR}/interface.cpp")
target_link_libraries(UtilGoldSrcAssetConverter PRIVATE UtilGoldSrcAssetConverterCore)
set_target_properties(UtilGoldSrcAssetConverter PROPERTIES PREFIX "" POSITION_INDEPENDENT_CODE ON)
install(TARGETS UtilGoldSrcAssetConverter RUNTIME DESTINATION . LIBRARY DESTINATION .)
if(MSVC)
  install(FILES "$<TARGET_PDB_FILE:UtilGoldSrcAssetConverter>" DESTINATION . OPTIONAL)
endif()

# Command line tool: converts a .bsp, resolving textures from the BSP and from WAD files, given or found through the
# game's search paths. It loads the DLL, and filesystem_stdio.dll for -basedir, from its own directory, so it is
# installed next to them.
if(UTILGOLDSRCASSETCONVERTER_BUILD_CLI)
  add_executable(GoldSrcAssetConverterCLI
    tools/GoldSrcAssetConverterCLI.cpp
    "${UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR}/interface.cpp")
  target_compile_features(GoldSrcAssetConverterCLI PRIVATE cxx_std_17)
  target_link_libraries(GoldSrcAssetConverterCLI PRIVATE UtilGoldSrcAssetConverterInterface)
  if(NOT WIN32)
    target_link_libraries(GoldSrcAssetConverterCLI PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_dependencies(GoldSrcAssetConverterCLI UtilGoldSrcAssetConverter)
  if(TARGET filesystem_stdio)
    add_dependencies(GoldSrcAssetConverterCLI filesystem_stdio)
  endif()
  install(TARGETS GoldSrcAssetConverterCLI RUNTIME DESTINATION .)
  if(MSVC)
    install(FILES "$<TARGET_PDB_FILE:GoldSrcAssetConverterCLI>" DESTINATION . OPTIONAL)
  endif()
endif()

if(UTILGOLDSRCASSETCONVERTER_BUILD_TESTS)
  enable_testing()

  add_executable(UtilGoldSrcAssetConverterTests tests/BSPConverterTests.cpp)
  target_link_libraries(UtilGoldSrcAssetConverterTests PRIVATE UtilGoldSrcAssetConverterCore GoldSrcRenderCoreTestFiles)
  target_include_directories(UtilGoldSrcAssetConverterTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" ${NLOHMANN_JSON_INCLUDE_DIRS} "${UTILGOLDSRCASSETCONVERTER_TINYGLTF_DIR}" "${UTILGOLDSRCASSETCONVERTER_STB_DIR}")
  if(MSVC)
    target_compile_options(UtilGoldSrcAssetConverterTests PRIVATE /bigobj /utf-8)
  endif()

  set(UTILGOLDSRCASSETCONVERTER_TEST_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test-output")
  add_test(NAME UtilGoldSrcAssetConverter.BSP COMMAND UtilGoldSrcAssetConverterTests "${UTILGOLDSRCASSETCONVERTER_TEST_OUTPUT}")
  set_tests_properties(UtilGoldSrcAssetConverter.BSP PROPERTIES FIXTURES_SETUP UtilGoldSrcAssetConverterOutputs)

  # The DLL through its published interface only, loaded from the path given.
  add_executable(UtilGoldSrcAssetConverterDllTests
    tests/DllTests.cpp
    "${UTILGOLDSRCASSETCONVERTER_INTERFACE_DIR}/interface.cpp")
  target_compile_features(UtilGoldSrcAssetConverterDllTests PRIVATE cxx_std_17)
  # The core only builds the test BSP; the converter is reached through the interface alone.
  target_link_libraries(UtilGoldSrcAssetConverterDllTests PRIVATE UtilGoldSrcAssetConverterInterface GoldSrcRenderCoreTestFiles)
  if(NOT WIN32)
    target_link_libraries(UtilGoldSrcAssetConverterDllTests PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_dependencies(UtilGoldSrcAssetConverterDllTests UtilGoldSrcAssetConverter)
  add_test(NAME UtilGoldSrcAssetConverter.DLL COMMAND UtilGoldSrcAssetConverterDllTests "$<TARGET_FILE:UtilGoldSrcAssetConverter>")

  if(UTILGOLDSRCASSETCONVERTER_VALIDATOR_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME UtilGoldSrcAssetConverter.GLTFValidator
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_gltf_validator.py"
        --cache "${CMAKE_CURRENT_BINARY_DIR}/gltf-validator" "${UTILGOLDSRCASSETCONVERTER_TEST_OUTPUT}")
    set_tests_properties(UtilGoldSrcAssetConverter.GLTFValidator PROPERTIES FIXTURES_REQUIRED UtilGoldSrcAssetConverterOutputs)
  endif()

  # DiligentTools GLTF loader compatibility: needs the prebuilt DiligentEngine of the main build (DILIGENT_* cache
  # variables of the HLND2T-DiligentGraphics build scripts).
  if(DILIGENT_LIBRARIES AND DILIGENT_INCLUDE_DIRS)
    add_executable(UtilGoldSrcAssetConverterDiligentLoaderTests tests/DiligentLoaderTests.cpp)
    target_compile_features(UtilGoldSrcAssetConverterDiligentLoaderTests PRIVATE cxx_std_20)
    target_include_directories(UtilGoldSrcAssetConverterDiligentLoaderTests PRIVATE ${DILIGENT_INCLUDE_DIRS})
    target_link_directories(UtilGoldSrcAssetConverterDiligentLoaderTests PRIVATE ${DILIGENT_LIBRARY_DIRS})
    target_link_libraries(UtilGoldSrcAssetConverterDiligentLoaderTests PRIVATE ${DILIGENT_LIBRARIES})
    if(WIN32)
      target_compile_definitions(UtilGoldSrcAssetConverterDiligentLoaderTests PRIVATE PLATFORM_WIN32=1)
    endif()
    add_test(NAME UtilGoldSrcAssetConverter.DiligentLoader COMMAND UtilGoldSrcAssetConverterDiligentLoaderTests "${UTILGOLDSRCASSETCONVERTER_TEST_OUTPUT}")
    set_tests_properties(UtilGoldSrcAssetConverter.DiligentLoader PROPERTIES FIXTURES_REQUIRED UtilGoldSrcAssetConverterOutputs)
  endif()
endif()
