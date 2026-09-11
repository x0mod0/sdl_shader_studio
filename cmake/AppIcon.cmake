# Application icon.
#
# assets/icons is the single source of artwork; each platform wants it in its
# own container, so ssstudio_add_app_icon(<target>) attaches whichever applies:
#
#   macOS    AppIcon.icns, assembled here by iconutil and copied into
#            <bundle>/Contents/Resources, named by MACOSX_BUNDLE_ICON_FILE
#   Windows  assets/icons/app.ico, linked in as a resource via app.rc
#   Linux    PNGs installed into the hicolor theme, found by the .desktop entry
#
# Nothing here is required to produce a working binary: on a machine without
# the packaging tool the target still builds, with the platform's stock icon.

get_filename_component(SSSTUDIO_ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets" ABSOLUTE)

# Source size -> name inside AppIcon.iconset. Several sizes appear twice
# because a slot's @2x image is the same file as the next slot's @1x.
set(SSSTUDIO_MACOS_ICONSET
    16   icon_16x16.png
    32   icon_16x16@2x.png
    32   icon_32x32.png
    64   icon_32x32@2x.png
    128  icon_128x128.png
    256  icon_128x128@2x.png
    256  icon_256x256.png
    512  icon_256x256@2x.png
    512  icon_512x512.png
    1024 icon_512x512@2x.png)

# Sizes installed into share/icons/hicolor on Linux.
set(SSSTUDIO_HICOLOR_SIZES 16 32 48 64 128 256 512)

function(ssstudio_add_app_icon target)
    if(APPLE)
        find_program(SSSTUDIO_ICONUTIL iconutil)
        if(NOT SSSTUDIO_ICONUTIL)
            message(WARNING
                "iconutil was not found, so the bundle keeps the stock icon. "
                "It ships with macOS; a broken xcode-select can hide it.")
            return()
        endif()

        set(iconset "${CMAKE_CURRENT_BINARY_DIR}/generated/AppIcon.iconset")
        set(icns    "${CMAKE_CURRENT_BINARY_DIR}/generated/AppIcon.icns")

        # iconutil reads a directory, so the PNGs are staged under the names it
        # expects. The staging directory is wiped first: a stale file left from
        # an earlier size list would silently end up in the .icns.
        set(stage "")
        set(inputs "")
        list(LENGTH SSSTUDIO_MACOS_ICONSET count)
        math(EXPR last "${count} - 2")
        foreach(i RANGE 0 ${last} 2)
            math(EXPR j "${i} + 1")
            list(GET SSSTUDIO_MACOS_ICONSET ${i} size)
            list(GET SSSTUDIO_MACOS_ICONSET ${j} name)
            set(png "${SSSTUDIO_ASSET_DIR}/icons/icon-${size}.png")
            if(NOT EXISTS "${png}")
                message(FATAL_ERROR "app icon source is missing: ${png}")
            endif()
            list(APPEND stage COMMAND ${CMAKE_COMMAND} -E copy "${png}" "${iconset}/${name}")
            list(APPEND inputs "${png}")
        endforeach()
        list(REMOVE_DUPLICATES inputs)

        add_custom_command(
            OUTPUT "${icns}"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${iconset}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${iconset}"
            ${stage}
            COMMAND "${SSSTUDIO_ICONUTIL}" --convert icns --output "${icns}" "${iconset}"
            DEPENDS ${inputs}
            COMMENT "Building AppIcon.icns"
            VERBATIM)

        # Listing the .icns as a source is what ties it to the target's build:
        # CMake runs the command above, then copies the result into the bundle.
        target_sources(${target} PRIVATE "${icns}")
        set_source_files_properties("${icns}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE_ICON_FILE AppIcon)

    elseif(WIN32)
        # app.ico is checked in, so a Windows build needs no image tooling.
        # Regenerate it with tools/make_ico.py when the artwork changes.
        target_sources(${target} PRIVATE "${SSSTUDIO_ASSET_DIR}/icons/app.rc")

    elseif(UNIX)
        # An ELF binary carries no icon; the desktop environment matches the
        # .desktop entry's Icon= key against the installed theme instead.
        foreach(size IN LISTS SSSTUDIO_HICOLOR_SIZES)
            install(FILES "${SSSTUDIO_ASSET_DIR}/icons/icon-${size}.png"
                    DESTINATION "share/icons/hicolor/${size}x${size}/apps"
                    RENAME sdl-shader-studio.png)
        endforeach()
        install(FILES "${SSSTUDIO_ASSET_DIR}/sdl-shader-studio.desktop"
                DESTINATION share/applications)
        # The built name has spaces in it, which Exec= handles badly; install
        # the binary under the launcher-friendly name the .desktop entry uses.
        install(PROGRAMS "$<TARGET_FILE:${target}>"
                DESTINATION bin
                RENAME sdl-shader-studio)
    endif()
endfunction()
