#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <mutex>
#include <string_view>
#include <thread>

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

    struct Provider {
        std::string_view name;
        const char*      urlTemplate;   // gid-only: one %llu
        const char*      urlTemplateEx; // depot-aware: %u app, %u depot, %llu gid (or depot+gid: %llu/%llu)
        bool             needsDepot;    // if true and no depotId available, skip this provider
        Parser           parse;
        const wchar_t*   headers;       // extra request headers (e.g. User-Agent), or nullptr
    };

    consteval Provider Make(std::string_view name, const char* url, const char* urlEx,
                            bool needsDepot, Parser parse, const wchar_t* headers = nullptr) {
        return {name, url, urlEx, needsDepot, parse, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",
                              "https://manifest.opensteamtool.com/%u/%u/%llu", false, ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",
                              nullptr, false, ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",
                              nullptr, false, ParseSteamRunJson),
        Make("20770407",      "https://20770407.xyz/manifest/%llu/%llu",
                              nullptr, true, ParsePlainUint),
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",
                              nullptr, false, ParsePlainUint,
                              L"User-Agent: ManifestDeX/1.0\r\n"),
    };

    static std::atomic<const Provider*> g_active{&kProviders[0]};
    static std::mutex g_luaMutex;

    bool SetProvider(std::string_view name) {
        for (const auto& p : kProviders)
            if (p.name == name) {
                g_active.store(&p, std::memory_order_release);
                return true;
            }
        return false;
    }

    const char* ActiveProviderName() {
        const auto* p = g_active.load(std::memory_order_acquire);
        return p ? p->name.data() : "";
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        const auto* active = g_active.load(std::memory_order_acquire);
        if (!active) return false;
        const Provider& p = *active;
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();

        // If the provider requires a depot and we don't have one, fail.
        if (p.needsDepot && !depotId) {
            LOG_MANIFEST_WARN("Manifest {} requires depotId but none provided for gid={}", p.name, gid);
            return false;
        }

        char urlLog[256];
        const bool depotAware = p.urlTemplateEx && depotId;
        if (depotAware)
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplateEx, appId, depotId, gid);
        else if (p.needsDepot)
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        else
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(gid));

        auto r = OSTPlatform::Http::Execute(
            L"GET", urlLog, nullptr, 0, p.headers,
            timeouts.resolve, timeouts.connect, timeouts.send, timeouts.recv);

        // Retry once on 429 (rate limit) with a 2s backoff.
        if (r.ok && r.status == 429) {
            LOG_MANIFEST_WARN("Manifest {} rate-limited (429) for gid={}, retrying in 2s", p.name, gid);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            r = OSTPlatform::Http::Execute(
                L"GET", urlLog, nullptr, 0, p.headers,
                timeouts.resolve, timeouts.connect, timeouts.send, timeouts.recv);
        }

        LOG_MANIFEST_INFO("Manifest {} status={} gid={} depot={} shape={}",
                          p.name, r.status, gid, depotId, depotAware ? "app/depot/gid" : "gid-only");

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            std::lock_guard<std::mutex> lock(g_luaMutex);
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            std::lock_guard<std::mutex> lock(g_luaMutex);
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return FetchActive(manifestGid, outRequestCode, appId, depotId);
    }
}
