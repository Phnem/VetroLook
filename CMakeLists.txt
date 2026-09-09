cmake_minimum_required(VERSION 3.25)
project(VetroLook VERSION 1.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
get_filename_component(PARENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${PARENT_ROOT}/libjpeg-turbo")
 set(ROOT "${PARENT_ROOT}")
else()
 set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
include(ExternalProject)

# ---------------------------------------------------------------- libjpeg ---
# SIMD is not a nicety here: it is most of the JPEG decode budget, and JPEG is
# the format the viewer opens most. It needs NASM, so the build looks for one
# rather than silently producing a scalar build nobody notices until they
# benchmark it. VETRO_JPEG_SIMD reports what actually happened.
find_program(VETRO_NASM NAMES nasm
 HINTS "${ROOT}/tools/nasm/nasm-2.16.03" "${ROOT}/tools/nasm" ENV NASM_PATH
 PATHS "$ENV{ProgramFiles}/NASM" "$ENV{LOCALAPPDATA}/bin/NASM")
if(VETRO_NASM)
 set(VETRO_JPEG_SIMD ON)
 message(STATUS "libjpeg-turbo SIMD: ON (nasm at ${VETRO_NASM})")
else()
 set(VETRO_JPEG_SIMD OFF)
 message(WARNING "libjpeg-turbo SIMD: OFF - no nasm found. JPEG decoding will be "
  "roughly two to three times slower. Run build.ps1, which fetches NASM, or set "
  "-DVETRO_NASM=<path to nasm.exe>.")
endif()
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(WITH_TURBOJPEG ON CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_SIMD ${VETRO_JPEG_SIMD} CACHE BOOL "" FORCE)
ExternalProject_Add(jpeg_build SOURCE_DIR "${ROOT}/libjpeg-turbo" BINARY_DIR "${CMAKE_BINARY_DIR}/jpeg"
 CMAKE_ARGS -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
            -DWITH_SIMD=${VETRO_JPEG_SIMD} -DCMAKE_ASM_NASM_COMPILER=${VETRO_NASM} -DWITH_CRT_DLL=OFF
 INSTALL_COMMAND "" BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/jpeg/Release/turbojpeg-static.lib")
add_library(turbojpeg-static STATIC IMPORTED)
set_target_properties(turbojpeg-static PROPERTIES IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/jpeg/Release/turbojpeg-static.lib")
add_dependencies(turbojpeg-static jpeg_build)
foreach(opt ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
 set(WEBP_BUILD_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${ROOT}/libwebp" webp EXCLUDE_FROM_ALL)
file(GLOB_RECURSE RAW_SOURCES CONFIGURE_DEPENDS "${ROOT}/LibRaw/src/*.cpp")
list(FILTER RAW_SOURCES EXCLUDE REGEX "_ph\\.cpp$")
add_library(vetro_raw STATIC ${RAW_SOURCES})
target_include_directories(vetro_raw PUBLIC "${ROOT}/LibRaw")
target_compile_definitions(vetro_raw PRIVATE LIBRAW_NODLL NOMINMAX _CRT_SECURE_NO_WARNINGS)
# ------------------------------------------------------------ metadata ---
# Exiv2 reads EXIF, IPTC, XMP and MakerNotes from one place, and vendors the
# Adobe XMP Toolkit (xmpsdk/) for the XMP half. That toolkit needs expat, so
# expat is built first. Both are static; see LICENSE_AUDIT.md for the licence
# reasoning and THIRD_PARTY_NOTICES.txt for the required notices.
set(VETRO_EXPAT_LIB "${CMAKE_BINARY_DIR}/expat/Release/libexpatMT.lib")
ExternalProject_Add(expat_build SOURCE_DIR "${ROOT}/libexpat/expat" BINARY_DIR "${CMAKE_BINARY_DIR}/expat"
 CMAKE_ARGS -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF
            -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_PKGCONFIG=OFF
            -DEXPAT_MSVC_STATIC_CRT=ON
 INSTALL_COMMAND "" BUILD_BYPRODUCTS "${VETRO_EXPAT_LIB}")
set(VETRO_EXIV2_LIB "${CMAKE_BINARY_DIR}/exiv2/lib/Release/exiv2.lib")
set(VETRO_EXIV2_XMP_LIB "${CMAKE_BINARY_DIR}/exiv2/xmpsdk/exiv2-xmp.dir/Release/exiv2-xmp.lib")
ExternalProject_Add(exiv2_build SOURCE_DIR "${ROOT}/exiv2" BINARY_DIR "${CMAKE_BINARY_DIR}/exiv2"
 DEPENDS expat_build
 CMAKE_ARGS -DBUILD_SHARED_LIBS=OFF -DEXIV2_ENABLE_DYNAMIC_RUNTIME=OFF -DEXIV2_ENABLE_XMP=ON
            -DEXIV2_ENABLE_PNG=OFF -DEXIV2_ENABLE_BMFF=OFF -DEXIV2_ENABLE_BROTLI=OFF
            -DEXIV2_ENABLE_VIDEO=OFF -DEXIV2_ENABLE_INIH=OFF -DEXIV2_ENABLE_NLS=OFF
            -DEXIV2_BUILD_SAMPLES=OFF -DEXIV2_BUILD_EXIV2_COMMAND=OFF -DEXIV2_BUILD_UNIT_TESTS=OFF
            -DEXIV2_ENABLE_WEBREADY=OFF -DEXIV2_ENABLE_CURL=OFF
            -DEXPAT_LIBRARY=${VETRO_EXPAT_LIB} -DEXPAT_INCLUDE_DIR=${ROOT}/libexpat/expat/lib
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
            # expat is static here, so its headers must not declare dllimport.
            "-DCMAKE_C_FLAGS=-DXML_STATIC" "-DCMAKE_CXX_FLAGS=-DXML_STATIC"
 INSTALL_COMMAND "" BUILD_BYPRODUCTS "${VETRO_EXIV2_LIB}" "${VETRO_EXIV2_XMP_LIB}")
add_library(vetro_expat STATIC IMPORTED)
set_target_properties(vetro_expat PROPERTIES IMPORTED_LOCATION "${VETRO_EXPAT_LIB}")
add_dependencies(vetro_expat expat_build)
add_library(vetro_exiv2_xmp STATIC IMPORTED)
set_target_properties(vetro_exiv2_xmp PROPERTIES IMPORTED_LOCATION "${VETRO_EXIV2_XMP_LIB}")
add_dependencies(vetro_exiv2_xmp exiv2_build)
add_library(vetro_exiv2 INTERFACE)
add_library(vetro_exiv2_core STATIC IMPORTED)
set_target_properties(vetro_exiv2_core PROPERTIES IMPORTED_LOCATION "${VETRO_EXIV2_LIB}")
add_dependencies(vetro_exiv2_core exiv2_build)
target_link_libraries(vetro_exiv2 INTERFACE vetro_exiv2_core vetro_exiv2_xmp vetro_expat)
target_include_directories(vetro_exiv2 INTERFACE "${ROOT}/exiv2/include" "${CMAKE_BINARY_DIR}/exiv2" "${ROOT}/libexpat/expat/lib")
target_compile_definitions(vetro_exiv2 INTERFACE exiv2lib_STATIC XML_STATIC)

add_library(vetro_exr STATIC "${ROOT}/tinyexr/tinyexr.cc" "${ROOT}/tinyexr/deps/miniz/miniz.c")
target_include_directories(vetro_exr PUBLIC "${ROOT}/tinyexr" PRIVATE "${ROOT}/tinyexr/deps/miniz")
target_compile_definitions(vetro_exr PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_library(dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build-dav1d/src/libdav1d.a" INTERFACE_INCLUDE_DIRECTORIES "${ROOT}/dav1d/include")
set(AVIF_CODEC_DAV1D SYSTEM CACHE STRING "" FORCE)
set(AVIF_LIBYUV OFF CACHE STRING "" FORCE)
add_subdirectory("${ROOT}/libavif" avif EXCLUDE_FROM_ALL)
add_executable(VetroLook WIN32 src/main.cpp src/gfx.cpp src/pipeline.cpp src/theme.cpp src/meta.cpp src/metaread.cpp src/metacache.cpp src/favourites.cpp src/lensdb.cpp src/thumbs.cpp src/index.cpp src/usn.cpp src/dnd.cpp src/actions.cpp src/shellreg.cpp src/share.cpp src/decoders.cpp src/psd.cpp src/wuffs.cpp src/explorer.cpp src/integrations.cpp src/tc_ini.cpp third_party/exif.cpp resources/app.rc)
target_include_directories(VetroLook PRIVATE "${ROOT}/libjpeg-turbo/src" "${CMAKE_BINARY_DIR}/jpeg" "${ROOT}/libwebp/src" "${ROOT}/tinyexr/deps/miniz" third_party)
target_compile_definitions(VetroLook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX LIBRAW_NODLL _CRT_SECURE_NO_WARNINGS)
target_compile_options(VetroLook PRIVATE /W4 /utf-8 /O2 /Gy /Gw /MP)
target_link_options(VetroLook PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
target_link_libraries(VetroLook PRIVATE turbojpeg-static webpdecoder webpdemux vetro_raw vetro_exr vetro_exiv2 avif d2d1 d3d11 dxgi dxguid dcomp dwrite dwmapi windowscodecs shell32 shlwapi ole32 oleaut32 uuid comdlg32 advapi32 gdi32 psapi runtimeobject windowsapp)
if(EXISTS "${PARENT_ROOT}/libjpeg-turbo")
 set(VETRO_DEFAULT_OUTPUT_DIR "${PARENT_ROOT}/dist")
else()
 set(VETRO_DEFAULT_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
endif()
set(VETRO_OUTPUT_DIR "${VETRO_DEFAULT_OUTPUT_DIR}" CACHE PATH "Application output directory")
set_target_properties(VetroLook PROPERTIES OUTPUT_NAME "VetroLook" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${VETRO_OUTPUT_DIR}")
# The Lensfun database travels with the application: 5 MB of CC-BY-SA 3.0 XML,
# read at runtime by lensdb.cpp. See third_party/lensfun-db/COPYING.CC_BY-SA_3.0.
add_custom_command(TARGET VetroLook POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/README.txt" "$<TARGET_FILE_DIR:VetroLook>/README.txt"
 COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" "$<TARGET_FILE_DIR:VetroLook>/THIRD_PARTY_NOTICES.txt"
 COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:VetroLook>/lensfun-db"
 COMMAND ${CMAKE_COMMAND} -E copy_directory
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun-db" "$<TARGET_FILE_DIR:VetroLook>/lensfun-db"
 COMMENT "Copying the Lensfun lens database")

option(VETRO_REVIEW_MODE "Isolate this build from the installed application" OFF)
if(VETRO_REVIEW_MODE)
 target_compile_definitions(VetroLook PRIVATE VETRO_REVIEW_BUILD)
endif()


add_executable(VetroActionTests EXCLUDE_FROM_ALL tests/actions-test.cpp src/actions.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroIndexTests EXCLUDE_FROM_ALL tests/index-test.cpp src/index.cpp src/usn.cpp)
target_compile_definitions(VetroIndexTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(VetroIndexTests PRIVATE /utf-8 /W4)
target_link_libraries(VetroIndexTests PRIVATE shell32 ole32 advapi32)
add_executable(VetroDndTests EXCLUDE_FROM_ALL tests/dnd-test.cpp src/dnd.cpp)
target_compile_definitions(VetroDndTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(VetroDndTests PRIVATE /utf-8 /W4)
target_link_libraries(VetroDndTests PRIVATE shell32 ole32)
add_executable(VetroUsnDiagnostic EXCLUDE_FROM_ALL tests/usn-diagnostic.cpp src/usn.cpp)
add_executable(VetroTcIniTests EXCLUDE_FROM_ALL tests/tc-ini-test.cpp src/tc_ini.cpp)
target_compile_definitions(VetroTcIniTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(VetroTcIniTests PRIVATE /utf-8 /W4)
add_executable(VetroOrientationTests EXCLUDE_FROM_ALL tests/orientation-test.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroPsdMemoryTests EXCLUDE_FROM_ALL tests/psd-memory-test.cpp tests/psd-baseline.cpp src/psd.cpp)
add_executable(VetroMetadataTests EXCLUDE_FROM_ALL tests/metadata-test.cpp src/metaread.cpp src/metacache.cpp src/favourites.cpp src/lensdb.cpp)
add_executable(VetroBench EXCLUDE_FROM_ALL tests/bench.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroDecodeProbe EXCLUDE_FROM_ALL tests/decode-probe.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroRawStressTests EXCLUDE_FROM_ALL tests/raw-stress-test.cpp src/thumbs.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroFilmstripStressTests EXCLUDE_FROM_ALL tests/filmstrip-stress-test.cpp src/thumbs.cpp src/decoders.cpp src/pipeline.cpp src/psd.cpp src/wuffs.cpp third_party/exif.cpp)
add_executable(VetroGuiBench EXCLUDE_FROM_ALL tests/blackbox-harness.cpp)
target_compile_definitions(VetroGuiBench PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(VetroGuiBench PRIVATE /utf-8 /W4 /O2)
target_link_libraries(VetroGuiBench PRIVATE d3d11 dxgi psapi user32 gdi32 shell32)

# A keyboard-driven visual target used only to smoke-test the independent GUI
# benchmark harness on machines where no viewer is available yet.
add_executable(VetroGuiBenchTestWindow WIN32 EXCLUDE_FROM_ALL tests/blackbox-test-window.cpp)
target_compile_definitions(VetroGuiBenchTestWindow PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(VetroGuiBenchTestWindow PRIVATE /utf-8 /W4 /O2)
target_link_libraries(VetroGuiBenchTestWindow PRIVATE user32 gdi32)
target_compile_definitions(VetroUsnDiagnostic PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(VetroUsnDiagnostic PRIVATE /utf-8 /W4)
target_link_libraries(VetroUsnDiagnostic PRIVATE advapi32)
get_target_property(VETRO_INCLUDES VetroLook INCLUDE_DIRECTORIES)
get_target_property(VETRO_DEFINITIONS VetroLook COMPILE_DEFINITIONS)
get_target_property(VETRO_LIBRARIES VetroLook LINK_LIBRARIES)
target_include_directories(VetroActionTests PRIVATE src ${VETRO_INCLUDES})
target_compile_definitions(VetroActionTests PRIVATE ${VETRO_DEFINITIONS})
target_compile_options(VetroActionTests PRIVATE /utf-8 /O2)
target_link_libraries(VetroActionTests PRIVATE ${VETRO_LIBRARIES})
target_include_directories(VetroDecodeProbe PRIVATE src ${VETRO_INCLUDES})
target_compile_definitions(VetroDecodeProbe PRIVATE ${VETRO_DEFINITIONS})
target_compile_options(VetroDecodeProbe PRIVATE /utf-8 /O2)
target_link_libraries(VetroDecodeProbe PRIVATE ${VETRO_LIBRARIES} psapi)
foreach(target VetroRawStressTests VetroFilmstripStressTests VetroBench VetroMetadataTests VetroPsdMemoryTests VetroOrientationTests)
 target_include_directories(${target} PRIVATE src ${VETRO_INCLUDES})
 target_compile_definitions(${target} PRIVATE ${VETRO_DEFINITIONS})
 target_compile_options(${target} PRIVATE /utf-8 /O2)
 target_link_libraries(${target} PRIVATE ${VETRO_LIBRARIES} psapi)
endforeach()
target_compile_definitions(VetroFilmstripStressTests PRIVATE VETRO_THUMB_TESTS)
target_compile_definitions(VetroRawStressTests PRIVATE VETRO_THUMB_TESTS)


