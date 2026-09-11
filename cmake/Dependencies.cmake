# Dependency resolution.
#
# Everything prefers an installed package and falls back to FetchContent, so a
# checkout builds offline if the dependencies are already present and still works
# on a clean machine with network access.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Files unpacked from a URL download get the time they were unpacked rather than
# the time stored in the archive, so moving a dependency always rebuilds it.
# CMake before 3.24 has no such policy and needs nothing here.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# --- toml++ (manifest and settings) ----------------------------------------
find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# --- SDL3 (GUI and preview only; the core never links it) -------------------
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.0
        GIT_SHALLOW TRUE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
endif()

# Where the SDL_shadercross library file sits, so the directory can be searched
# for an SDL3 shipped beside it. An imported target names its file through one of
# several configuration-specific properties, and which ones exist depends on how
# the package was built, so all of them are tried.
function(ssstudio_shadercross_library_dir out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT TARGET SDL3_shadercross::SDL3_shadercross)
        return()
    endif()
    get_target_property(configurations SDL3_shadercross::SDL3_shadercross IMPORTED_CONFIGURATIONS)
    set(properties IMPORTED_LOCATION)
    foreach(config IN LISTS configurations)
        list(APPEND properties "IMPORTED_LOCATION_${config}")
    endforeach()
    foreach(property IN LISTS properties)
        get_target_property(location SDL3_shadercross::SDL3_shadercross ${property})
        if(location AND EXISTS "${location}")
            get_filename_component(directory "${location}" DIRECTORY)
            set(${out_var} "${directory}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# --- SDL_shadercross (HLSL -> SPIR-V/DXIL/DXBC/MSL) -------------------------
#
# SDL_shadercross can be used two ways, and which one is safe depends on where it
# came from. Linking the library is the cheaper of the two, but a prebuilt release
# ships the SDL3 it was built against and loads it through its own rpath, so
# linking that one puts a second SDL3 in the process. The two copies do not share
# the error buffer the compiler reports through, which is why HLSL errors from
# such a build arrive with no message, file or line. Running the command line tool
# instead costs a process per compile and keeps the app to a single SDL3.
set(SSSTUDIO_SHADERCROSS_MODE "auto" CACHE STRING
    "How to reach SDL_shadercross: auto, library or cli")
set_property(CACHE SSSTUDIO_SHADERCROSS_MODE PROPERTY STRINGS auto library cli)

set(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE "none")
set(SSSTUDIO_SHADERCROSS_CLI_DIR "")

if(SSSTUDIO_WITH_SHADERCROSS)
    find_package(SDL3_shadercross QUIET CONFIG)
    set(ssstudio_shadercross_prebuilt ${SDL3_shadercross_FOUND})
    if(NOT SDL3_shadercross_FOUND)
        # Pinned to a commit on main rather than main itself, so a clean configure
        # always builds the same compiler. That also pins SPIRV-Cross, DXC and the
        # other submodules, whose exact revisions the commit records. Not shallow:
        # a shallow clone only reaches branch and tag tips, and this repository is
        # small enough that the full history costs nothing.
        FetchContent_Declare(SDL3_shadercross
            GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross.git
            GIT_TAG 1ff05bec573988a98ef9e0260b4da44f512b8367) # main, 2026-09-03
        set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "" FORCE)
        set(SDLSHADERCROSS_CLI OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3_shadercross)
    endif()

    if(NOT TARGET SDL3_shadercross::SDL3_shadercross)
        message(WARNING
            "SDL_shadercross is unavailable. The app still builds and runs, but shader "
            "compilation is disabled until a toolchain is configured in Settings > Tools.")
    else()
        # A shadercross built here shares this project's SDL3 target and can only
        # be linked. Only an installed one can bring its own SDL3 along.
        set(ssstudio_ships_own_sdl3 FALSE)
        if(ssstudio_shadercross_prebuilt)
            ssstudio_shadercross_library_dir(ssstudio_shadercross_libdir)
            if(ssstudio_shadercross_libdir)
                file(GLOB ssstudio_bundled_sdl3
                    "${ssstudio_shadercross_libdir}/libSDL3.*"
                    "${ssstudio_shadercross_libdir}/SDL3.dll"
                    "${ssstudio_shadercross_libdir}/../bin/SDL3.dll")
                if(ssstudio_bundled_sdl3)
                    set(ssstudio_ships_own_sdl3 TRUE)
                endif()
            endif()
        endif()

        if(SSSTUDIO_SHADERCROSS_MODE STREQUAL "auto")
            if(ssstudio_ships_own_sdl3)
                set(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE "cli")
            else()
                set(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE "library")
            endif()
        else()
            set(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE "${SSSTUDIO_SHADERCROSS_MODE}")
        endif()

        if(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE STREQUAL "cli")
            find_program(SSSTUDIO_SHADERCROSS_EXECUTABLE
                NAMES shadercross
                HINTS "${ssstudio_shadercross_libdir}/../bin" "${ssstudio_shadercross_libdir}/.."
                PATH_SUFFIXES bin)
            if(SSSTUDIO_SHADERCROSS_EXECUTABLE)
                get_filename_component(SSSTUDIO_SHADERCROSS_CLI_DIR
                                       "${SSSTUDIO_SHADERCROSS_EXECUTABLE}" DIRECTORY)
                message(STATUS
                    "SDL_shadercross: running ${SSSTUDIO_SHADERCROSS_EXECUTABLE} out of process "
                    "(it ships its own SDL3, so linking it would load a second one)")
            else()
                message(WARNING
                    "SDL_shadercross ships its own SDL3, so it is not linked. The shadercross "
                    "tool was not found either; set Settings > Tools > shadercross dir at "
                    "runtime, or pass -DSSSTUDIO_SHADERCROSS_MODE=library to link it anyway and "
                    "accept the lost compiler messages.")
            endif()
        else()
            message(STATUS "SDL_shadercross: linked as a library")
        endif()
    endif()
