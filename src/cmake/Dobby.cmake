# Defines a static `dobby` target by fetching upstream jmpews/Dobby (arm64 +
# x86_64 inline hooking on macOS). Idempotent — safe to include() repeatedly.
# The macOS counterpart of Detours.cmake.
if(TARGET dobby)
    return()
endif()

include(FetchCache)
include(FetchContent)

# Dobby's CMakeLists declares a very old cmake_minimum_required that CMake 4
# rejects; allow the old policy version so the subproject configures.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)

# Dobby exposes DOBBY_DEBUG as a configure-time option rather than a
# per-configuration target property. macOS therefore uses one build tree per
# configuration (see build.sh): Debug gets Dobby's diagnostics and Release is
# built with logging/extra debug instructions disabled.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DOBBY_DEBUG ON CACHE BOOL "" FORCE)
else()
    set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    dobby
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(dobby)

# Dobby prints its Debug banner from a library constructor. Because Steam
# inherits DYLD_INSERT_LIBRARIES into helper processes, that constructor runs
# once in every helper before OpenSteamTool can apply its main-process filter.
# Suppress only the constructor translation unit's DEBUG_LOG calls; the rest of
# Dobby keeps DOBBY_DEBUG enabled, so hook/trampoline diagnostics remain intact.
# dllmain_macos.cpp reports the enabled state once through the project logger
# after steamclient.dylib loads in the main process.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_source_files_properties(
        "${dobby_SOURCE_DIR}/source/dobby.cpp"
        TARGET_DIRECTORY dobby dobby_static DobbyX
        PROPERTIES COMPILE_DEFINITIONS "LOG_LEVEL=LOG_LEVEL_INFO"
    )
endif()

# Dobby's public header lives under include/; propagate it to consumers.
if(TARGET dobby)
    target_include_directories(dobby INTERFACE "${dobby_SOURCE_DIR}/include")
endif()
