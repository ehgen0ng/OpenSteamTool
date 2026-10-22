cmake_minimum_required(VERSION 3.20)
project(OpenSteamTool VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(OPENSTEAMTOOL_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded into OpenSteamTool.dll")

# Allow CMAKE_MSVC_RUNTIME_LIBRARY to control runtime selection for all targets,
# including dependencies pulled in via FetchContent.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Static MSVC runtime everywhere, so the resulting DLL has no extra runtime
# dependencies. Must be set BEFORE FetchContent_MakeAvailable so the fetched
# deps (Lua, Detours, spdlog) inherit it.
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(WIN32 OR APPLE)
    include(SteamMonitorData)
    opensteamtool_embed_steam_monitor_data(STEAM_MONITOR_DATA_HEADER
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../steam-monitor"
        OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
endif()

if(APPLE)
    # Dobby's debug mode is a configure-time option, so one Dobby target cannot
    # differ between Debug and Release in a multi-config build tree. Keep the
    # two configurations isolated under build/macos/{Debug,Release}; build.sh
    # drives both trees with the same workflow as build.bat on Windows.
    get_property(OPENSTEAMTOOL_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(OPENSTEAMTOOL_MULTI_CONFIG)
        message(FATAL_ERROR
            "macOS requires a single-config generator so Dobby Debug/Release settings stay isolated. "
            "Run ./build.sh (or configure separate Debug and Release build directories).")
    endif()

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "macOS build configuration" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND
       NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "macOS supports only Debug or Release builds (got '${CMAKE_BUILD_TYPE}').")
    endif()
    message(STATUS "OpenSteamTool macOS configuration: ${CMAKE_BUILD_TYPE}")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

    # Log.h includes this generated header in both Debug and Release.
    include(LogMacros)

    # Shared runtime dependencies.
    include(Lua)
    include(Tomlplusplus)
    include(Protobuf)
    include(Spdlog)

    # SteamDiagnostics.cpp #includes the generated OpenSteamToolBuildInfo.h for
    # OPENSTEAMTOOL_VERSION; generate it the same way the Windows path does.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenSteamToolBuildInfo.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/OpenSteamToolBuildInfo.h"
        @ONLY
    )

    # Generate IPC metadata with the same host tool used by Windows.
    set(OPENSTEAMTOOL_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
    add_subdirectory("${OPENSTEAMTOOL_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
    include("${OPENSTEAMTOOL_TOOLS_DIR}/cmake/IPCCodegen.cmake")
    opensteamtool_add_ipc_codegen(IPC_GEN
        IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd"
        CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    # Match Windows protobuf variants: full in Debug, lite in Release.
    set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(
        OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
               "${PROTO_GEN_DIR}/steam_messages.pb.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
        COMMAND $<TARGET_FILE:protoc>
            "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto" protoc
        COMMENT "Generating protobuf C++ sources (${CMAKE_BUILD_TYPE})"
        VERBATIM
    )

    add_subdirectory(OSTPlatform)

    add_library(OpenSteamToolMacOS SHARED
        # macOS entry point and pattern backend
        dllmain_macos.cpp
        Utils/SteamMetadata/PatternLoader_macos.cpp
        "${STEAM_MONITOR_DATA_HEADER}"

        # Configuration, logging, and hook support
        Utils/Config/LuaConfig.cpp
        Utils/Config/RuntimePaths.cpp
        Utils/Logging/Log_macos.cpp
        Utils/HookSupport/VehCommon_macos.cpp

        # Platform-specific hooks
        Hook/Hooks_Package_macos.cpp
        Utils/SteamMetadata/StatsClient.cpp
        Utils/Tickets/AppTicket.cpp
        Hook/Hooks_SteamUI_macos.cpp

        # Shared hook modules
        Hook/Hooks_Manifest.cpp
        Hook/Hooks_Decryption.cpp
        Hook/Hooks_CallBack.cpp
        Hook/Hooks_KeyValues.cpp
        Hook/Hooks_Misc.cpp
        Hook/PendingAPICalls.cpp
        Hook/Hooks_NetPacket.cpp
        "${PROTO_GEN_DIR}/steam_messages.pb.cc"

        # Configuration and metadata services
        Utils/Config/Config.cpp
        Utils/Config/ConfigFileWatcher_macos.cpp
        Utils/Config/LuaFileWatcher_macos.cpp
        Utils/SteamMetadata/SteamDiagnostics_macos.cpp
        Utils/SteamMetadata/ManifestClient.cpp
        Utils/SteamMetadata/IPCLoader_macos.cpp

        # CloudRedirect host and shared pipe subsystem
        Utils/CloudRedirect/CloudRedirectHost.cpp
        Pipe/PipeManager.cpp
        Pipe/ProcessInspector.cpp
        Pipe/Features/Injection/Injection.cpp
        Pipe/Features/DenuvoAuth/DenuvoAuth_macos.cpp

        # IPC and hook aggregation
        Hook/Hooks_IPC_macos.cpp
        Hook/Hooks_IPC_ISteamUser.cpp
        Hook/Hooks_IPC_ISteamUtils.cpp
        Hook/HookManager.cpp

        "${IPC_GEN}"
    )

    target_include_directories(OpenSteamToolMacOS PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_link_libraries(OpenSteamToolMacOS PRIVATE
        OSTPlatform
        lua_static
        dobby_static
        tomlplusplus::tomlplusplus
        $<$<CONFIG:Debug>:libprotobuf>
        $<$<CONFIG:Release>:libprotobuf-lite>
        $<$<CONFIG:Debug>:spdlog::spdlog>
    )

    # Hook/HookMacros.h stamps __fastcall onto every hooked signature; arm64
    # clang correctly ignores the calling convention but warns. Silence it
    # here rather than touching the shared header.
    target_compile_options(OpenSteamToolMacOS PRIVATE
        -Wno-ignored-attributes
    )

    # Match Windows: Debug compiles the shared spdlog backend and every LOG_*
    # call in *_macos.cpp; Release turns those calls into no-ops.
    target_compile_definitions(OpenSteamToolMacOS PRIVATE
        $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
        $<$<CONFIG:Debug>:OPENSTEAMTOOL_DOBBY_DEBUG>
    )

    # Keep the CMake target name (OpenSteamToolMacOS) but emit the artifact as
    # libOpenSteamTool.dylib, matching the Windows OpenSteamTool naming and what
    # steam-inject.sh / dllmain_macos.cpp's GetLoaded() expect.
    set_target_properties(OpenSteamToolMacOS PROPERTIES OUTPUT_NAME OpenSteamTool)

    # Windows Debug emits a PDB. The macOS equivalent is a dSYM bundle next to
    # the Debug dylib, generated after every successful Debug link.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        find_program(DSYMUTIL_EXECUTABLE dsymutil REQUIRED)
        add_custom_command(TARGET OpenSteamToolMacOS POST_BUILD
            COMMAND "${DSYMUTIL_EXECUTABLE}"
                    "$<TARGET_FILE:OpenSteamToolMacOS>"
                    -o "$<TARGET_FILE:OpenSteamToolMacOS>.dSYM"
            COMMENT "Generating libOpenSteamTool.dylib.dSYM"
            VERBATIM
        )
    endif()

    return()
endif()

if(NOT WIN32)
    message(STATUS "OpenSteamTool: no targets on non-Windows platforms; skipping.")
    return()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenSteamToolBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/OpenSteamToolBuildInfo.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependency recipes (FetchContent-backed, cached at <repo>/.deps).
# ---------------------------------------------------------------------------
set(OPENSTEAMTOOL_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
include(Lua)
include(Spdlog)
include(Protobuf)
include(Tomlplusplus)
include(LogMacros)

# Build the host code generators (ipc_codegen) in their own binary subtree so
# their executables never land in the shipped output directory. EXCLUDE_FROM_ALL
# means only the tools actually depended on (ipc_codegen) get built here.
add_subdirectory("${OPENSTEAMTOOL_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
include("${OPENSTEAMTOOL_TOOLS_DIR}/cmake/IPCCodegen.cmake")

set(IPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd")
# Per-config output dir, mirroring the protobuf recipe below: a Multi-Config
# build invokes the generator once per config, and a single shared output file
# would ping-pong between the Release/Debug graphs and force needless rebuilds.
opensteamtool_add_ipc_codegen(IPC_GEN
    IDL "${IPC_IDL}"
    CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Protobuf code generation — one variant per config, from the same .proto:
#
#   Debug   → full Message     (protoc --cpp_out)       → links libprotobuf
#   Release → lite MessageLite (protoc --cpp_out=lite:) → links libprotobuf-lite
#
# Output lands in the per-config generated/ dir (same root as the IPC header),
# so each config owns its own files. A shared output path would ping-pong
# between the Release/Debug graphs (the command embeds the config-specific
# protoc path) and force a regen + recompile on every config switch.
# ---------------------------------------------------------------------------
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf C++ sources"
    VERBATIM
)

# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns the OS-specific backends.
# Its build is defined in its own module CMakeLists so this top-level file stays
# focused on host-level products (the DLLs) and code generation.
# ---------------------------------------------------------------------------
add_subdirectory(OSTPlatform)

# ---------------------------------------------------------------------------
# Windows host artifacts: the injected hook DLL and the two loader-hijack DLLs.
# All Windows-only by construction (PE DLL hijacking + Win32 hooking).
# ---------------------------------------------------------------------------

# OpenSteamTool — the hook DLL injected into Steam (always 64-bit).
add_library(OpenSteamTool SHARED
    dllmain.cpp

    # Shared utilities
    Utils/Tickets/AppTicket.cpp
    Utils/Config/Config.cpp
    Utils/Config/ConfigFileWatcher.cpp
    Utils/Config/LuaConfig.cpp
    Utils/Config/LuaFileWatcher.cpp
    Utils/Config/RuntimePaths.cpp
    Utils/CloudRedirect/CloudRedirectHost.cpp
    Utils/HookSupport/VehCommon.cpp
    Utils/Logging/Log.cpp
    Utils/SteamMetadata/IPCLoader.cpp
    Utils/SteamMetadata/ManifestClient.cpp
    Utils/SteamMetadata/PatternLoader.cpp
    Utils/SteamMetadata/RemoteToml.cpp
    Utils/SteamMetadata/StatsClient.cpp
    Utils/SteamMetadata/SteamDiagnostics.cpp
    "${STEAM_MONITOR_DATA_HEADER}"

    # Pipe subsystem: generic handshake registry + ProcessInspector core, plus
    # self-contained features that own their primitives.
    Pipe/PipeManager.cpp
    Pipe/ProcessInspector.cpp
    Pipe/Features/DenuvoAuth/DenuvoAuth.cpp
    Pipe/Features/DenuvoAuth/ProtectionScan.cpp
    Pipe/Features/Injection/Injection.cpp

    # Generated IPC message structs (depends on IPCMessages.steamd via add_custom_command)
    "${IPC_GEN}"

    # Per-category hook modules
    Hook/HookManager.cpp
    Hook/Hooks_CallBack.cpp
    Hook/Hooks_Decryption.cpp
    Hook/Hooks_IPC.cpp
    Hook/Hooks_IPC_ISteamUser.cpp
    Hook/Hooks_IPC_ISteamUtils.cpp
    Hook/Hooks_KeyValues.cpp
    Hook/Hooks_Manifest.cpp
    Hook/Hooks_Misc.cpp
    Hook/Hooks_NetPacket.cpp
    Hook/PendingAPICalls.cpp
    Hook/Hooks_SteamUI.cpp
    Hook/Hooks_Package.cpp

    # protobuf generated sources (full in Debug, lite in Release)
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
)

# Header search path — per-config include directory
target_include_directories(OpenSteamTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)

target_link_libraries(OpenSteamTool PRIVATE
    OSTPlatform
    lua_static
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
)

# Logging is compiled in only for Debug; Release reduces LOG_* to no-ops.
target_compile_definitions(OpenSteamTool PRIVATE
    $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
)

# ---------------------------------------------------------------------------
# dwmapi.dll hijack — small loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(dwmapi SHARED
    dwmapi/dwmapi.cpp
    Loader/LoaderBootstrap.cpp
    Utils/Config/RuntimePaths.cpp
)

# ---------------------------------------------------------------------------
# xinput1_4.dll hijack — secondary loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(xinput1_4 SHARED
    xinput1_4/xinput1_4.cpp
    xinput1_4/xinput1_4.def
    Loader/LoaderBootstrap.cpp
    Utils/Config/RuntimePaths.cpp
)
