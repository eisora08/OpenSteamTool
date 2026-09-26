#include "VehCommon.h"

#include "OSTPlatform/include/Memory.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
    std::vector<VehCommon::Int3Site> g_sites;
    OSTPlatform::Trap::HandlerHandle g_vehHandle = nullptr;
}

namespace VehCommon {

static bool VehHandler(OSTPlatform::Trap::ExceptionKind kind, OSTPlatform::Trap::Context& ctx) {
    if (kind == OSTPlatform::Trap::ExceptionKind::Breakpoint && OnBreakpoint(ctx)) return true;
    if (kind == OSTPlatform::Trap::ExceptionKind::SingleStep && OnSingleStep(ctx)) return true;
    return false;
}

static void EnsureHandlerInstalled() {
    if (!g_vehHandle)
        g_vehHandle = OSTPlatform::Trap::AddVectoredHandler(VehHandler);
}

static void ArmInt3(void* target) {
    OSTPlatform::Memory::WriteExecutableByte(target, 0xCC);
}

static void RestoreByte(void* target, uint8_t original) {
    OSTPlatform::Memory::WriteExecutableByte(target, original);
}

void Arm(Int3Site site) {
    EnsureHandlerInstalled();
    g_sites.push_back(site);
    ArmInt3(site.target);
}

bool HasSites() {
    return !g_sites.empty();
}

bool OnBreakpoint(OSTPlatform::Trap::Context& ctx) {
    for (auto& site : g_sites) {
        if (!site.target || !IsAt(ctx.InstructionPointer(), site.target)) continue;

        // Restore the original byte so the CPU can execute the real
        // first instruction when we resume.
        RestoreByte(site.target, site.originalByte);

        if (site.onHit) site.onHit(ctx, site);

        if (site.persistent) {
            // Set TF: CPU executes one instruction then raises SINGLE_STEP,
            // where we re-arm the int3.
            ctx.EnableSingleStep();
        }
        // For one-shot sites, leaving the byte restored permanently is the
        // desired behavior -- no further action needed.
        return true;
    }
    return false;
}

bool OnSingleStep(OSTPlatform::Trap::Context& ctx) {
    for (auto& site : g_sites) {
        if (!site.persistent || !site.target) continue;
        if (!IsPostInt3Step(ctx.InstructionPointer(), site.target)) continue;
        ArmInt3(site.target);
        return true;
    }
    return false;
}

// DisarmAll() runs while modules are being torn down: site.target can already
// point into an image that is no longer mapped, and the dereference below would
// fault instead of merely failing to restore. Probe it under SEH so an unhook
// is best-effort rather than fatal.
#if defined(_WIN32)
static void SafeRestoreSite(uint8_t* target, uint8_t originalByte) {
    __try {
        if (target && *target == 0xCC) {
            RestoreByte(target, originalByte);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Module might already be unmapped, safely ignore
    }
}
#else
static void SafeRestoreSite(uint8_t* target, uint8_t originalByte) {
    if (target && *target == 0xCC) {
        RestoreByte(target, originalByte);
    }
}
#endif

void DisarmAll() {
    for (auto& site : g_sites) {
        SafeRestoreSite(site.target, site.originalByte);
    }
    g_sites.clear();
}

void RemoveHandler() {
    if (g_vehHandle) {
        OSTPlatform::Trap::RemoveVectoredHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}

} // namespace VehCommon
