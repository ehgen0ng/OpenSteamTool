#include "include/Memory.h"

#include "include/Log.h"

#include <libkern/OSCacheControl.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <cstdint>

namespace OSTPlatform::Memory {
namespace {

size_t ImageSize(const mach_header_64* header) {
    if (!header || header->magic != MH_MAGIC_64) return 0;

    size_t maxEnd = 0;
    auto* command = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if (segment->filesize > 0) {
                const size_t end = static_cast<size_t>(segment->vmaddr + segment->vmsize);
                if (end > maxEnd) maxEnd = end;
            }
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }
    return maxEnd;
}

} // namespace

std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module) {
    auto* header = static_cast<mach_header_64*>(module);
    const size_t size = ImageSize(header);
    if (!header || size == 0) return std::nullopt;

    return ModuleImage{
        reinterpret_cast<uint8_t*>(header),
        size,
    };
}

bool WriteExecutableByte(void* target, uint8_t value) {
    if (!target) {
        OSTP_LOG_WARN("WriteExecutableByte: target is null");
        return false;
    }

    const mach_port_t task = mach_task_self();
    const auto address = reinterpret_cast<mach_vm_address_t>(target);

    // arm64 macOS enforces W^X: a code page mapped R+X cannot simply be flipped
    // to R+W in place. VM_PROT_COPY forces a private copy-on-write mapping of the
    // containing page so the byte can be written even when the original mapping
    // is shared/executable. mach_vm_protect rounds [address, address+1) to the
    // enclosing page, so a size of 1 covers the whole page holding the target.
    kern_return_t kr = mach_vm_protect(
        task, address, 1, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        OSTP_LOG_WARN("WriteExecutableByte(target=0x{:X}) mach_vm_protect(RW|COPY) failed (kr={})",
                      static_cast<uint64_t>(address), static_cast<int>(kr));
        return false;
    }

    *static_cast<uint8_t*>(target) = value;

    kr = mach_vm_protect(task, address, 1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        OSTP_LOG_WARN("WriteExecutableByte(target=0x{:X}) mach_vm_protect(restore RX) failed (kr={})",
                      static_cast<uint64_t>(address), static_cast<int>(kr));
        return false;
    }

    // The write went through the data cache; invalidate the instruction cache so
    // the CPU re-fetches the patched byte (a no-op flush on a byte we just wrote,
    // but required for correctness on arm64's split caches).
    sys_icache_invalidate(target, 1);
    return true;
}

} // namespace OSTPlatform::Memory
