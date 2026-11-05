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
    std::string api;      // real API name (page/percentages) or display-name slug
    std::string name;     // display name
    std::string desc;
    std::string icon;     // icon_achieved, "<hash>.jpg"
    std::string iconGray; // icon_unachieved, "<hash>.jpg" (empty -> reuse icon)
    bool hidden = false;
    double pct = -1.0;    // global % printed on the legacy page (-1 when absent)
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

// ── SSR page parsing (current Steam Community layout) ─────────
//
// The achievements page is now a React SSR document; the achievement rows
// are no longer in the HTML. Everything lives in a JSON island:
//
//   <script type="application/json" id="valve-ssr-data">
//     { ... "renderContext": { "queryData": "{\"queries\":[...]}" } ... }
//   </script>
//
// where queryData is itself a JSON document stored as a string. Inside it:
//
//   queries[].state.data.groups[].achievements[]
//     -> api_name, name, description, hidden, archived,
//        icon_achieved, icon_unachieved
//
// and the game name sits in the island's loaderData as "appName".

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes one level of JSON string escaping. queryData is escaped once inside
// the island and the achievement fields are escaped again inside queryData, so
// this runs twice on the way down.
std::string JsonUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) { out += c; continue; }
        const char e = s[++i];
        switch (e) {
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (i + 4 >= s.size()) { out += e; break; }
            uint32_t cp = 0;
            bool ok = true;
            for (int k = 1; k <= 4; ++k) {
                const int v = HexValue(s[i + k]);
                if (v < 0) { ok = false; break; }
                cp = (cp << 4) | static_cast<uint32_t>(v);
            }
            if (!ok) { out += e; break; }
            i += 4;
            // Surrogate pair (emoji and other non-BMP names).
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
                s[i + 1] == '\\' && s[i + 2] == 'u') {
                uint32_t lo = 0;
                bool ok2 = true;
                for (int k = 3; k <= 6; ++k) {
                    const int v = HexValue(s[i + k]);
                    if (v < 0) { ok2 = false; break; }
                    lo = (lo << 4) | static_cast<uint32_t>(v);
                }
                if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            AppendUtf8(out, cp);
            break;
        }
        default: out += e; break;  // \" \\ \/ and anything unexpected
        }
    }
    return out;
}

bool IsJsonWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

size_t EndOfJsonString(std::string_view s, size_t quotePos) {
    for (size_t i = quotePos + 1; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '"') return i;
    }
    return std::string_view::npos;
}

// Finds "key": "value" and returns the decoded value. Rejects a key whose
// following token is not a string (e.g. an escaped "key" inside a nested
// JSON string), so it can safely be called on the outer island.
bool JsonStringField(std::string_view obj, std::string_view key, std::string& out) {
    std::string pat = "\"";
    pat += key;
    pat += '"';

    size_t search = 0;
    while (true) {
        const size_t p = obj.find(pat, search);
        if (p == std::string_view::npos) return false;
        search = p + pat.size();

        size_t c = p + pat.size();
        while (c < obj.size() && IsJsonWs(obj[c])) ++c;
        if (c >= obj.size() || obj[c] != ':') continue;
        ++c;
        while (c < obj.size() && IsJsonWs(obj[c])) ++c;
        if (c >= obj.size() || obj[c] != '"') continue;

        const size_t e = EndOfJsonString(obj, c);
        if (e == std::string_view::npos) return false;
        out = JsonUnescape(obj.substr(c + 1, e - c - 1));
        return true;
    }
}

bool JsonBoolField(std::string_view obj, std::string_view key, bool& out) {
    std::string pat = "\"";
    pat += key;
    pat += '"';

    size_t search = 0;
    while (true) {
        const size_t p = obj.find(pat, search);
        if (p == std::string_view::npos) return false;
        search = p + pat.size();

        size_t c = p + pat.size();
        while (c < obj.size() && IsJsonWs(obj[c])) ++c;
        if (c >= obj.size() || obj[c] != ':') continue;
        ++c;
        while (c < obj.size() && IsJsonWs(obj[c])) ++c;
        if (obj.substr(c, 4) == "true") { out = true; return true; }
        if (obj.substr(c, 5) == "false") { out = false; return true; }
        continue;
    }
}

