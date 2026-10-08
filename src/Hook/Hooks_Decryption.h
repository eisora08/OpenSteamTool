#pragma once
#include "dllmain.h"

namespace Hooks_Decryption {
    // LoadDepotDecryptionKey hook: serves user-provided decryption keys for
    // depots configured via Lua.
    void Install();
    void Uninstall();

    // Reads Steam's own apptickets\<appId> entry from the local config store
    // (localconfig.vdf). Empty when Steam never cached one.
    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId);

    // ── In-memory ownership-ticket cache ──────────────────────────────
    // Steam's localconfig apptickets section is per-machine and only exists
    // once Steam has fetched a ticket there; a fresh PC has none and the
    // ticket forge then has no template (error 54). To cover that, tickets
    // are harvested from two places Steam itself already handles:
    //   * ConfigStore::GetBinary reads of apptickets\<id> (this hook)
    //   * eMsg 858 GetAppOwnershipTicketResponse replies (NetPacket hook)
    // Harvested tickets belong to the logged-in account by construction.

    // Store a ticket (called from both harvest points). Ignores anything too
    // short to be a signed ownership ticket. Flushes the cache if the ticket
    // belongs to a different SteamID (account switch mid-session).
    void CacheAppOwnershipTicket(AppId_t appId, const uint8_t* data, size_t size);

    // In-memory lookup; empty when not harvested this session.
    std::vector<uint8_t> GetMemCachedAppOwnershipTicket(AppId_t appId);

    // Any harvested ticket (forge template fallback — the source appid is
    // irrelevant, only the signature and SteamID matter). Empty on a fresh PC.
    std::vector<uint8_t> GetAnyMemCachedAppOwnershipTicket();

    // Blocks until a ticket for appId is harvested or timeoutMs elapses.
    // Returns the ticket, or empty on timeout.
    std::vector<uint8_t> WaitForCachedTicket(AppId_t appId, uint32_t timeoutMs);
}
