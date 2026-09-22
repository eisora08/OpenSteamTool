#include "ManifestCache.h"

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace ManifestCache {

namespace {

    // ── Source URLs ───────────────────────────────────────────────
    // Source 1: ManifestHub3 on GitHub (primary, community-maintained)
    constexpr const char* kGitHubRawBase = "https://raw.githubusercontent.com/steamtools-games/ManifestHub3";
    // Source 2: OST Archive (BetterSteamTools)
    constexpr const char* kOstArchiveBase = "https://manifest.luastools.xyz";

    // Real manifests reach ~19 MB; 64 MB leaves generous headroom while still
    // bounding what a hostile origin can make us buffer. Mirrors the server's
    // MAX_MANIFEST_BYTES ceiling.
    constexpr uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;
    // Default recv timeouts per source.
    constexpr uint32_t kGitHubRecvTimeoutMs = 30000;
    constexpr uint32_t kOstArchiveRecvTimeoutMs = 60000;

    // Negative cache: a depot:gid that returns a definitive 404 is remembered
    // here for a short window so Steam's ~30 s scheduled-update retries do not
    // re-GET (and re-hammer) the archive for a manifest we already know is
    // missing. A real download bypasses this (see bypassNegativeCache); a Steam
    // restart clears it, so a manifest a donor supplies later is still picked up.
    constexpr auto kNegativeTtl = std::chrono::minutes(10);
    std::mutex g_negMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_negCache;

    // A valid manifest is at least the payload header + a metadata section + the
    // EOF marker; anything smaller is certainly not one.
    constexpr size_t   kMinBodyBytes = 16;

    // On-disk byte order (little-endian u32 magics).
    constexpr unsigned char kPayloadMagic[4] = {0xD0, 0x17, 0xF6, 0x71}; // 0x71F617D0
    constexpr unsigned char kEofMagic[4]     = {0xAB, 0x15, 0xC4, 0x32}; // 0x32C415AB

    // De-dupe concurrent fetches of the same depot:gid. BuildDepotDependency can
    // fire more than once for a plan, and each spawns a detached worker.
    std::mutex                    g_inFlightMutex;
    std::unordered_set<std::string> g_inFlight;

    struct InFlightGuard {
        std::string key;
        bool acquired = false;
        explicit InFlightGuard(std::string k) : key(std::move(k)) {
            std::lock_guard<std::mutex> lock(g_inFlightMutex);
            acquired = g_inFlight.insert(key).second;
        }
        ~InFlightGuard() {
            if (!acquired) return;
            std::lock_guard<std::mutex> lock(g_inFlightMutex);
            g_inFlight.erase(key);
        }
    };

    fs::path DepotCacheDir() {
        const fs::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
        if (steamExe.empty()) return {};
        return steamExe.parent_path() / "depotcache";
    }

    bool LooksLikeManifest(const std::string& body) {
        if (body.size() < kMinBodyBytes) return false;
        const auto* head = reinterpret_cast<const unsigned char*>(body.data());
        const auto* tail = head + body.size() - 4;
        for (int i = 0; i < 4; ++i) {
            if (head[i] != kPayloadMagic[i]) return false;
            if (tail[i] != kEofMagic[i])     return false;
        }
        return true;
    }

    bool WriteAtomic(const fs::path& dest, const std::string& body) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec); // no-op if it exists

        // Unique-ish temp beside the target so the rename stays on one volume.
        const fs::path tmp = fs::path(dest).concat(
            std::format(".{}.tmp", ::GetCurrentThreadId()));

        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                LOG_MANIFEST_WARN("ManifestCache: cannot open temp {}", tmp.string());
                return false;
            }
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            ofs.flush();
            if (!ofs) {
                LOG_MANIFEST_WARN("ManifestCache: write failed for {}", tmp.string());
                ofs.close();
                fs::remove(tmp, ec);
                return false;
            }
        }

        if (!MoveFileExA(tmp.string().c_str(), dest.string().c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            LOG_MANIFEST_WARN("ManifestCache: rename {} -> {} failed (error={})",
                              tmp.string(), dest.string(), GetLastError());
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    // ── Source helpers ────────────────────────────────────────────
    struct FetchResult {
        bool ok = false;
        bool notArchived = false;
        std::string body;
    };

    FetchResult FetchUrl(const std::string& url, uint32_t recvTimeoutMs,
                         const char* sourceName) {
        const auto to = Config::GetManifestTimeouts();
        const auto resp = OSTPlatform::Http::Execute(
            L"GET", url.c_str(), nullptr, 0, nullptr,
            to.resolve, to.connect, to.send, recvTimeoutMs, kMaxBodyBytes);

        if (!resp.ok || resp.status != 200) {
            const bool notArchived = (resp.ok && resp.status == 404);
            LOG_MANIFEST_TRACE("ManifestCache({}): GET {} status={} notArchived={}",
                               sourceName, url, resp.status, notArchived);
            return {false, notArchived, {}};
        }
        if (!LooksLikeManifest(resp.body)) {
            LOG_MANIFEST_WARN("ManifestCache({}): rejected body for {} ({} bytes)",
                              sourceName, url, resp.body.size());
            return {false, false, {}};
        }
        return {true, false, std::move(resp.body)};
    }

    FetchResult TryGitHubSource(uint32_t depot, uint64_t gid) {
        const std::string url = std::format("{}/depot_cache/{}_{}.bin", kGitHubRawBase, depot, gid);
        return FetchUrl(url, kGitHubRecvTimeoutMs, "GitHub");
    }

    FetchResult TryOstArchiveSource(uint32_t depot, uint64_t gid) {
        const std::string url = std::format("{}/m/{}/{}", kOstArchiveBase, depot, gid);
        return FetchUrl(url, kOstArchiveRecvTimeoutMs, "OST-Archive");
    }

} // namespace

bool EnsureCached(AppId_t app, uint32_t depot, uint64_t gid,
                  uint32_t recvTimeoutMs, bool* outNotArchived,
                  bool bypassNegativeCache) {
    if (outNotArchived) *outNotArchived = false;
    if (!depot || !gid) return false;

    const fs::path dir = DepotCacheDir();
    if (dir.empty()) {
        LOG_MANIFEST_WARN("ManifestCache: could not resolve depotcache dir");
        return false;
    }
    const fs::path dest = dir / std::format("{}_{}.manifest", depot, gid);

    std::error_code ec;
    if (fs::exists(dest, ec)) return true; // Steam already has it

    const std::string key = std::format("{}_{}", depot, gid);

    // Negative cache: skip the GET for a depot:gid we recently saw 404, unless a
    // real download is forcing a fresh check. This is what stops the ~30 s
    // scheduled-update retries from re-hammering the archive. A cached miss is
    // not a fresh "not archived" signal, so leave outNotArchived false.
    if (!bypassNegativeCache) {
        std::lock_guard<std::mutex> lock(g_negMutex);
        auto it = g_negCache.find(key);
        if (it != g_negCache.end()) {
            if (std::chrono::steady_clock::now() - it->second < kNegativeTtl)
                return false;
            g_negCache.erase(it); // stale entry: allow a re-check
        }
    }

    InFlightGuard guard(key);
    if (!guard.acquired) return false; // another worker is already on it

    // Re-check after taking the slot: the other worker may have just finished.
    if (fs::exists(dest, ec)) return true;

    // ── Try sources in configured order ──────────────────────────
    const bool githubFirst = (Config::GetRemoteOrder() != "ost-first");

    FetchResult first, second;
    if (githubFirst) {
        first  = TryGitHubSource(depot, gid);
        second = TryOstArchiveSource(depot, gid);
    } else {
        first  = TryOstArchiveSource(depot, gid);
        second = TryGitHubSource(depot, gid);
    }

    if (first.ok && !first.body.empty()) {
        if (WriteAtomic(dest, first.body)) {
            LOG_MANIFEST_INFO("ManifestCache: cached app={} depot={} gid={} ({} bytes) -> {}",
                              app, depot, gid, first.body.size(), dest.string());
            return true;
        }
        LOG_MANIFEST_WARN("ManifestCache: WriteAtomic failed for depot={} gid={}", depot, gid);
    }

    if (second.ok && !second.body.empty()) {
        if (WriteAtomic(dest, second.body)) {
            LOG_MANIFEST_INFO("ManifestCache: cached app={} depot={} gid={} ({} bytes) -> {}",
                              app, depot, gid, second.body.size(), dest.string());
            return true;
        }
        LOG_MANIFEST_WARN("ManifestCache: WriteAtomic failed for depot={} gid={}", depot, gid);
    }

    // ── Both sources failed ──────────────────────────────────────
    const bool notArchived = first.notArchived && second.notArchived;
    if (outNotArchived) *outNotArchived = notArchived;
    if (notArchived) {
        std::lock_guard<std::mutex> lock(g_negMutex);
        g_negCache[key] = std::chrono::steady_clock::now();
    }
    LOG_MANIFEST_TRACE("ManifestCache: miss app={} depot={} gid={} (first={} second={})",
                       app, depot, gid, first.ok, second.ok);
    return false;
}

} // namespace ManifestCache
