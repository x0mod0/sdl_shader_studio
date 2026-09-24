cmake_minimum_required(VERSION 3.21)
project(sdl_shader_studio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(SSSTUDIO_BUILD_GUI       "Build the desktop application"        ON)
option(SSSTUDIO_BUILD_CLI       "Build the ssstudio command line tool" ON)
option(SSSTUDIO_BUILD_TESTS     "Build the test suite"                 ON)
option(SSSTUDIO_WITH_SHADERCROSS "Use SDL_shadercross for compilation" ON)
option(SSSTUDIO_WITH_GLSLANG    "Enable the GLSL front end (glslang)"  ON)
option(SSSTUDIO_WITH_LZ4        "Enable LZ4 pack compression"          ON)
option(SSSTUDIO_WITH_ZSTD       "Enable zstd pack compression"         OFF)
option(SSSTUDIO_WITH_IMAGES     "Load preview textures with SDL3_image"  ON)
option(SSSTUDIO_WITH_AVIF       "Also decode AVIF (pulls dav1d and aom)" OFF)

# The Windows icon is linked in as a resource, which needs the RC compiler.
# enable_language() only works at file scope, so it cannot live in AppIcon.cmake.
if(WIN32)
    enable_language(RC)
endif()
# The same goes for Objective-C++, which the macOS menu bar is written in
# (src/gui/native_menu_mac.mm): AppKit has no C or C++ interface.
if(APPLE)
    enable_language(OBJCXX)
endif()

include(cmake/Dependencies.cmake)
include(cmake/AppIcon.cmake)
include(cmake/BundleTools.cmake)

# ---------------------------------------------------------------------------
# Embed the loader template into the core library.
# ---------------------------------------------------------------------------
set(SSSTUDIO_TEMPLATE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/templates/s3pack.h")
set(SSSTUDIO_TEMPLATE_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/s3pack_template.cpp")
add_custom_command(
    OUTPUT "${SSSTUDIO_TEMPLATE_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${SSSTUDIO_TEMPLATE_SRC}
            -DOUTPUT=${SSSTUDIO_TEMPLATE_CPP}
            -DSYMBOL=kS3PackTemplate
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_file.cmake"
    DEPENDS "${SSSTUDIO_TEMPLATE_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_file.cmake"
    COMMENT "Embedding templates/s3pack.h"
    VERBATIM)

# ---------------------------------------------------------------------------
# Core library: no SDL dependency, so it builds and tests anywhere.
# ---------------------------------------------------------------------------
add_library(ssstudio_core STATIC
    src/core/types.cpp
    src/core/process.cpp
    src/core/reflection.cpp
    src/core/spirv_reflect.cpp
    src/core/packer.cpp
    src/core/pack_reader.cpp
    src/core/compress.cpp
    src/core/keys.cpp
    src/core/codegen.cpp
    src/core/compiler.cpp
    src/core/glslang_backend.cpp
    src/core/graph.cpp
    src/core/graph_codegen.cpp
    src/core/graph_io.cpp
    src/core/templates.cpp
    src/core/color.cpp
    src/core/completion.cpp
    src/core/indent.cpp
    src/core/syntax.cpp
    src/core/lint.cpp
    src/core/cache.cpp
    src/core/diff.cpp
    src/core/project.cpp
    src/core/settings.cpp
    src/core/theme_pack.cpp
    src/core/asset_cache.cpp
    src/core/json.cpp
    src/core/pass_graph.cpp
    src/core/sampler_state.cpp
    src/core/shader_import.cpp
    src/core/build.cpp
    "${SSSTUDIO_TEMPLATE_CPP}")
add_library(ssstudio::core ALIAS ssstudio_core)

target_include_directories(ssstudio_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(ssstudio_core PUBLIC
    SSSTUDIO_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(ssstudio_core PUBLIC tomlplusplus::tomlplusplus)

if(SSSTUDIO_WITH_LZ4 AND TARGET LZ4::lz4)
    target_link_libraries(ssstudio_core PRIVATE LZ4::lz4)
    target_compile_definitions(ssstudio_core PRIVATE SSSTUDIO_HAVE_LZ4)
endif()
if(SSSTUDIO_WITH_ZSTD AND TARGET zstd::libzstd)
    target_link_libraries(ssstudio_core PRIVATE zstd::libzstd)
    target_compile_definitions(ssstudio_core PRIVATE SSSTUDIO_HAVE_ZSTD)
endif()
if(SSSTUDIO_WITH_GLSLANG AND TARGET glslang::glslang)
    target_link_libraries(ssstudio_core PRIVATE glslang::glslang glslang::SPIRV)
    # GetDefaultResources() lives in its own glslang library.
    if(TARGET glslang::glslang-default-resource-limits)
        target_link_libraries(ssstudio_core PRIVATE glslang::glslang-default-resource-limits)
    elseif(TARGET glslang-default-resource-limits)
        target_link_libraries(ssstudio_core PRIVATE glslang-default-resource-limits)
    endif()
    target_compile_definitions(ssstudio_core PRIVATE SSSTUDIO_HAVE_GLSLANG)
endif()
if(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE STREQUAL "library")
    target_link_libraries(ssstudio_core PRIVATE SDL3_shadercross::SDL3_shadercross SDL3::SDL3)
    target_compile_definitions(ssstudio_core PRIVATE SSSTUDIO_HAVE_SHADERCROSS)
elseif(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE STREQUAL "cli" AND SSSTUDIO_SHADERCROSS_CLI_DIR)
    # Not linked on purpose: see cmake/Dependencies.cmake. The tool is looked up
    # again at runtime, so this is only where to start looking.
    target_compile_definitions(ssstudio_core PRIVATE
        SSSTUDIO_SHADERCROSS_CLI_DIR="${SSSTUDIO_SHADERCROSS_CLI_DIR}")
endif()

if(MSVC)
    target_compile_options(ssstudio_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ssstudio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(SSSTUDIO_BUILD_CLI)
    add_executable(ssstudio src/cli/main.cpp)
    target_link_libraries(ssstudio PRIVATE ssstudio_core)
    # The command line tool compiles shaders too, and finds its toolchain the
    # same way: a `tools/` directory beside the executable.
    ssstudio_bundle_tools(ssstudio)
endif()

# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
if(SSSTUDIO_BUILD_GUI AND TARGET SDL3::SDL3)
    add_executable(sdl_shader_studio WIN32 MACOSX_BUNDLE
        src/gui/main.cpp
        src/gui/app.cpp
        src/gui/chrome.cpp
        src/gui/theme.cpp
        src/gui/fonts.cpp
        src/gui/menu_model.cpp
        src/gui/widgets.cpp
        src/gui/panels/editor_panel.cpp
        src/gui/panels/io_panel.cpp
        src/gui/panels/preview_panel.cpp
        src/gui/panels/build_panel.cpp
        src/gui/panels/settings_panel.cpp
        src/gui/panels/graph_panel.cpp
        src/gui/panels/scene_panel.cpp
        src/gui/scene/scene.cpp
        src/gui/scene/scene_io.cpp
        src/gui/scene/batcher.cpp
        src/gui/text_editor.cpp
        src/gui/file_dialog.cpp
        src/gui/preview/renderer.cpp
        src/gui/preview/texture_registry.cpp
        ${IMGUI_SOURCES})
    target_include_directories(sdl_shader_studio PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui"
        ${IMGUI_INCLUDE_DIRS})
    target_link_libraries(sdl_shader_studio PRIVATE ssstudio_core SDL3::SDL3)
    # The menus go in the system menu bar where there is one, and stay in the
    # window's top bar everywhere else.
    if(APPLE)
        target_sources(sdl_shader_studio PRIVATE src/gui/native_menu_mac.mm)
        set_source_files_properties(src/gui/native_menu_mac.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(sdl_shader_studio PRIVATE "-framework AppKit")
    else()
        target_sources(sdl_shader_studio PRIVATE src/gui/native_menu_other.cpp)
    endif()
    if(TARGET SDL3_image::SDL3_image)
        target_link_libraries(sdl_shader_studio PRIVATE SDL3_image::SDL3_image)
        target_compile_definitions(sdl_shader_studio PRIVATE SSSTUDIO_HAVE_SDL_IMAGE)
        if(SSSTUDIO_WITH_AVIF)
            target_compile_definitions(sdl_shader_studio PRIVATE SSSTUDIO_IMAGE_AVIF)
        endif()
    endif()
    # Only in library mode: a prebuilt SDL_shadercross loads the SDL3 it shipped
    # with, and linking it here would put that second copy in the app whether or
    # not the core library asked for it.
    if(SSSTUDIO_SHADERCROSS_EFFECTIVE_MODE STREQUAL "library")
        target_link_libraries(sdl_shader_studio PRIVATE SDL3_shadercross::SDL3_shadercross)
    endif()
    set_target_properties(sdl_shader_studio PROPERTIES OUTPUT_NAME "SS Studio")
    # macOS fills these into the bundle's Info.plist. They are not cosmetic: an
    # empty CFBundleIdentifier makes the system refuse to serve the app its
    # out-of-process open/save panel, so every native file picker silently never
    # appears and never calls back.
    set_target_properties(sdl_shader_studio PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER      "com.x0mod0.s3tudio"
        MACOSX_BUNDLE_BUNDLE_NAME         "SS Studio"
        MACOSX_BUNDLE_BUNDLE_VERSION      "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT           "SDL Shader Studio")
    ssstudio_add_app_icon(sdl_shader_studio)
    # Ships the shader toolchain inside the application, so a copy handed to
    # someone else compiles HLSL without them installing anything. Settings >
    # Tools still wins, so a different toolchain can be pointed at from the app.
    ssstudio_bundle_tools(sdl_shader_studio)
    # The theme packs that ship with the application. Copied rather than
    # referenced from the source tree, because the app looks beside its own
    # executable - and inside Contents/Resources in a bundle - so that a copy
    # handed to someone else carries its themes with it.
    if(APPLE)
        set(SSSTUDIO_THEME_DEST "$<TARGET_BUNDLE_CONTENT_DIR:sdl_shader_studio>/Resources/themes")
    else()
        set(SSSTUDIO_THEME_DEST "$<TARGET_FILE_DIR:sdl_shader_studio>/themes")
    endif()
    # Driven by a stamp that depends on the theme files themselves, not by a
    # POST_BUILD on the application: a POST_BUILD only runs when the app
    # relinks, so editing or adding a theme would not have shipped it until
    # something else happened to change. CONFIGURE_DEPENDS re-globs when a file
    # appears, which is the other half of the same problem.
    #
    # Recursive, because a pack can be a directory: an edit to the theme.toml
    # inside one changes no file at the top level, and a flat glob would only
    # have seen the directory itself.
    file(GLOB_RECURSE SSSTUDIO_THEME_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/themes/*")
    # The default theme's fonts travel inside its pack, and so with the copy
    # below. Said at configure time when one is missing, because the app itself
    # only falls back to the built-in font - a build shipped without them would
    # look wrong without anything having failed.
    foreach(font IN ITEMS IBMPlexSans-Regular.ttf JetBrainsMono-Regular.ttf)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/themes/tide-dark.s3theme/fonts/${font}")
            message(WARNING "themes/tide-dark.s3theme/fonts/${font} is missing; the app will "
                            "draw the Tide themes in its built-in font. See the README there.")
        endif()
    endforeach()
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ssstudio_themes.stamp"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/themes" "${SSSTUDIO_THEME_DEST}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/ssstudio_themes.stamp"
        DEPENDS ${SSSTUDIO_THEME_FILES}
        COMMENT "Copying theme packs")
    add_custom_target(ssstudio_themes ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ssstudio_themes.stamp")
    # After the app, so the bundle it copies into already exists.
    add_dependencies(ssstudio_themes sdl_shader_studio)
elseif(SSSTUDIO_BUILD_GUI)
    message(WARNING "SDL3 was not found; only the CLI and tests will be built")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SSSTUDIO_BUILD_TESTS)
    enable_testing()
    # Render the loader template with the default layout so the loader test can
    # #include it. Same substitution the tool performs at build time.
    set(TOOL_VERSION "SDL Shader Studio ${PROJECT_VERSION} (test)")
    set(LAYOUT_SIGNATURE "S3PK/v1.0/compact/by_key/table_first/key32/align16/names")
    set(MAGIC "S3PK")
    set(PACK_EXT ".s3pack")
    set(VERSION_MAJOR 1)
    set(VERSION_MINOR 0)
    configure_file(templates/s3pack.h
                   "${CMAKE_CURRENT_BINARY_DIR}/generated/generated_s3pack.h" @ONLY)

    add_executable(ssstudio_tests
        tests/main.cpp
        tests/test_pack_roundtrip.cpp
        tests/test_keys.cpp
        tests/test_codegen.cpp
        tests/test_reflection.cpp
        tests/test_loader.cpp
        tests/test_graph.cpp
        tests/test_efficiency.cpp
        tests/test_color.cpp
        tests/test_completion.cpp
        tests/test_indent.cpp
        tests/test_syntax.cpp
        tests/test_shader_import.cpp
        tests/test_sampler_state.cpp
        tests/test_asset_cache.cpp
        tests/test_pass_graph.cpp
        tests/test_json.cpp
        tests/test_chain_import.cpp
        tests/test_lint.cpp
        tests/test_scene.cpp
        tests/test_editor_tabs.cpp
        tests/test_theme_pack.cpp
        # The scene layer is GUI-only by design; the test target compiles those
        # two files directly rather than linking a GUI library, which is what
        # keeps the core (and therefore build output) free of scene code.
        src/gui/scene/scene.cpp
        src/gui/scene/batcher.cpp
        tests/stub/sdl_stub.c)
    target_link_libraries(ssstudio_tests PRIVATE ssstudio_core)
    target_include_directories(ssstudio_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stub"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_test(NAME ssstudio_tests COMMAND ssstudio_tests)
endif()

ssstudio_install_tools("bin/tools/shadercross")
install(TARGETS ssstudio_core ARCHIVE DESTINATION lib)
if(SSSTUDIO_BUILD_CLI)
    install(TARGETS ssstudio RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
