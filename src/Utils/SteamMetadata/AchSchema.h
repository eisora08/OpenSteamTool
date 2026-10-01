#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace AchSchema {

    // Cached synthetic achievements schema (binary VDF, UserGameStatsSchema form)
    // for appId, or "" when nothing has been synthesized yet.
    std::string GetCached(uint32_t appId);

    // Like GetCached, but when a synthesis for appId is still in flight wait up
    // to timeout for it to finish and re-read; "" on timeout or failure. Used so
    // the very first GetUserStats response can carry the schema instead of
    // leaving the client with an empty achievement list until a retry.
    std::string GetCachedAwait(uint32_t appId, std::chrono::milliseconds timeout);

    // Fetch the public achievements page for appId in the background and cache a
    // synthesized schema, adopting Valve's real achievement API names from the
    // global-percentages API when the two sources line up (falling back to
    // display-name slugs). No-op while a fetch is in flight, after success, or
    // during the retry cooldown following a failure.
    void RequestSynth(uint32_t appId);

    // Feed the local SteamID seen on a GetUserStats response header so the
    // per-user record file can be named with the correct account id. The first
    // feed also refreshes cloud_redirect's schema store from the local cache.
    void SetLocalAccountId(uint64_t steamId64);

    // Mirror a synthesized schema into <steam>\appcache\stats\ under the names
    // the Steam client actually loads (UserGameStatsSchema_<appid>.bin and
    // UserGameStats_<accountid>_<appid>.bin), and into cloud_redirect's own
    // schema store (which flushes back over appcache). Files that already
    // exist are left alone unless they are malformed, in which case they are
    // replaced.
    void EnsureSteamStatsFiles(uint32_t appId, const std::string& schemaBytes);

    // True when <steam>\appcache\stats\UserGameStatsSchema_<appid>.bin exists
    // and is well-formed (ends with the binary KV double end marker). When it
    // does, response injection must stand down so the client falls back to its
    // own disk-load path (the one the Steam client and tools like SLSah rely
    // on). Malformed leftovers from older builds never trigger the defer.
    bool SteamStatsFileExists(uint32_t appId);

} // namespace AchSchema
