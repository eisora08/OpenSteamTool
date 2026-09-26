#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/HookSupport/VehCommon.h"
#include "dllmain.h"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
    // ── Resolve-only functions ─────────────────────────────────────
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    // ── VEH-captured functions (one-shot int3) ───────────────────────────────
    // On int3 hit, ctx->Rcx is stored to the named output variable.
    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t,      g_steamEngine,    void*);
    CAPTURE_THIS_FUNC(GetAppDataFromAppInfo,  int64,        g_pCAppInfoCache, void*, AppId_t, const char*, uint8*, int32);

    // Assumes one game at a time.  Set by SpawnProcess VEH when -onlinefix
    // is detected; cleared when a non-onlinefix game launches.
    std::atomic<AppId_t> g_OnlineFixRealAppId{0};
    // True once the game starts SteamNetworkingSockets P2P (see GetAppID handler).
    std::atomic<bool> g_NetworkingSocketsActive{false};
    // Set by -realappid on the same command line. Stores the real AppId
    // for the -realappid override; see ShouldReportOnlineFixAppId.
    std::atomic<AppId_t> g_OnlineFixRealAppIdOverride{0};
    std::unordered_map<AppId_t, std::string> g_GameNameCache;
    std::mutex g_GameNameCacheMutex;


    // ── SpawnProcess interception ────────────────────────────────────────────
    // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
    //                    pGameID, ...)
    // arg1=pCUser, arg2=pExePath, arg3=pCommandLine, arg4=pWorkingDir
    // arg5=pGameID (CGameID*; low 24 bits = AppId)
    static void OnSpawnProcessHit(OSTPlatform::Trap::Context& ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);

        if (LuaConfig::HasDepot(appId) && cmdLine && strstr(cmdLine, "-onlinefix"))
        {
            g_OnlineFixRealAppId = appId;
            g_NetworkingSocketsActive = false;
            if (strstr(cmdLine, "-realappid")) {
                g_OnlineFixRealAppIdOverride = appId;
                LOG_MISC_INFO("SpawnProcess: appid {} -> {}, realappid override, cmd=\"{}\"",
                              appId, kOnlineFixAppId, cmdLine);
            } else {
                g_OnlineFixRealAppIdOverride = 0;
                LOG_MISC_INFO("SpawnProcess: appid {} -> {}, cmd=\"{}\"",
                              appId, kOnlineFixAppId, cmdLine);
            }
            pGameID->SetAppID(kOnlineFixAppId);
        } else {
            g_OnlineFixRealAppId = 0;
            g_OnlineFixRealAppIdOverride = 0;
        }
    }

    // ── SteamController_OptedInMask ──────────────────────────────────────────
    // Called by CUser_BuildSpawnEnvBlock with pGameID's appid to
    // compute EnableConfiguratorSupport and the SDL_* env vars.
    // With 480 the spawned game inherits Spacewar's Steam Input
    // opt-in and gameoverlayrenderer hijacks the XInput stream.
    HOOK_FUNC(OptedInMask, int64,void* pThis, AppId_t appId)
    {
        if (appId == kOnlineFixAppId && g_OnlineFixRealAppId) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}",appId, g_OnlineFixRealAppId.load());
            appId = g_OnlineFixRealAppId;
        }
        return oOptedInMask(pThis, appId);
    }

    // ── CUser_BuildSpawnEnvBlock ─────────────────────────────────────────────
    // pOverlayCGameID drives SteamOverlayGameId, which the in-game
    // overlay reads for screenshot tags, community URLs, and asset
    // selection.  pCGameID drives SteamGameId / SteamAppId; leave it
    // at 480 so the in-game ownership bypass holds.
    HOOK_FUNC(BuildSpawnEnvBlock, int64,
              void* pThis, CGameID* pCGameID, void* a3, void* env,
              CGameID* pOverlayCGameID, void* a6, int a7,
              void* a8, void* a9, unsigned int a10, char a11)
    {
        AppId_t overlayAppId = g_OnlineFixRealAppIdOverride
            ? g_OnlineFixRealAppIdOverride.load()
            : g_OnlineFixRealAppId.load();
        if (overlayAppId && pOverlayCGameID
            && pOverlayCGameID->AppID(true) == kOnlineFixAppId) 
        {
            LOG_MISC_INFO("BuildSpawnEnvBlock: SetAppID in OverlayCGameID {} -> {}",
                          pOverlayCGameID->AppID(true), overlayAppId);
            pOverlayCGameID->SetAppID(overlayAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                    pOverlayCGameID, a6, a7,
                                    a8, a9, a10, a11);
    }

    // CAppInfoCache::GetOrAddAppData
    // The injected package keeps Lua-provided ids in PackageInfo::AppIdVec.
    // Some of those ids can actually be depot ids, but we cannot trust the
    // Lua config to classify app ids and depot ids for us. In offline mode,
    // depot ids usually have only placeholder appinfo data. That blocks
    // CClientAppManager_ProcessPendingLicenseUpdates, because it waits for
    // every AppIdVec entry to have resolved appinfo unless the entry has been
    // marked as a known-unknown id by the PICS path. For injected ids that
    // still have placeholder appinfo, set skip_flag so Steam treats them like
    // PICS unknown_appids instead of keeping the license update pending.
    HOOK_FUNC(GetOrAddAppData,CAppData*,void* pCache, AppId_t appId,bool bCreate)
    {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bCreate);
        // LOG_MISC_TRACE("GetOrAddAppData: appId={} bCreate={} -> pData={}", appId, bCreate, pData ? pData->DebugString() : "null");
        // TODO: find a more robust way
        if (LuaConfig::HasDepot(appId, false) && pData && !bCreate && pData->IsUnresolvedAppInfo()) {
            LOG_MISC_DEBUG("GetOrAddAppData: Marking appId {} as skip_flag=true to bypass license update blocking", appId);
            pData->bSkipFlag = true;
        }
        return pData;
    }
}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);

        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_CAPTURE_C(GetAppDataFromAppInfo);

        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        // INSTALL_HOOK_C(GetOrAddAppData);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        // UNINSTALL_HOOK(GetOrAddAppData);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) {
            LOG_MISC_WARN("GetAppIDForCurrentPipeWrap called before capture — returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId={}", appid);
        }
        return appid;
    }

    
    AppId_t ResolveAppId() {
        if (g_OnlineFixRealAppIdOverride) return g_OnlineFixRealAppIdOverride;
        if (g_OnlineFixRealAppId) return g_OnlineFixRealAppId;
        return GetAppIDForCurrentPipeWrap();
    }

    bool IsOnlineFixActive() {
        return g_OnlineFixRealAppId != 0;
    }

    void NotifyNetworkingSocketsUsed() {
        if (g_OnlineFixRealAppId && !g_NetworkingSocketsActive) {
            g_NetworkingSocketsActive = true;
            LOG_MISC_INFO("NetworkingSockets active: GetAppID now reports 480 for cert match");
        }
    }

    bool ShouldReportOnlineFixAppId() {
        if (g_OnlineFixRealAppIdOverride) return false;
        return g_OnlineFixRealAppId != 0 && g_NetworkingSocketsActive;
    }

    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut)
    {
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            if(updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        LOG_MISC_WARN("EnsureBufferCapacity: oCUtlBufferEnsureCapacity not resolved");
        return false;
    }

    // ── Game name ────────────────────────────────────────────────
    constexpr size_t kGameNameCacheMax = 512;
    std::string GetGameNameByAppID(AppId_t appId)
    {
        {
            std::lock_guard<std::mutex> lock(g_GameNameCacheMutex);
            auto it = g_GameNameCache.find(appId);
            if (it != g_GameNameCache.end()) return it->second;
        }

        std::string name;

        if (CAPTURE_READY(GetAppDataFromAppInfo)) {
            char buf[256] = {};
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1) {
                // Steam may report a length beyond the buffer it was given;
                // clamp so name.assign() cannot read past buf.
                const size_t copyLen = (std::min)(static_cast<size_t>(len - 1), sizeof(buf) - 1);
                name.assign(buf, copyLen);
            }
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, name);
        {
            std::lock_guard<std::mutex> lock(g_GameNameCacheMutex);
            if (g_GameNameCache.size() >= kGameNameCacheMax)
                g_GameNameCache.clear();
            g_GameNameCache[appId] = name;
        }
        return name;
    }

}
