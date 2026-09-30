#include "AcfSizeRepair.h"

#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace AcfSizeRepair {

namespace {

    // k_EAppStateFullyInstalled
    constexpr uint32_t kFullyInstalled = 0x4;

    bool ReadAllText(const fs::path& path, std::string* out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out->assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
        return !out->empty();
    }

    // Write to a sibling temp file and rename over the target so a reader
    // never sees a half-written ACF (Steam re-reads these files on demand).
    bool WriteAllText(const fs::path& path, const std::string& text) {
        const fs::path tmp = fs::path(path.native() + L".osttmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out.good()) {
                out.close();
                std::error_code rm;
                fs::remove(tmp, rm);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (!ec) return true;

        // Fallback: in-place overwrite (e.g. rename blocked while Steam has
        // the file open elsewhere).
        fs::remove(tmp, ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    // Sum of regular-file sizes under dir (recursive). Best-effort: unreadable
    // entries are skipped, never thrown.
    uint64_t DirectorySize(const fs::path& dir) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) return 0;
        uint64_t total = 0;
        fs::recursive_directory_iterator it(
            dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return 0;
        const fs::recursive_directory_iterator end;
        while (it != end) {
            std::error_code entryEc;
            if (it->is_regular_file(entryEc) && !entryEc) {
                const auto bytes = it->file_size(entryEc);
                if (!entryEc) total += bytes;
            }
            it.increment(ec);
            if (ec) break;
        }
        return total;
    }

    // Fallback when the install directory is gone: the sum of the size fields
    // under InstalledDepots (the only other size source Steam keeps).
    uint64_t SumDepotSizes(const std::string& text) {
        static const std::regex re(R"re("size"(\s*)"(\d+)")re");
        uint64_t total = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), re), n; it != n; ++it)
            total += std::strtoull((*it)[2].str().c_str(), nullptr, 10);
        return total;
    }

    bool EndsWith(const std::string& s, const char* suffix) {
        const size_t len = std::char_traits<char>::length(suffix);
        return s.size() >= len &&
               s.compare(s.size() - len, len, suffix) == 0;
    }

    // Returns true when the ACF was rewritten.
    bool RepairFile(const fs::path& acfPath) {
        std::string text;
        if (!ReadAllText(acfPath, &text)) return false;

        // Only SizeOnDisk=0 entries are in scope; anything else Steam already
        // tracks correctly.
        static const std::regex sizeRe(R"re("SizeOnDisk"(\s*)"(\d+)")re");
        std::smatch m;
        if (!std::regex_search(text, m, sizeRe)) return false;
        if (std::strtoull(m[2].str().c_str(), nullptr, 10) != 0) return false;

        // Only fully installed apps: an app mid-update or mid-verify has a
        // SizeOnDisk Steam is about to rewrite itself.
        static const std::regex stateRe(R"re("StateFlags"(\s*)"(\d+)")re");
        if (!std::regex_search(text, m, stateRe)) return false;
        const auto state = std::strtoul(m[2].str().c_str(), nullptr, 10);
        if ((state & kFullyInstalled) == 0) return false;

        std::string name = acfPath.filename().string();
        static const std::regex nameRe(R"re("name"(\s*)"([^"]*)")re");
        if (std::regex_search(text, m, nameRe)) name = m[2].str();

        uint64_t size = 0;
        static const std::regex dirRe(R"re("installdir"(\s*)"([^"]*)")re");
        if (std::regex_search(text, m, dirRe)) {
            const fs::path installDir =
                acfPath.parent_path() / "common" / std::string(m[2]);
            size = DirectorySize(installDir);
        }
        if (size == 0) size = SumDepotSizes(text);
        if (size == 0) {
            LOG_WARN("AcfSizeRepair: {} ({}) reports SizeOnDisk=0 and no "
                     "measurable size - left as is",
                     acfPath.filename().string(), name);
            return false;
        }

        // One-time backup of the untouched original.
        std::error_code ec;
        const fs::path bak = fs::path(acfPath.native() + L".bak");
        if (!fs::exists(bak, ec))
            fs::copy_file(acfPath, bak, ec);

        const std::string value = std::to_string(size);
        const std::string rewritten = std::regex_replace(
            text, sizeRe, std::string("\"SizeOnDisk\"$1\"") + value + "\"");

        if (!WriteAllText(acfPath, rewritten)) {
            LOG_WARN("AcfSizeRepair: failed to write {}",
                     acfPath.filename().string());
            return false;
        }
        LOG_INFO("AcfSizeRepair: {} ({}) SizeOnDisk 0 -> {}",
                 acfPath.filename().string(), name, value);
        return true;
    }

} // namespace

int RepairAll(const std::string& steamRoot) {
    const fs::path root = fs::path(OSTPlatform::Encoding::Utf8ToWide(steamRoot));
    std::vector<fs::path> libraries;
    std::error_code ec;

    const fs::path rootSteamapps = root / "steamapps";
    if (fs::is_directory(rootSteamapps, ec))
        libraries.push_back(rootSteamapps);

    // Every additional library listed in libraryfolders.vdf. The VDF escapes
    // backslashes as \\; Utf8ToWide keeps non-ASCII paths usable.
    std::string vdf;
    if (ReadAllText(rootSteamapps / "libraryfolders.vdf", &vdf)) {
        static const std::regex pathRe(R"re("path"(\s+)"((?:[^"\\]|\\.)*)")re");
        for (std::sregex_iterator it(vdf.begin(), vdf.end(), pathRe), n;
             it != n; ++it) {
            const std::string escaped = (*it)[2].str();
            std::string path;
            path.reserve(escaped.size());
            for (size_t i = 0; i < escaped.size(); ++i) {
                if (escaped[i] == '\\' && i + 1 < escaped.size() &&
                    escaped[i + 1] == '\\') {
                    path += '\\';
                    ++i;
                } else {
                    path += escaped[i];
                }
            }
            const fs::path steamapps =
                fs::path(OSTPlatform::Encoding::Utf8ToWide(path)) / "steamapps";
            if (!fs::is_directory(steamapps, ec)) continue;
            bool duplicate = false;
            for (const auto& library : libraries) {
                if (library == steamapps) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) libraries.push_back(steamapps);
        }
    }

    int repaired = 0;
    for (const auto& steamapps : libraries) {
        fs::directory_iterator it(steamapps, ec);
        if (ec) {
            LOG_WARN("AcfSizeRepair: cannot list {}: {}",
                     steamapps.string(), ec.message());
            continue;
        }
        for (const auto& entry : it) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc) continue;
            const std::string filename = entry.path().filename().string();
            if (filename.rfind("appmanifest_", 0) != 0) continue;
            if (!EndsWith(filename, ".acf")) continue;
            if (RepairFile(entry.path())) ++repaired;
        }
    }
    return repaired;
}

} // namespace AcfSizeRepair
