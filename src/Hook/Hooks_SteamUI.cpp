#include "Hooks_SteamUI.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    // Apps currently downloading/updating (AppStateFlags UpdateRunning/Started).
    // Maintained here on the UI thread; read from other threads under the lock.
    std::mutex g_activeDlMutex;
    std::unordered_set<AppId_t> g_activeDl;

    // Apps with update work in flight (download, staging, verify, paused
    // mid-update...), plus child→parent so a DLC is covered while its base
    // game updates. Same lock and update site as g_activeDl.
    std::unordered_set<AppId_t> g_updating;
    std::unordered_map<AppId_t, AppId_t> g_parentApp;

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp)
        {
            // Track whether this app is actively downloading, so the manifest
            // path can tell a real user download from a background scheduled
            // update. UpdateRunning|UpdateStarted = a download is actually going.
            const bool active = (pApp->AppStateFlags &
                (k_EAppStateUpdateRunning | k_EAppStateUpdateStarted)) != 0;
            // Broader "Steam is working on this app" set for the depot-target
            // gate: the whole update lifecycle, including a paused mid-update,
            // where a target change would land half-done state.
            const bool updating = (pApp->AppStateFlags & (
                k_EAppStateUpdateRunning | k_EAppStateUpdateStarted |
                k_EAppStateUpdatePaused  | k_EAppStateDownloading |
                k_EAppStateReconfiguring | k_EAppStateVerifyingInstalled |
                k_EAppStatePreallocating | k_EAppStateStaging |
                k_EAppStateCommitting    | k_EAppStateVerifyingStaged)) != 0;
            {
                std::lock_guard<std::mutex> lock(g_activeDlMutex);
                if (active) g_activeDl.insert(pApp->nAppID);
                else        g_activeDl.erase(pApp->nAppID);
                if (updating) g_updating.insert(pApp->nAppID);
                else          g_updating.erase(pApp->nAppID);
                if (pApp->ParentAppID)
                    g_parentApp[pApp->nAppID] = pApp->ParentAppID;
            }

            if (LuaConfig::HasDepot(pApp->nAppID, false))
            {
                // We forge ownership for configured apps, so a license that
                // Steam reports as locked by another local user must not keep
                // the Play button disabled. Scoped to configured apps only:
                // stripping it globally would enable Play on shared games we
                // do not inject for, which Steam then rejects at launch.
                if (pApp->OwnershipFlags & k_EAppOwnershipFlags_LicenseLocked)
                {
                    pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                        pApp->OwnershipFlags & ~k_EAppOwnershipFlags_LicenseLocked);
                    LOG_STEAMUI_TRACE("FillInAppOverview: cleared LicenseLocked for appId={}",
                                      pApp->nAppID);
                }

                uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
                if (t)
                {
                    pApp->PurchasedTime = t;
                    LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                      pApp->PurchasedTime, pApp->nAppID);
                }
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // Apps to drop from the library: queued off-thread, marked on the UI thread.
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::unordered_set<AppId_t> g_removedAppIds;

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> draining;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                draining.swap(g_pendingRemovals);
            }
            for (AppId_t appId : draining)
            {
                if (LuaConfig::IsOwned(appId))
                {
                    LOG_STEAMUI_DEBUG("RunFrame: appId {} is owned again, skipping removal", appId);
                    continue;
                }
                if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                {
                    // Only remove from the library if it's not already uninstalled
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    if(pApp->AppStateFlags == k_EAppStateUninstalled){
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }
                }
                
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
            }
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        HOOK_END();
    }

    void Uninstall()
    {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }

    size_t ActiveDownloadCount()
    {
        std::lock_guard<std::mutex> lock(g_activeDlMutex);
        return g_activeDl.size();
    }

    bool IsAppUpdating(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_activeDlMutex);
        AppId_t id = appId;
        for (int hops = 0; hops < 2; ++hops)
        {
            if (g_updating.contains(id)) return true;
            auto it = g_parentApp.find(id);
            if (it == g_parentApp.end()) break;
            id = it->second;
        }
        return false;
    }
}
