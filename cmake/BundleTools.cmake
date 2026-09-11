# Shipping the shader toolchain with the application.
#
# The compiler is reached as a command line tool rather than a linked library
# (see Dependencies.cmake for why), which means the built application depends on
# a program that lives somewhere else on the machine that built it. That is fine
# for a developer and useless for anyone handed the result: the path baked in at
# configure time is absolute, and on another machine it is either missing or, far
# worse, a different version of the toolchain that happens to sit there.
#
# So the tool and the libraries it loads are copied in beside the application.
# The layout is mirrored rather than flattened - `<root>/bin/shadercross` with
# `<root>/lib/*` - because the tool finds its libraries through an rpath of
# `@executable_path/../lib`, and flattening them would break that. Mirroring also
# keeps the SDL3 the toolchain ships out of the directory holding the
# application's own executable, which is the whole reason the tool is a separate
# process in the first place.

# Runtime files the toolchain needs, as a flat list of "<relative dir>|<file>"
# pairs so a caller can rebuild the layout wherever it wants it.
function(ssstudio_collect_tool_files out_var)
    set(pairs "")
    if(NOT SSSTUDIO_SHADERCROSS_EXECUTABLE OR NOT EXISTS "${SSSTUDIO_SHADERCROSS_EXECUTABLE}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    list(APPEND pairs "bin|${SSSTUDIO_SHADERCROSS_EXECUTABLE}")

    get_filename_component(bin_dir "${SSSTUDIO_SHADERCROSS_EXECUTABLE}" DIRECTORY)
    get_filename_component(prefix "${bin_dir}" DIRECTORY)

    # Only the shared libraries: the static archives, the CMake package and the
    # pkg-config files are all build-time things, and copying them would triple
    # the size of the application for no runtime benefit.
    set(lib_globs
        "${prefix}/lib/*.dylib" "${prefix}/lib/*.so" "${prefix}/lib/*.so.*"
        "${prefix}/bin/*.dll" "${bin_dir}/*.dylib" "${bin_dir}/*.so" "${bin_dir}/*.so.*")
    file(GLOB libraries ${lib_globs})
    foreach(library ${libraries})
        # Windows keeps its DLLs beside the executable; everything else keeps
        # them one directory over, which is what the rpath above points at.
        if(library MATCHES "\\.dll$")
            list(APPEND pairs "bin|${library}")
        else()
            list(APPEND pairs "lib|${library}")
        endif()
    endforeach()

    set(${out_var} "${pairs}" PARENT_SCOPE)
endfunction()

# Copies the toolchain next to `target` after it is built, and installs it
# alongside. `subdir` is where the tools go relative to the application.
function(ssstudio_bundle_tools target)
    ssstudio_collect_tool_files(tool_files)
    if(NOT tool_files)
        message(STATUS "No shadercross tool to bundle; the app will look on PATH at runtime")
        return()
    endif()

    # A macOS application keeps everything that is not the executable under
    # Contents/Resources. Elsewhere the tools sit in a directory beside the
    # program, which is what bundled_tool_roots() looks for.
    # The target property, not the CMAKE_MACOSX_BUNDLE variable: the variable is
    # only the default for targets that do not say for themselves, and this one
    # does, so reading it would quietly put the tools in the wrong place.
    get_target_property(is_bundle ${target} MACOSX_BUNDLE)
    if(APPLE AND is_bundle)
        set(root "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/tools/shadercross")
    else()
        set(root "$<TARGET_FILE_DIR:${target}>/tools/shadercross")
    endif()

    set(commands "")
    foreach(pair ${tool_files})
        string(FIND "${pair}" "|" split)
        string(SUBSTRING "${pair}" 0 ${split} where)
        math(EXPR after "${split} + 1")
        string(SUBSTRING "${pair}" ${after} -1 file)
        list(APPEND commands
             COMMAND ${CMAKE_COMMAND} -E make_directory "${root}/${where}"
             # copy_if_different rather than copy: these are tens of megabytes,
             # and an incremental build should not move them every time.
             COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "${root}/${where}/")
    endforeach()

    add_custom_command(TARGET ${target} POST_BUILD ${commands}
                       COMMENT "Bundling the shader toolchain with ${target}"
                       VERBATIM)
endfunction()

# The same files, for `cmake --install`. Kept separate from the post-build copy
# because an install tree lays the application out differently from a build tree.
function(ssstudio_install_tools destination)
    ssstudio_collect_tool_files(tool_files)
    foreach(pair ${tool_files})
        string(FIND "${pair}" "|" split)
        string(SUBSTRING "${pair}" 0 ${split} where)
        math(EXPR after "${split} + 1")
        string(SUBSTRING "${pair}" ${after} -1 file)
        # USE_SOURCE_PERMISSIONS keeps the executable bit on the tool itself; an
        # installed shadercross that cannot be run is a confusing way to fail.
        install(PROGRAMS "${file}" DESTINATION "${destination}/${where}")
    endforeach()
endfunction()
