# Embed root-level steam-monitor TOML files into a generated C++ header.
# Source metadata remains outside src/; the generated header exists only in
# the build tree and is compiled into the Windows DLL and macOS dylib.

function(opensteamtool_embed_steam_monitor_data out_var)
    set(one_value_args ROOT OUTPUT_DIR)
    cmake_parse_arguments(STEAM_MONITOR "" "${one_value_args}" "" ${ARGN})

    foreach(_required ROOT OUTPUT_DIR)
        if(NOT STEAM_MONITOR_${_required})
            message(FATAL_ERROR
                "opensteamtool_embed_steam_monitor_data requires ${_required}")
        endif()
    endforeach()

    get_filename_component(_root "${STEAM_MONITOR_ROOT}" ABSOLUTE)
    set(_entries "")
    set(_entry_count 0)

    foreach(_channel IN ITEMS pattern ipc)
        file(GLOB _channel_files
            CONFIGURE_DEPENDS
            RELATIVE "${_root}/${_channel}"
            "${_root}/${_channel}/*/*.toml"
        )
        list(SORT _channel_files)

        foreach(_relative IN LISTS _channel_files)
            get_filename_component(_component "${_relative}" DIRECTORY)
            get_filename_component(_sha256 "${_relative}" NAME_WE)
            string(LENGTH "${_sha256}" _sha256_length)

            if(_component MATCHES "/" OR
               NOT _sha256_length EQUAL 64 OR
               NOT _sha256 MATCHES "^[0-9A-Fa-f]+$")
                message(FATAL_ERROR
                    "Invalid steam-monitor path '${_channel}/${_relative}'; "
                    "expected <channel>/<component>/<sha256>.toml")
            endif()

            string(TOLOWER "${_sha256}" _sha256)
            set(_path "${_root}/${_channel}/${_relative}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
            file(READ "${_path}" _body)

            string(FIND "${_body}" ")OST_META\"" _delimiter_pos)
            if(NOT _delimiter_pos EQUAL -1)
                message(FATAL_ERROR
                    "steam-monitor file '${_path}' contains the reserved raw-string delimiter")
            endif()

            string(APPEND _entries
                "    {\"${_channel}\", \"${_component}\", \"${_sha256}\", "
                "R\"OST_META(${_body})OST_META\"},\n")
            math(EXPR _entry_count "${_entry_count} + 1")
        endforeach()
    endforeach()

    if(_entry_count EQUAL 0)
        message(WARNING
            "No steam-monitor TOML files found under '${_root}'. "
            "Building with empty embedded metadata; runtime loaders will use "
            "their existing fallback or warning paths.")
    endif()

    file(MAKE_DIRECTORY "${STEAM_MONITOR_OUTPUT_DIR}")
    set(STEAM_MONITOR_ENTRIES "${_entries}")
    set(STEAM_MONITOR_ENTRY_COUNT "${_entry_count}")
    set(_output "${STEAM_MONITOR_OUTPUT_DIR}/SteamMonitorData.gen.h")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SteamMonitorData.gen.h.in"
        "${_output}"
        @ONLY
    )

    message(STATUS "Embedded ${_entry_count} steam-monitor metadata file(s)")
    set(${out_var} "${_output}" PARENT_SCOPE)
endfunction()
