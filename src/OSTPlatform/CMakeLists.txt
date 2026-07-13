# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns OS-specific backends.
# Public headers must NOT expose Win32, WinHttp, registry, BCrypt, Mach-O,
# or remote-process primitives — those stay behind this library.
# ---------------------------------------------------------------------------

add_library(OSTPlatform STATIC
    # Platform-agnostic
    Numbers.cpp
    Log.cpp
)

if(WIN32)
    include(Detours)

    target_sources(OSTPlatform PRIVATE
        Encoding.cpp
        Windows/Dialog.cpp
        Windows/Detour.cpp
        Windows/DirectoryWatch.cpp
        Windows/DynamicLibrary.cpp
        Windows/Hash.cpp
        Windows/Http.cpp
        Windows/Memory.cpp
        Windows/ByteSearch.cpp
        Windows/PE.cpp
        Windows/Process.cpp
        Windows/RemoteProcess.cpp
        Windows/SteamCredentialStore.cpp
        Windows/Thread.cpp
        Windows/Trap.cpp
    )

    target_link_libraries(OSTPlatform PRIVATE
        winhttp
        Bcrypt
        detours
    )
elseif(APPLE)
    include(Dobby)

    target_sources(OSTPlatform PRIVATE
        Darwin/Dialog.cpp
        Darwin/Detour.cpp
        Darwin/DirectoryWatch.cpp
        Darwin/DynamicLibrary.cpp
        Darwin/Encoding.cpp
        Darwin/Hash.cpp
        Darwin/Http.cpp
        Darwin/Memory.cpp
        Darwin/SteamCredentialStore.cpp
        Darwin/Thread.cpp
        Darwin/Trap.cpp
        Darwin/ByteSearch.cpp
        Darwin/Process.cpp
        Darwin/RemoteProcess.cpp
    )

    target_link_libraries(OSTPlatform PRIVATE
        dobby
        "-lcurl"                  # Http.cpp uses libcurl (macOS-bundled)
        "-framework CoreServices" # DirectoryWatch.cpp uses FSEvents
    )
else()
    message(FATAL_ERROR "OSTPlatform: unsupported platform")
endif()

target_include_directories(OSTPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # src/ — for cross-module private headers
)

# Logging is compiled in only for Debug; Release reduces OSTP_LOG_* to no-ops.
target_compile_definitions(OSTPlatform PRIVATE
    $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
)
