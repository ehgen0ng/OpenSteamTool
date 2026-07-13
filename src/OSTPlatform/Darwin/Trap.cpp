#include "include/Trap.h"

#include <dobby.h>

// macOS Trap backend.
//
// The Windows Trap is a Vectored-Exception-Handler dispatcher driving 0xCC
// soft-breakpoints, reading arguments off the fastcall registers/stack. On
// macOS, VehCommon_macos.cpp's Dobby instrument callback hands its raw
// DobbyRegisterContext* straight to this Context (nativeContext_), so
// Argument() reads the AArch64 AAPCS integer/pointer argument registers
// (x0-x7) instead. AddVectoredHandler/RemoveVectoredHandler stay stubs: there
// is no VEH-style dispatch on macOS (see VehCommon_macos.cpp OnBreakpoint).
namespace OSTPlatform::Trap {

uint64_t Context::InstructionPointer() const {
    // Not reached on macOS — VehCommon_macos.cpp's InstrumentThunk dispatches
    // straight to site.onHit without going through OnBreakpoint's IsAt() check.
    return 0;
}

uint64_t Context::Argument(int index) const {
    // AAPCS64: the first 8 integer/pointer arguments are x0-x7 (index is 1-based).
    if (!nativeContext_ || index < 1 || index > 8) return 0;
    auto* ctx = static_cast<DobbyRegisterContext*>(nativeContext_);
    return ctx->general.x[index - 1];
}

void Context::EnableSingleStep() {}

HandlerHandle AddVectoredHandler(Handler) { return nullptr; }
void RemoveVectoredHandler(HandlerHandle) {}

}  // namespace OSTPlatform::Trap
