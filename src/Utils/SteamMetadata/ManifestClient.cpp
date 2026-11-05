#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())))
            body.remove_prefix(1);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))
            body.remove_suffix(1);
        if (body.empty()) return false;
        uint64_t code = 0;
        auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{} || ptr != body.data() + body.size()) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // One row per source. url is the gid-only shape, depotUrl takes
    // (depotId, gid), appUrl takes (appId, depotId, gid). A source that can
    // only answer one shape leaves the others null; needsDepot sources are
    // skipped when the depot id is unknown instead of being counted as a
    // failure.
    struct Provider {
        std::string_view name;
        const char*      url;        // one %llu (gid)
        const char*      depotUrl;   // two %llu (depot, gid)
        const char*      appUrl;     // %u %u %llu (app, depot, gid)
        bool             needsDepot;
        Parser           parse;
        const wchar_t*   headers;    // extra request headers (e.g. UA gate), or nullptr
    };

    consteval Provider Make(std::string_view name, const char* url, const char* depotUrl,
                            const char* appUrl, bool needsDepot, Parser parse,
                            const wchar_t* headers = nullptr)
    {
        return {name, url, depotUrl, appUrl, needsDepot, parse, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",
                              nullptr,
                              "https://manifest.opensteamtool.com/%u/%u/%llu",
                              false, ParsePlainUint),
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",
                              nullptr, nullptr,
                              false, ParsePlainUint,
                              L"User-Agent: ManifestDeX/1.0\r\n"),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",
                              nullptr, nullptr,
                              false, ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",
                              nullptr, nullptr,
                              false, ParseSteamRunJson),
        Make("20770407",      nullptr,
                              "https://20770407.xyz/manifest/%llu/%llu",
                              nullptr,
                              true, ParsePlainUint),
        Make("SDM",           nullptr,
                              "https://steamapi.993499094.xyz/manifest/%llu/%llu",
                              nullptr,
                              true, ParsePlainUint),
    };

    static constexpr size_t kProviderCount = std::size(kProviders);

    // ── provider state ────────────────────────────────────────────
    //
    // The lock only protects the index and the cooldown/sequence tables. No
    // HTTP and no Lua call is made while it is held, so a dead upstream cannot
    // serialise the other depots behind it.
    static std::mutex g_stateMutex;

    static size_t g_active = 0;                       // opensteamtool by default
    static std::chrono::steady_clock::time_point g_deadUntil[kProviderCount];
    static uint64_t g_okSeq[kProviderCount] = {};

    // Total time one code resolution may spend across every provider. The hook
    // only waits budget+1s, so anything slower is useless to Steam.
    static constexpr uint32_t kFetchBudgetMs = 8000;
    static constexpr uint32_t kProviderCooldownMs = 60000;

    // Failover switch, mirrors [manifest] failover.
    static bool g_failover = true;

    static size_t ActiveIndex() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_active;
    }

    static bool FailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failover;
    }

    static bool IsCoolingDown(size_t i, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_deadUntil[i] > now;
    }

    static uint64_t OkSeq(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_okSeq[i];
    }

    static void MarkFailed(size_t i, std::chrono::steady_clock::time_point now, uint64_t seq) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        // A success that landed after this request started describes a newer
        // state; do not let an outdated failure bench a healthy provider.
        if (g_okSeq[i] != seq) {
            LOG_MANIFEST_DEBUG("Manifest {} failure ignored (superseded by a success)",
                               kProviders[i].name);
            return;
        }
        g_deadUntil[i] = now + std::chrono::milliseconds(kProviderCooldownMs);
    }

    static void RecordSuccess(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ++g_okSeq[i];
        g_deadUntil[i] = {};
    }

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (size_t i = 0; i < kProviderCount; ++i) {
            if (kProviders[i].name == name) {
                // Re-selecting the same provider is a no-op: a full config
                // reload must not clear the cooldown of a provider already
                // proven dead.
                if (i == g_active) return true;
                g_active = i;
                g_deadUntil[i] = {};
                ++g_okSeq[i];
                return true;
            }
        }
        return false;
    }

    void SetFailoverEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_failover == enabled) return;
        g_failover = enabled;
        // Old cooldown verdicts are meaningless under the new policy.
        for (auto& t : g_deadUntil) t = {};
    }

    bool IsFailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failover;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return kProviders[g_active].name.data();
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchFrom(const Provider& p, uint64_t gid, AppId_t appId, AppId_t depotId,
                          uint64_t* outCode, uint32_t resolveMs, uint32_t connectMs,
                          uint32_t sendMs, uint32_t recvMs) {
        char url[256];
        if (p.appUrl && appId && depotId) {
            std::snprintf(url, sizeof(url), p.appUrl,
                          static_cast<unsigned>(appId),
                          static_cast<unsigned>(depotId),
                          static_cast<unsigned long long>(gid));
        } else if (p.depotUrl && depotId) {
            std::snprintf(url, sizeof(url), p.depotUrl,
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        } else if (p.url) {
            std::snprintf(url, sizeof(url), p.url,
                          static_cast<unsigned long long>(gid));
        } else {
            return false;
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET", url, nullptr, 0, p.headers,
            resolveMs, connectMs, sendMs, recvMs);

        LOG_MANIFEST_INFO("Manifest {} status={} depot={} gid={}", p.name, r.status, depotId, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // Failover off: only the configured provider is tried.
    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        const size_t i = ActiveIndex();
        const Provider& p = kProviders[i];
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        return FetchFrom(p, gid, appId, depotId, outCode,
                         timeouts.resolve, timeouts.connect, timeouts.send, timeouts.recv);
    }

    // Ring walk from the configured provider: skip cooling sources, skip
    // depot-only sources without a depot id (not counted as a failure), stop
    // once the budget is exhausted.
    static bool FetchWithFailover(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kFetchBudgetMs);

        const size_t start = ActiveIndex();

        size_t tried = 0;
        for (size_t k = 0; k < kProviderCount; ++k) {
            const size_t i = (start + k) % kProviderCount;
            const Provider& p = kProviders[i];

            const auto now = std::chrono::steady_clock::now();
            if (IsCoolingDown(i, now)) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (cooldown)", p.name);
                continue;
            }
            if (p.needsDepot && !depotId) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (no depot id)", p.name);
                continue;
            }

            const uint64_t seq = OkSeq(i);

            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                LOG_MANIFEST_WARN("Manifest gid={}: {}ms budget exhausted after {} attempt(s)",
                                  gid, kFetchBudgetMs, tried);
                break;
            }
            // Clamp to the remaining budget and never below 1ms: WinHTTP reads
            // a zero timeout as "wait forever", which would run past the hook's
            // own wait window.
            const auto cap = [left](uint32_t ms) {
                return static_cast<uint32_t>(
                    std::max<int64_t>(1, std::min<int64_t>(ms, left)));
            };

            const bool ok = FetchFrom(p, gid, appId, depotId, outCode,
                                      cap(timeouts.resolve), cap(timeouts.connect),
                                      cap(timeouts.send), cap(timeouts.recv));
            ++tried;

            if (ok) {
                RecordSuccess(i);
                return true;
            }

            MarkFailed(i, std::chrono::steady_clock::now(), seq);
        }

        if (tried == 0)
            LOG_MANIFEST_WARN("Manifest gid={}: every provider is in cooldown", gid);
        return false;
    }

    // ── public ────────────────────────────────────────────────────

    // LuaConfig exposes one shared lua_State and is not internally locked, so
    // concurrent code resolution has to serialise here. The lock covers only
    // the Lua segment; network I/O happens after it is released.
    static std::mutex g_luaMutex;

    static bool TryLua(uint64_t manifestGid, uint64_t* outRequestCode,
                       AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_luaMutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return false;
    }

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (TryLua(manifestGid, outRequestCode, appId, depotId))
            return true;

        if (FailoverEnabled())
            return FetchWithFailover(manifestGid, outRequestCode, appId, depotId);

        return FetchActive(manifestGid, outRequestCode, appId, depotId);
    }
} // namespace ManifestClient
