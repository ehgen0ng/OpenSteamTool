#include "include/RemoteProcess.h"

// macOS performs library insertion by re-signing Steam and using a launcher with
// DYLD_INSERT_LIBRARIES, not by writing into a live remote process the way the
// Windows backend does (OpenProcess + VirtualAllocEx + CreateRemoteThread). There
// is therefore no in-process remote injector here. These stubs keep the shared
// Pipe/Injection feature linking; at runtime GetArchitecture reports Unknown, which
// makes Injection::Apply select no library and return before InjectLibrary is ever
// reached. InjectLibrary still reports a failure status for completeness.
namespace OSTPlatform::RemoteProcess {

const char* ToString(Architecture architecture) {
    switch (architecture) {
    case Architecture::Unknown: return "Unknown";
    case Architecture::X86: return "X86";
    case Architecture::X64: return "X64";
    }
    return "Unknown";
}

const char* ToString(InjectStatus status) {
    switch (status) {
    case InjectStatus::Ok: return "Ok";
    case InjectStatus::OpenProcessFailed: return "OpenProcessFailed";
    case InjectStatus::UnknownArchitecture: return "UnknownArchitecture";
    case InjectStatus::AllocFailed: return "AllocFailed";
    case InjectStatus::WriteFailed: return "WriteFailed";
    case InjectStatus::ResolveLoadLibraryFailed: return "ResolveLoadLibraryFailed";
    case InjectStatus::CreateThreadFailed: return "CreateThreadFailed";
    case InjectStatus::WaitFailed: return "WaitFailed";
    case InjectStatus::RemoteLoadFailed: return "RemoteLoadFailed";
    }
    return "RemoteLoadFailed";
}

Architecture GetArchitecture(uint32_t) {
    // No cross-process inspection: report Unknown so Injection::Apply bails.
    return Architecture::Unknown;
}

InjectStatus InjectLibrary(uint32_t, const std::filesystem::path&) {
    // Unreachable in practice (Injection::Apply returns on Unknown architecture);
    // report a failure so any future caller treats it as "not injected".
    return InjectStatus::RemoteLoadFailed;
}

} // namespace OSTPlatform::RemoteProcess
