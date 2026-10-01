#include "AchSchema.h"

#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AchSchema {

namespace {

struct Ach {
    std::string api;   // real API name (percentages zip) or display-name slug
    std::string name;  // display name
    std::string desc;
    std::string icon;  // "<hash>.jpg" (colored); reused for icon_gray
    bool hidden = false;
    double pct = -1.0; // global % printed on the page (-1 when absent)
};

enum class State : uint8_t { InFlight, Ready, Failed };

std::mutex g_mtx;
std::condition_variable g_cv;
std::unordered_map<uint32_t, State>    g_state;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_retryAt;
std::atomic<uint32_t> g_accountId{0};

constexpr auto kRetryCooldown = std::chrono::minutes(30);

std::filesystem::path CachePath(uint32_t appId) {
    return std::filesystem::path(Config::logDir) / "schemas" / (std::to_string(appId) + ".bin");
}

// ── Tiny HTML helpers (tolerant, single-pass, no full parser) ──

std::string HtmlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { out += s[i++]; continue; }
        struct Ent { std::string_view ent; char ch; };
        static constexpr Ent kEnts[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
            {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '},
        };
        bool matched = false;
        for (const auto& e : kEnts) {
            if (s.substr(i, e.ent.size()) == e.ent) {
                out += e.ch;
                i += e.ent.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        // Numeric entity &#NN; / &#xNN;
        if (i + 3 < s.size() && s[i + 1] == '#' && (s[i + 2] == 'x' || s[i + 2] == 'X')) {
            uint32_t cp = 0;
            size_t j = i + 3;
            bool hex = true, any = false;
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            while (j < s.size() && hexVal(s[j]) >= 0) { cp = cp * 16 + hexVal(s[j]); j++; any = true; }
            if (any && j < s.size() && s[j] == ';') {
                if (cp >= 0x80) { /* UTF-8 encode */ 
                    if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                } else out += char(cp);
                i = j + 1; continue;
            }
        } else if (i + 2 < s.size() && s[i + 1] == '#') {
            uint32_t cp = 0;
            size_t j = i + 2;
            bool any = false;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') { cp = cp * 10 + (s[j] - '0'); j++; any = true; }
            if (any && j < s.size() && s[j] == ';') {
                if (cp >= 0x80) {
                    if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                } else out += char(cp);
                i = j + 1; continue;
            }
        }
        out += s[i++];
    }
    return out;
}

std::string_view TagContent(std::string_view block, std::string_view open, std::string_view close) {
    auto b = block.find(open);
    if (b == std::string_view::npos) return {};
    b += open.size();
    auto e = block.find(close, b);
    if (e == std::string_view::npos) return {};
    return block.substr(b, e - b);
}

std::string SlugApiName(const std::string& display, uint32_t bit,
                        std::unordered_set<std::string>& used) {
    std::string slug;
    slug.reserve(display.size());
    bool lastUnderscore = false;
    for (char c : display) {
        unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) { slug += char(u); lastUnderscore = false; }
        else if ((u >= 'a' && u <= 'z')) { slug += char(u - 'a' + 'A'); lastUnderscore = false; }
        else if (!lastUnderscore && !slug.empty()) { slug += '_'; lastUnderscore = true; }
    }
    while (!slug.empty() && slug.back() == '_') slug.pop_back();
    if (slug.empty()) slug = "ACH_" + std::to_string(bit);
    std::string candidate = slug;
    for (uint32_t n = 2; !used.insert(candidate).second; n++)
        candidate = slug + "_" + std::to_string(n);
    return candidate;
}

// ── Page parsing ─────────────────────────────────────────────

bool ParsePage(std::string_view html, std::vector<Ach>& out, std::string& gamename) {
    gamename.clear();

    // Game name: <meta property="og:title" content="Name on Steam" />
    // Achievements pages report "Steam Community :: Name :: Achievements".
    if (auto m = html.find("og:title"); m != std::string_view::npos) {
        auto c = html.find("content=\"", m);
        if (c != std::string_view::npos) {
            c += 9;
            auto e = html.find('"', c);
            if (e != std::string_view::npos) {
                gamename = HtmlDecode(html.substr(c, e - c));
                static constexpr std::string_view kSuffix = " on Steam";
                if (gamename.size() > kSuffix.size() &&
                    gamename.compare(gamename.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
                    gamename.resize(gamename.size() - kSuffix.size());
                static constexpr std::string_view kSep = " :: ";
                auto s1 = gamename.find(kSep);
                if (s1 != std::string::npos) {
                    auto s2 = gamename.rfind(kSep);
                    if (s2 > s1)
                        gamename = gamename.substr(s1 + kSep.size(), s2 - s1 - kSep.size());
                }
            }
        }
    }

    static constexpr std::string_view kMarker = "<div class=\"achieveRow";
    size_t pos = 0;
    while ((pos = html.find(kMarker, pos)) != std::string_view::npos) {
        size_t end = html.find(kMarker, pos + kMarker.size());
        if (end == std::string_view::npos) end = html.size();
        std::string_view block = html.substr(pos, end - pos);

        Ach a;
        a.name = HtmlDecode(TagContent(block, "<h3>", "</h3>"));
        a.desc = HtmlDecode(TagContent(block, "<h5>", "</h5>"));

        if (auto img = block.find("<img src=\""); img != std::string_view::npos) {
            img += 10;
            auto q = block.find('"', img);
            if (q != std::string_view::npos) {
                std::string_view url = block.substr(img, q - img);
                auto slash = url.rfind('/');
                if (slash != std::string_view::npos) a.icon = std::string(url.substr(slash + 1));
            }
        }

        static constexpr std::string_view kPct = "achievePercent\">";
        if (auto p = block.find(kPct); p != std::string_view::npos) {
            size_t i = p + kPct.size();
            while (i < block.size() && (block[i] == ' ' || block[i] == '\t' ||
                                        block[i] == '\r' || block[i] == '\n'))
                i++;
            size_t e = i;
            while (e < block.size() &&
                   ((block[e] >= '0' && block[e] <= '9') || block[e] == '.'))
                e++;
            if (e > i)
                a.pct = std::strtod(std::string(block.substr(i, e - i)).c_str(), nullptr);
        }

        a.hidden = a.name.empty() || a.name == "???" ||
                   a.name.compare(0, 6, "Hidden") == 0;
        if (a.icon.empty() && a.name.empty()) { pos = end; continue; }
        out.push_back(std::move(a));
        pos = end;
    }
    return !out.empty();
}

// ── Global percentages JSON ──────────────────────────────────
// Response: {"achievementpercentages":{"achievements":{"achievement":
//   [{"name":"CAT1","percent":"98.8"}, ...]}}} — same order as the page.
// Used only to adopt Valve's real API names (the key player percentages
// are merged by); every failure path falls back to display-name slugs.

bool ParsePercentPairs(std::string_view body,
                       std::vector<std::pair<std::string, double>>& out) {
    out.clear();
    size_t i = 0;
    while ((i = body.find("\"name\"", i)) != std::string_view::npos) {
        auto c = body.find(':', i + 6);
        auto q1 = (c == std::string_view::npos) ? std::string_view::npos
                                                : body.find('"', c + 1);
        auto q2 = (q1 == std::string_view::npos) ? std::string_view::npos
                                                 : body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        auto pk = body.find("\"percent\"", q2);
        auto pc = (pk == std::string_view::npos) ? std::string_view::npos
                                                 : body.find(':', pk + 9);
        if (pc == std::string_view::npos) return false;
        size_t v = pc + 1;
        while (v < body.size() && (body[v] == ' ' || body[v] == '\t' ||
                                   body[v] == '\r' || body[v] == '\n' || body[v] == '"'))
            v++;
        size_t e = v;
        while (e < body.size() && ((body[e] >= '0' && body[e] <= '9') ||
                                   body[e] == '.' || body[e] == '-' ||
                                   body[e] == 'e' || body[e] == 'E'))
            e++;
        if (e == v) return false;
        out.emplace_back(std::string(body.substr(q1 + 1, q2 - q1 - 1)),
                         std::strtod(std::string(body.substr(v, e - v)).c_str(), nullptr));
        i = e;
    }
    return !out.empty();
}

// ── Binary VDF writer (Valve KeyValues: types 0/1/2/8) ──────

constexpr char kVdfSubkey = 0x00;
constexpr char kVdfString = 0x01;
constexpr char kVdfInt32  = 0x02;
constexpr char kVdfEnd    = 0x08;

void PutKey(std::string& b, char type, std::string_view key) {
    b += type;
    b.append(key);
    b += '\0';
}
void PutStr(std::string& b, std::string_view key, std::string_view val) {
    PutKey(b, kVdfString, key);
    b.append(val);
    b += '\0';
}
void PutInt(std::string& b, std::string_view key, uint32_t val) {
    PutKey(b, kVdfInt32, key);
    for (int i = 0; i < 4; i++) b += char((val >> (8 * i)) & 0xFF);
}

std::string BuildSchema(uint32_t appId, const std::vector<Ach>& achs,
                        const std::string& gamename) {
    std::string b;
    b.reserve(4096 + achs.size() * 220);

    PutKey(b, kVdfSubkey, std::to_string(appId));
    PutKey(b, kVdfSubkey, "stats");

    for (size_t g = 0; g * 32 < achs.size(); g++) {
        const size_t groupId = g + 1;
        const size_t begin = g * 32;
        const size_t end = (begin + 32 < achs.size()) ? begin + 32 : achs.size();
        PutKey(b, kVdfSubkey, std::to_string(groupId));
        PutKey(b, kVdfSubkey, "bits");
        for (size_t i = begin; i < end; i++) {
            const Ach& a = achs[i];
            const std::string tokenBase =
                "NEW_ACHIEVEMENT_" + std::to_string(groupId) + "_" + std::to_string(i - begin);
            PutKey(b, kVdfSubkey, std::to_string(i - begin));
            PutStr(b, "name", a.api);
            PutKey(b, kVdfSubkey, "display");
            PutKey(b, kVdfSubkey, "name");
            PutStr(b, "english", a.name);
            PutStr(b, "token", tokenBase + "_NAME");
            b += kVdfEnd;
            PutKey(b, kVdfSubkey, "desc");
            PutStr(b, "english", a.desc);
            PutStr(b, "token", tokenBase + "_DESC");
            b += kVdfEnd;
            PutInt(b, "hidden", a.hidden ? 1u : 0u);
            PutStr(b, "icon", a.icon);
            PutStr(b, "icon_gray", a.icon);
            b += kVdfEnd;  // display
            b += kVdfEnd;  // bit
        }
        b += kVdfEnd;      // bits
        PutStr(b, "type", "ACHIEVEMENTS");
        b += kVdfEnd;      // group
    }
    b += kVdfEnd;      // stats
    PutInt(b, "version", 3);
    if (!gamename.empty()) PutStr(b, "gamename", gamename);
    b += kVdfEnd;      // root
    b += kVdfEnd;      // file terminator (top-level end marker)
    return b;
}

// ── Fetch + cache ────────────────────────────────────────────

constexpr wchar_t kHttpHeaders[] =
    L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/126.0 Safari/537.36\r\n";

std::vector<std::pair<std::string, double>> FetchGlobalPercentages(uint32_t appId) {
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.steampowered.com/ISteamUserStats/"
             "GetGlobalAchievementPercentagesForApp/v1/?gameid=%u",
             appId);
    auto res = OSTPlatform::Http::Execute(L"GET", url, nullptr, 0, kHttpHeaders,
                                          4000, 4000, 5000, 10000, 512 * 1024);
    std::vector<std::pair<std::string, double>> out;
    if (res.ok && res.status == 200 && !res.body.empty())
        ParsePercentPairs(res.body, out);
    return out;
}

void FetchAndCache(uint32_t appId) {
    char url[96];
    snprintf(url, sizeof(url), "https://steamcommunity.com/stats/%u/achievements/?l=english", appId);

    auto res = OSTPlatform::Http::Execute(L"GET", url, nullptr, 0, kHttpHeaders,
                                          4000, 4000, 5000, 10000, 512 * 1024);

    bool ok = false;
    uint32_t count = 0;
    uint32_t bytesLen = 0;
    if (res.ok && res.status == 200 && !res.body.empty()) {
        std::vector<Ach> achs;
        std::string gamename;
        if (ParsePage(res.body, achs, gamename)) {
            // Adopt Valve's real API names by zipping the page against the
            // global-percentages API (both are ordered by global %). The two
            // sources round differently (page rounds, API truncates: 0.1 vs
            // 0.0, 96.8 vs 96.7) so allow up to 0.11 of slack; anything else
            // keeps display-name slugs for every achievement.
            auto global = FetchGlobalPercentages(appId);
            bool zip = global.size() == achs.size();
            if (zip) {
                for (size_t i = 0; i < achs.size(); i++) {
                    if (std::fabs(global[i].second - achs[i].pct) > 0.11) {
                        zip = false;
                        break;
                    }
                }
            }
            if (!zip)
                LOG_ACHIEVEMENT_WARN(
                    "AchSchema: percentages unavailable for app {} ({} vs {} rows), "
                    "using display-name slugs",
                    appId, global.size(), achs.size());
            std::unordered_set<std::string> shared;
            for (size_t i = 0; i < achs.size(); i++) {
                if (zip)
                    achs[i].api = std::move(global[i].first);
                else
                    achs[i].api = SlugApiName(achs[i].name, static_cast<uint32_t>(i), shared);
            }
            std::string bytes = BuildSchema(appId, achs, gamename);
            bytesLen = static_cast<uint32_t>(bytes.size());

            EnsureSteamStatsFiles(appId, bytes);

            std::error_code ec;
            auto dir = CachePath(appId).parent_path();
            std::filesystem::create_directories(dir, ec);
            std::ofstream f(CachePath(appId), std::ios::binary | std::ios::trunc);
            if (f) {
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                f.close();
                ok = f.good();
                count = static_cast<uint32_t>(achs.size());
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state[appId] = ok ? State::Ready : State::Failed;
        if (!ok) g_retryAt[appId] = std::chrono::steady_clock::now() + kRetryCooldown;

        if (ok)
            LOG_ACHIEVEMENT_DEBUG("AchSchema: synthesized schema for app {} ({} achievements, {} bytes)",
                                  appId, count, bytesLen);
        else
            LOG_ACHIEVEMENT_DEBUG("AchSchema: no achievements page for app {} (http status {})",
                                  appId, res.status);
    }
    g_cv.notify_all();
}

} // namespace

// Valve's binary KV schema files always close with the root map end marker
// (0x08) followed by the top-level end marker (0x08). A file missing the
// terminator parses as a prefix and is silently rejected by the client, so
// treat anything else as absent/broken instead of deferring to it.
static bool SchemaFileLooksValid(const std::filesystem::path& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz < 3) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(-2, std::ios::end);
    char tail[2] = {};
    f.read(tail, 2);
    return f.gcount() == 2 && tail[0] == static_cast<char>(kVdfEnd) &&
           tail[1] == static_cast<char>(kVdfEnd);
}

std::string GetCached(uint32_t appId) {
    std::error_code ec;
    auto path = CachePath(appId);
    if (!std::filesystem::exists(path, ec)) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string GetCachedAwait(uint32_t appId, std::chrono::milliseconds timeout) {
    std::string s = GetCached(appId);
    if (!s.empty()) return s;
    {
        std::unique_lock<std::mutex> lk(g_mtx);
        if (!g_cv.wait_for(lk, timeout, [appId] {
                auto it = g_state.find(appId);
                return it == g_state.end() || it->second != State::InFlight;
            }))
            return {};
    }
    return GetCached(appId);
}

void RequestSynth(uint32_t appId) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_state.find(appId);
        if (it != g_state.end()) {
            if (it->second == State::InFlight || it->second == State::Ready) return;
            auto at = g_retryAt.find(appId);
            if (at != g_retryAt.end() && std::chrono::steady_clock::now() < at->second) return;
        }
        g_state[appId] = State::InFlight;
    }
    OSTPlatform::Thread::StartDetached([appId]() -> uint32_t {
        FetchAndCache(appId);
        return 0;
    });
}

// cloud_redirect.dll keeps its own copy of every synthesized schema under
// cloud_redirect\stats\<accountid>\schemas\ and flushes that store back over
// appcache\stats whenever Steam flushes its stats cache. A stale copy there
// resurrects old schemas, so every schema we own is mirrored into it.
std::filesystem::path CrSchemaPath(uint32_t accountId, uint32_t appId) {
    return std::filesystem::path(Config::logDir).parent_path() / "cloud_redirect" /
           "stats" / std::to_string(accountId) / "schemas" /
           (std::to_string(appId) + ".bin");
}

bool FileMatches(const std::filesystem::path& p, const std::string& bytes) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz != bytes.size()) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string cur((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return cur == bytes;
}

void MirrorCrSchema(uint32_t accountId, uint32_t appId, const std::string& bytes) {
    if (accountId == 0 || bytes.empty()) return;
    std::error_code ec;
    auto p = CrSchemaPath(accountId, appId);
    if (FileMatches(p, bytes)) return;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
        LOG_ACHIEVEMENT_WARN("AchSchema: cannot create {} ({})", p.parent_path().string(),
                             ec.message());
        return;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (f.good())
        LOG_ACHIEVEMENT_INFO("AchSchema: mirrored schema ({} bytes) for app {} into {}",
                             bytes.size(), appId, p.string());
    else
        LOG_ACHIEVEMENT_WARN("AchSchema: failed writing {}", p.string());
}

void SetLocalAccountId(uint64_t steamId64) {
    if (steamId64 == 0) return;
    const uint32_t accountId = static_cast<uint32_t>(steamId64 & 0xFFFFFFFFull);
    g_accountId.store(accountId, std::memory_order_relaxed);

    // Once per account per session: refresh cloud_redirect's whole schema
    // store from our cache, so its flush cannot resurrect files captured in
    // an older session (they only diverge when a session ends mid-update).
    static std::atomic<uint32_t> g_crSynced{0};
    if (g_crSynced.exchange(accountId) == accountId) return;
    std::error_code ec;
    auto dir = std::filesystem::path(Config::logDir) / "schemas";
    if (!std::filesystem::exists(dir, ec) || ec) return;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.path().extension() != ".bin") continue;
        const std::string stem = e.path().stem().string();
        char* end = nullptr;
        const unsigned long v = std::strtoul(stem.c_str(), &end, 10);
        if (!end || *end != '\0' || v == 0 || v > 0xFFFFFFFFul) continue;
        std::ifstream f(e.path(), std::ios::binary);
        if (!f) continue;
        std::string bytes((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        MirrorCrSchema(accountId, static_cast<uint32_t>(v), bytes);
    }
}

void EnsureSteamStatsFiles(uint32_t appId, const std::string& schemaBytes) {
    if (schemaBytes.empty()) return;

    std::error_code ec;
    auto dir = std::filesystem::path(Config::logDir).parent_path() / "appcache" / "stats";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ACHIEVEMENT_WARN("AchSchema: cannot create {} ({})", dir.string(), ec.message());
        return;
    }

    auto schemaPath = dir / ("UserGameStatsSchema_" + std::to_string(appId) + ".bin");
    const bool exists = std::filesystem::exists(schemaPath, ec);
    if (exists && SchemaFileLooksValid(schemaPath)) {
        LOG_ACHIEVEMENT_DEBUG("AchSchema: {} already exists, leaving Steam's file untouched",
                              schemaPath.filename().string());
    } else {
        if (exists)
            LOG_ACHIEVEMENT_WARN("AchSchema: {} is malformed, replacing it",
                                 schemaPath.filename().string());
        std::ofstream f(schemaPath, std::ios::binary | std::ios::trunc);
        f.write(schemaBytes.data(), static_cast<std::streamsize>(schemaBytes.size()));
        f.close();
        if (f.good())
            LOG_ACHIEVEMENT_INFO("AchSchema: wrote {} ({} bytes) for app {}",
                                 schemaPath.filename().string(), schemaBytes.size(), appId);
        else
            LOG_ACHIEVEMENT_WARN("AchSchema: failed writing {}", schemaPath.string());
    }

    const uint32_t accountId = g_accountId.load(std::memory_order_relaxed);
    if (accountId != 0)
        MirrorCrSchema(accountId, appId, schemaBytes);
    if (accountId == 0) return;
    auto recPath = dir / ("UserGameStats_" + std::to_string(accountId) + "_" +
                          std::to_string(appId) + ".bin");
    if (std::filesystem::exists(recPath, ec)) return;
    // Minimal per-user record, byte-identical to the 38-byte files Steam itself
    // writes for games without local changes: cache { crc = 0, PendingChanges = 0 }.
    static constexpr char kUserRecord[] = {
        0x00, 'c', 'a', 'c', 'h', 'e', 0x00,
        0x02, 'c', 'r', 'c', 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 'P', 'e', 'n', 'd', 'i', 'n', 'g', 'C', 'h', 'a', 'n', 'g', 'e', 's',
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x08,
    };
    static_assert(sizeof(kUserRecord) == 38, "user record must stay 38 bytes");
    std::ofstream rf(recPath, std::ios::binary | std::ios::trunc);
    rf.write(kUserRecord, sizeof(kUserRecord));
    rf.close();
    if (rf.good())
        LOG_ACHIEVEMENT_INFO("AchSchema: wrote {} (38 bytes) for app {}",
                             recPath.filename().string(), appId);
    else
        LOG_ACHIEVEMENT_WARN("AchSchema: failed writing {}", recPath.string());
}

bool SteamStatsFileExists(uint32_t appId) {
    std::error_code ec;
    auto dir = std::filesystem::path(Config::logDir).parent_path() / "appcache" / "stats";
    auto p = dir / ("UserGameStatsSchema_" + std::to_string(appId) + ".bin");
    return std::filesystem::exists(p, ec) && SchemaFileLooksValid(p);
}

} // namespace AchSchema
