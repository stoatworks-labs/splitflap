cmake_minimum_required(VERSION 3.15)

project(Splitflap
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(SPLITFLAP_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin shares, as an OBJECT library.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and
# the result is a bundle that loads, exports plugMain, and reports that it
# contains no plugins.
#
# Note what is NOT in here: PluginEntry.cpp. It carries the CFFGLPluginInfo and
# is listed directly in the MODULE target below.
# ---------------------------------------------------------------------------
add_library(splitflap_core OBJECT
    source/Controls.h
    source/Controls.cpp
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Diag.h
    source/Diag.cpp
    source/Clock.h
    source/Clock.cpp
    source/Font.h
    source/Font.cpp
    source/Flap.h
    source/Flap.cpp
    source/Drum.h
    source/Drum.cpp
    source/Onset.h
    source/Onset.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Splitflap.h
    source/Splitflap.cpp
)

target_include_directories(splitflap_core PUBLIC source)
target_link_libraries(splitflap_core PUBLIC ffgl::sdk)
target_compile_definitions(splitflap_core PUBLIC SPLITFLAP_VERSION="${PROJECT_VERSION}")
set_target_properties(splitflap_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(splitflap_core PUBLIC Threads::Threads)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(splitflap_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(splitflap_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE splitflap_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

splitflap_add_plugin(Splitflap source/PluginEntry.cpp
    "Splitflap" "com.stoatworks.ffgl.splitflap")

# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Splitflap
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a headless CGL context.
#
# macOS only, because it stands up a CGL context directly. Every check in it
# reads the rendered picture: the flip counts, the asymmetry, the settle, the
# flap's fall, the resize and the priming are all measured off pixels the
# shipped shaders drew, never off the state texture.
# ---------------------------------------------------------------------------
if(SPLITFLAP_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(sftest tools/sftest/main.cpp)
    target_link_libraries(sftest PRIVATE splitflap_core z "-framework OpenGL")
    target_compile_options(sftest PRIVATE -Wno-deprecated-declarations)

    foreach(check flips asymmetry settle fall resize prime negative names font)
        add_test(NAME ${check} COMMAND sftest --${check})
    endforeach()
endif()