// Game name from the document title:
//   <title>Steam Community :: NAME :: Achievements</title>
// Used because loaderData carries appName inside a doubly-escaped string.
bool GameNameFromHtmlTitle(std::string_view html, std::string& gamename) {
    const size_t b = html.find("<title>");
    if (b == std::string_view::npos) return false;
    const size_t start = b + 7;
    const size_t e = html.find("</title>", start);
    if (e == std::string_view::npos) return false;

    const std::string title = HtmlDecode(html.substr(start, e - start));
    static constexpr std::string_view kSep = " :: ";
    const auto s1 = title.find(kSep);
    if (s1 == std::string::npos) return false;
    const auto s2 = title.rfind(kSep);
    if (s2 <= s1) return false;
    gamename = title.substr(s1 + kSep.size(), s2 - s1 - kSep.size());
    return !gamename.empty();
}

// Decodes the island and hands back renderContext.queryData.
bool ExtractSsrData(std::string_view html, std::string& queryData, std::string& appName) {
    const size_t idPos = html.find("id=\"valve-ssr-data\"");
    if (idPos == std::string_view::npos) return false;
    const size_t open = html.find('>', idPos);
    if (open == std::string_view::npos) return false;
    const size_t close = html.find("</script>", open + 1);
    if (close == std::string_view::npos) return false;
    const std::string_view island = html.substr(open + 1, close - open - 1);

    // Game name: loaderData carries {"appid":..,"appName":".."}.
    JsonStringField(island, "appName", appName);

    if (!JsonStringField(island, "queryData", queryData)) return false;

    if (appName.empty()) JsonStringField(queryData, "appName", appName);
    if (appName.empty()) GameNameFromHtmlTitle(html, appName);
    return !queryData.empty();
}

bool ParseSsrPage(std::string_view html, std::vector<Ach>& out, std::string& gamename) {
    out.clear();
    gamename.clear();

    std::string queryData;
    if (!ExtractSsrData(html, queryData, gamename)) return false;

    // Only look at achievement objects once we are inside the groups array;
    // the same document also carries unrelated query state.
    size_t pos = queryData.find("\"groups\"");
    if (pos == std::string_view::npos) return false;

    while (true) {
        pos = queryData.find("\"internal_key\"", pos);
        if (pos == std::string_view::npos) break;

        const size_t next = queryData.find("\"internal_key\"", pos + 14);
        const size_t end = (next == std::string_view::npos) ? queryData.size() : next;
        const std::string_view obj(queryData.data() + pos, end - pos);

        Ach a;
        JsonStringField(obj, "api_name", a.api);
        JsonStringField(obj, "name", a.name);
        JsonStringField(obj, "description", a.desc);
        JsonStringField(obj, "icon_achieved", a.icon);
        JsonStringField(obj, "icon_unachieved", a.iconGray);

        bool hidden = false;
        if (JsonBoolField(obj, "hidden", hidden)) a.hidden = hidden;
        bool archived = false;
        if (JsonBoolField(obj, "archived", archived) && archived)
            a.hidden = true;  // keep the entry so bit positions stay aligned

        if (!(a.name.empty() && a.icon.empty() && a.api.empty()))
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
            PutStr(b, "icon_gray", a.iconGray.empty() ? a.icon : a.iconGray);
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

        // Current layout first; the legacy HTML parser stays as a fallback for
        // pages that still serve the old markup (or a localized variant).
        const bool ssr = ParseSsrPage(res.body, achs, gamename);
        const bool parsed = ssr || ParsePage(res.body, achs, gamename);

        if (parsed) {
            std::unordered_set<std::string> shared;
            if (ssr) {
                // api_name ships in the page now, so the percentages round-trip
                // (and its ordering assumptions) is no longer needed. Only fill
                // gaps with deterministic display-name slugs.
                for (size_t i = 0; i < achs.size(); i++) {
                    if (achs[i].api.empty())
                        achs[i].api = SlugApiName(achs[i].name, static_cast<uint32_t>(i), shared);
                }
            } else {
                // Legacy page: adopt Valve's real API names by zipping the page
                // against the global-percentages API (both are ordered by
                // global %). The two sources round differently (page rounds,
                // API truncates: 0.1 vs 0.0, 96.8 vs 96.7) so allow up to 0.11
                // of slack; anything else keeps display-name slugs.
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
                for (size_t i = 0; i < achs.size(); i++) {
                    if (zip)
                        achs[i].api = std::move(global[i].first);
                    else
                        achs[i].api = SlugApiName(achs[i].name, static_cast<uint32_t>(i), shared);
                }
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
            LOG_ACHIEVEMENT_DEBUG("AchSchema: no achievements for app {} (http {}, body {} bytes, ssr {})",
                                  appId, res.status, res.body.size(),
                                  res.body.find("valve-ssr-data") != std::string::npos);
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
