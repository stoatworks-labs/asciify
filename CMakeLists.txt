cmake_minimum_required(VERSION 3.15)

project(Asciify
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal build by default so the plugin loads in both native Apple Silicon
# and Intel builds of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a faster single-arch dev build.
#
# This must sit immediately after project() and before any target is defined.
# CMake latches the architecture list when the first target is created, and
# setting it later is silently ignored -- you get an arm64-only binary that the
# build log calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

# MSVC guesses the source encoding from the system code page. This repo is
# UTF-8 and the font data is full of block-drawing characters in comments, so
# say so: the failure mode is not a build error, it is a silently mangled
# string literal.
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(ASCIIFY_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule under external/.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. CFFGLPluginInfo registers itself from a
# file-scope constructor in Asciify.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(asciify_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Font.h
    source/Font.cpp
    source/FontData.cpp
    source/Match.h
    source/Match.cpp
    source/Alphabet.h
    source/Alphabet.cpp
    source/Controls.h
    source/Controls.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Asciify.h
    source/Asciify.cpp
)

target_include_directories(asciify_core PUBLIC source)
target_link_libraries(asciify_core PUBLIC ffgl::sdk)
target_compile_definitions(asciify_core PUBLIC ASCIIFY_VERSION="${PROJECT_VERSION}")
set_target_properties(asciify_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too for the include path (and for the
# GLEW_STATIC define the static vcpkg triplet propagates). macOS uses the system
# OpenGL framework and needs none of this.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(asciify_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin itself: a loadable macOS .bundle (MODULE library).
# ---------------------------------------------------------------------------
add_library(Asciify MODULE source/PluginEntry.cpp)
target_link_libraries(Asciify PRIVATE asciify_core)

if(APPLE)
    set_target_properties(Asciify PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Asciify"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Asciify"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.asciify"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(Asciify PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# Offline harness. Which character a cell chose is a fact, not a matter of
# taste, so this is where the effect is actually verified rather than merely
# previewed. See tools/asctest.
# ---------------------------------------------------------------------------
if(ASCIIFY_BUILD_TOOLS AND APPLE)
    add_executable(asctest tools/asctest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(asctest PRIVATE asciify_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS Asciify
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