endif()

# --- glslang (GLSL front end) -----------------------------------------------
if(SSSTUDIO_WITH_GLSLANG)
    find_package(glslang QUIET CONFIG)
    if(NOT glslang_FOUND)
        FetchContent_Declare(glslang
            GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
            GIT_TAG 14.3.0
            GIT_SHALLOW TRUE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
        set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
        set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
        # glslang's spirv-opt integration needs SPIRV-Tools vendored into its
        # source tree (update_glslang_sources.py), which FetchContent does not
        # do. We only need the GLSL front end, so build without the optimizer.
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(glslang)
    endif()
    if(NOT TARGET glslang::glslang)
        message(STATUS "glslang not found; GLSL authoring will be disabled")
    endif()
endif()

# --- LZ4 / zstd (optional pack compression) ---------------------------------
if(SSSTUDIO_WITH_LZ4)
    find_package(lz4 QUIET)
    if(NOT TARGET LZ4::lz4)
        find_library(LZ4_LIBRARY NAMES lz4 liblz4)
        find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
        if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
            add_library(LZ4::lz4 UNKNOWN IMPORTED)
            set_target_properties(LZ4::lz4 PROPERTIES
                IMPORTED_LOCATION "${LZ4_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}")
        else()
            message(STATUS "LZ4 not found; packs will be written uncompressed")
        endif()
    endif()
endif()

if(SSSTUDIO_WITH_ZSTD)
    find_package(zstd QUIET)
    if(NOT TARGET zstd::libzstd)
        message(STATUS "zstd not found; zstd compression will be unavailable")
    endif()
endif()

# --- SDL3_image (preview textures; GUI only) --------------------------------
#
# What the formats cost is not evenly spread, and the difference is worth
# spelling out because the defaults get it wrong for this project:
#
#   * BMP, GIF, TGA, QOI, PNM and SVG are built in and cost nothing;
#   * PNG and JPEG come from the platform's own decoder on Apple (ImageIO) and
#     from vendored libpng / libjpeg elsewhere - both small;
#   * WebP always needs libwebp, which is modest;
#   * AVIF needs libavif *plus* dav1d *plus* aom, which together are larger than
#     everything else here combined.
#
# So AVIF is opt-in. The submodule list matters as much as the format switches:
# FetchContent clones every submodule a repository declares unless it is told
# otherwise, so leaving it unset would fetch aom whether or not AVIF is wanted.
if(SSSTUDIO_BUILD_GUI AND SSSTUDIO_WITH_IMAGES)
    find_package(SDL3_image QUIET CONFIG)
    if(NOT SDL3_image_FOUND)
        set(ssstudio_image_submodules
            "external/libpng"
            "external/zlib"
            "external/jpeg"
            "external/libwebp")
        if(SSSTUDIO_WITH_AVIF)
            list(APPEND ssstudio_image_submodules
                "external/libavif" "external/dav1d" "external/aom")
        endif()

        FetchContent_Declare(SDL3_image
            GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
            GIT_TAG release-3.4.6
            GIT_SHALLOW TRUE
            GIT_SUBMODULES "${ssstudio_image_submodules}")

        # Vendored and strict together. Vendored on its own is not enough: with
        # SDLIMAGE_STRICT off, a decoder whose library is missing is dropped at
        # configure time without a word, and the first anyone hears of it is a
        # .webp that will not open on someone else's machine.
        set(SDLIMAGE_VENDORED ON  CACHE BOOL "" FORCE)
        set(SDLIMAGE_STRICT   ON  CACHE BOOL "" FORCE)
        set(SDLIMAGE_INSTALL  OFF CACHE BOOL "" FORCE)
        set(SDLIMAGE_SAMPLES  OFF CACHE BOOL "" FORCE)
        set(SDLIMAGE_TESTS    OFF CACHE BOOL "" FORCE)
        set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
        set(SDLIMAGE_WEBP_SHARED OFF CACHE BOOL "" FORCE)

        set(SDLIMAGE_AVIF ${SSSTUDIO_WITH_AVIF} CACHE BOOL "" FORCE)
        # Nothing here reads these, and each one is a submodule not cloned.
        foreach(format TIF JXL XCF XV LBM PCX ANI XPM)
            set(SDLIMAGE_${format} OFF CACHE BOOL "" FORCE)
        endforeach()

        FetchContent_MakeAvailable(SDL3_image)
    endif()

    if(NOT TARGET SDL3_image::SDL3_image)
        message(WARNING
            "SDL3_image is unavailable. The app still builds; texture bindings will "
            "report that image support was not compiled in.")
    endif()
endif()

# --- Dear ImGui (docking branch) --------------------------------------------
#
# Pinned to a docking-branch commit (1.93.0 WIP, 2026-09-07), which is newer than
# the last v*-docking tag. It is downloaded as a source archive rather than
# cloned: the repository carries over 100 MB of history against a 2 MB archive,
# and URL_HASH fails the build if the download is ever not what was reviewed.
# To move it, change the commit in the URL and replace the hash with what
# `shasum -a 256` reports for the new archive.
if(SSSTUDIO_BUILD_GUI)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/a2b7d6e7928e92a0b764d7bd8f3faceb660e60a7.tar.gz
        URL_HASH SHA256=4780fa950e2ceb4f2bdb1c9b71d5bbe6d8e7aa286635ee363819598db7de29bf)
    FetchContent_MakeAvailable(imgui)

    set(IMGUI_INCLUDE_DIRS
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends")
    set(IMGUI_SOURCES
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp")
endif()
