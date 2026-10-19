// ============================================================================
// src/platform/posix/os_posix.cpp -- Platform:: process-level services over POSIX
// ============================================================================
// The POSIX half of src/platform/common/platform_os.h (Phase D.4 THREADS E.9).
//
// IsReadablePage (a diagnostic probe, not retail): the page must be MAPPED and
// readable. A mapped page may still be PROT_NONE (MemoryAllocator's reserved but
// uncommitted ranges, allocator_posix.cpp), so the probe reads the mapping's
// permissions from /proc/self/maps rather than asking whether it is mapped.
// ============================================================================

#include "platform_os.h"

#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif
#include <unistd.h>
#include <cstdio>

namespace Platform {

namespace {

#if !defined(__APPLE__)
uintptr_t PageBase(const void* p) {
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    return reinterpret_cast<uintptr_t>(p) & ~(page - 1);  // CAST-OK: ptr_int
}

#endif
}  // namespace

bool IsReadablePage(const void* p) {
#if defined(__APPLE__)
    MemoryRegion region{};
    return QueryMemoryRegion(p, region) && region.readable;
#else
    const uintptr_t base = PageBase(p);
    // /proc/self/maps: "start-end perms ...", hex addresses, sorted.
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;
    char line[512];
    bool readable = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (base >= lo && base < hi) {
            readable = perms[0] == 'r';
            break;
        }
    }
    std::fclose(maps);
    return readable;
#endif
}

bool QueryMemoryRegion(const void* p, MemoryRegion& out) {
#if defined(__APPLE__)
    mach_vm_address_t address = reinterpret_cast<uintptr_t>(p);
    const auto requested = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto rc = mach_vm_region(mach_task_self(), &address, &size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (rc != KERN_SUCCESS || requested < address) return false;
    out.base = address; out.size = size;
    out.readable = (info.protection & VM_PROT_READ) != 0;
    out.executable = (info.protection & VM_PROT_EXECUTE) != 0;
    out.committed = out.accessible = info.protection != VM_PROT_NONE;
    out.image = info.shared;
    return true;
#else
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);  // CAST-OK: ptr_int
    // /proc/self/maps: "start-end perms offset dev inode path", hex addresses, sorted.
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long lo = 0, hi = 0, offset = 0, inode = 0;
        char perms[8] = {};
        char dev[16] = {};
        if (std::sscanf(line, "%llx-%llx %7s %llx %15s %llu", &lo, &hi, perms, &offset, dev,
                        &inode) != 6)
            continue;
        if (a < lo || a >= hi) continue;
        out.base       = lo;
        out.size       = hi - lo;
        // A PROT_NONE mapping is MemoryAllocator's reserved-not-committed range
        // (allocator_posix.cpp): MEM_RESERVE, not MEM_COMMIT.
        out.committed  = perms[0] == 'r' || perms[1] == 'w' || perms[2] == 'x';
        out.accessible = out.committed;
        out.readable   = perms[0] == 'r';
        out.executable = perms[2] == 'x';
        out.image      = inode != 0;
        found = true;
        break;
    }
    std::fclose(maps);
    return found;
#endif
}

namespace {

#if !defined(__APPLE__)
int FirstObjectBase(struct dl_phdr_info* info, size_t, void* data) {
    *static_cast<uintptr_t*>(data) = info->dlpi_addr;  // CAST-OK: posix
    return 1;   // the first entry is the main program
}

#endif
}  // namespace

uintptr_t ExecutableImageBase() {
#if defined(__APPLE__)
    return reinterpret_cast<uintptr_t>(_dyld_get_image_header(0));
#else
    uintptr_t base = 0;
    dl_iterate_phdr(&FirstObjectBase, &base);
    return base;
#endif
}

// ExitProcess: platform_os.h says why error.cpp's Windows arm still declares the
// kernel32 import itself.
void ExitProcess(uint32_t exitCode) {
    std::fflush(nullptr);
    _exit(static_cast<int>(exitCode));
}

}  // namespace Platform
