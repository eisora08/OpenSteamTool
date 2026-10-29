#include "AppUpdater.h"

#include "OpenSteamToolBuildInfo.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Process.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/Mirror.h"

#include <toml++/toml.hpp>

#include <windows.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace AppUpdater {

namespace {
    // Bounds for a sane framework DLL payload — guards against a truncated or hostile
    // response being written to disk. OpenSteamTool.dll is ~1 MB.
    constexpr size_t kMinDllBytes = 200 * 1024;
    constexpr size_t kMaxDllBytes = 8 * 1024 * 1024;

    // The companion loader DLLs are much smaller than the main one (~110-140 KB),
    // so they get their own floor; the SHA-256 check remains the real guard.
    constexpr size_t kMinCompanionBytes = 40 * 1024;

    // Companion loader DLLs, paired with the pointer key that carries each
    // digest. Their relative path is derived from the main DLL's directory, so
    // they always ship inside the same version folder.
    struct CompanionSpec { const char* name; const char* shaKey; };
    constexpr CompanionSpec kCompanions[] = {
        { "dwmapi.dll",    "sha256_dwmapi"    },
        { "xinput1_4.dll", "sha256_xinput1_4" },
    };

    constexpr const char* kPointerPath = "opensteamtool/latest.toml";

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    std::string EscapeForPowerShellSingleQuoted(std::string s)
    {
        // Inside a PowerShell single-quoted string, ' is escaped by doubling it.
        for (size_t p = s.find('\''); p != std::string::npos; p = s.find('\'', p + 2))
            s.insert(p, 1, '\'');
        return s;
    }

    // Fetch `relPath`, validate it (size floor/ceiling, MZ header, SHA-256), then
    // rename-aside whatever sits at `targetPath` and write the new bytes at that
    // canonical path. Every failure path restores the original file, so a live
    // install is never left half-replaced.
    bool StageOneFile(const std::string& relPath, const std::string& expectedSha,
                      const std::string& targetPath, size_t minBytes)
    {
        std::optional<std::string> download = Mirror::Fetch(relPath);
        if (!download) {
            LOG_WARN("AppUpdater: download failed for {}", relPath);
            return false;
        }
        const std::string& body = *download;

        if (body.size() < minBytes || body.size() > kMaxDllBytes) {
            LOG_WARN("AppUpdater: rejected {} (suspicious size {} bytes)", relPath, body.size());
            return false;
        }
        if (body.size() < 2 || body[0] != 'M' || body[1] != 'Z') {
            LOG_WARN("AppUpdater: rejected {} (missing MZ header)", relPath);
            return false;
        }

        const std::string actual = OSTPlatform::Hash::Sha256OfBuffer(body.data(), body.size());
        if (actual.empty() || !EqualsIgnoreCase(actual, expectedSha)) {
            LOG_WARN("AppUpdater: SHA-256 mismatch for {} (expected {}, got {})",
                     relPath, expectedSha, actual);
            return false;
        }

        // The mapped file is write-locked while loaded, but NTFS allows renaming a
        // loaded image, so move it aside and write the new bytes at the canonical path.
        const std::string backup = targetPath + ".old";
        if (!MoveFileExA(targetPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            LOG_WARN("AppUpdater: could not rename {} to backup (error={})",
                     targetPath, GetLastError());
            return false;
        }

        {
            std::ofstream ofs(targetPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                LOG_WARN("AppUpdater: could not open {} for writing; restoring backup", targetPath);
                MoveFileExA(backup.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                return false;
            }
            ofs.write(body.data(), static_cast<std::streamoff>(body.size()));
            ofs.flush();
            if (!ofs) {
                LOG_WARN("AppUpdater: write failed for {}; restoring backup", targetPath);
                ofs.close();
                MoveFileExA(backup.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                return false;
            }
        }

        LOG_INFO("AppUpdater: staged {} ({} bytes); applies on next Steam start",
                 targetPath, body.size());
        return true;
    }
} // namespace

CheckResult Check()
{
    CheckResult r;
    r.oldVersion = OPENSTEAMTOOL_VERSION;

    std::optional<std::string> body = Mirror::Fetch(kPointerPath);
    if (!body) {
        LOG_WARN("AppUpdater: {} unavailable from all mirrors", kPointerPath);
        return r;
    }

    toml::table tbl;
    try {
        tbl = toml::parse(*body);
    } catch (const toml::parse_error& e) {
        LOG_WARN("AppUpdater: latest.toml parse error: {}", e.description());
        return r;
    }

    const auto version = tbl["version"].value<std::string>();
    const auto path    = tbl["path"].value<std::string>();
    const auto sha     = tbl["sha256"].value<std::string>();
    if (!version || !path || !sha || version->empty() || path->empty() || sha->empty()) {
        LOG_WARN("AppUpdater: latest.toml missing version/path/sha256");
        return r;
    }

    r.newVersion = *version;
    r.dllRelPath = *path;
    r.sha256     = *sha;

    // Companion loader DLLs live next to the main one. Their digests are
    // optional: a pointer written by an older feed carries none and simply
    // stages the main DLL alone.
    if (const size_t slash = r.dllRelPath.rfind('/'); slash != std::string::npos) {
        const std::string dir = r.dllRelPath.substr(0, slash + 1);
        for (const auto& spec : kCompanions) {
            if (const auto d = tbl[spec.shaKey].value<std::string>(); d && !d->empty())
                r.companions.push_back({ dir + spec.name, *d });
        }
    }

    // latest.toml is authoritative for "newest"; build tags are not semver, so any
    // difference from the running version means there is something new to stage.
    if (r.newVersion == r.oldVersion) {
        LOG_INFO("AppUpdater: up to date (version {})", r.oldVersion);
        return r;
    }

    LOG_INFO("AppUpdater: update available {} -> {}", r.oldVersion, r.newVersion);
    r.updateAvailable = true;
    return r;
}

bool DownloadAndStage(const CheckResult& result, const std::string& selfDllPath)
{
    // Main DLL first: if this fails, the live install is untouched and nothing
    // else is staged.
    if (!StageOneFile(result.dllRelPath, result.sha256, selfDllPath, kMinDllBytes))
        return false;

    // Companions are best effort. The main DLL is already staged at this
    // point, so a failure here must not roll the update back — it is retried
    // on the next update attempt instead.
    const std::filesystem::path dir = std::filesystem::path(selfDllPath).parent_path();
    for (const auto& c : result.companions) {
        const std::string target =
            (dir / std::filesystem::path(c.relPath).filename()).string();
        if (!StageOneFile(c.relPath, c.sha256, target, kMinCompanionBytes))
            LOG_WARN("AppUpdater: companion {} not staged; retried on the next update",
                     target);
    }
    return true;
}

void CleanupStagedBackup(const std::string& selfDllPath)
{
    std::error_code ec;
    auto removeOne = [&ec](const std::filesystem::path& p) {
        if (std::filesystem::exists(p, ec) && DeleteFileA(p.string().c_str()))
            LOG_INFO("AppUpdater: removed stale backup {}", p.string());
    };

    const std::filesystem::path self(selfDllPath);
    removeOne(self.string() + ".old");

    // Companions leave their own "<name>.old" behind; sweep those too so a
    // finished update leaves no stray files in the Steam root.
    const std::filesystem::path dir = self.parent_path();
    for (const auto& spec : kCompanions)
        removeOne(dir / (std::string(spec.name) + ".old"));
}

void RestartSteam()
{
    const std::filesystem::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
    if (steamExe.empty()) {
        LOG_WARN("AppUpdater: steam.exe path unknown; cannot auto-restart");
        return;
    }

    const std::string exe = EscapeForPowerShellSingleQuoted(steamExe.string());
    // Detached, hidden PowerShell: graceful -shutdown, wait for every steam process to
    // exit (covers steamwebhelper), then relaunch.
    const std::string command =
        "powershell -NoProfile -WindowStyle Hidden -Command "
        "\"& '" + exe + "' -shutdown; "
        "Wait-Process -Name steam -Timeout 30 -ErrorAction SilentlyContinue; "
        "Start-Process '" + exe + "'\"";

    if (OSTPlatform::Process::LaunchDetachedHidden(command))
        LOG_INFO("AppUpdater: restart helper launched");
    else
        LOG_WARN("AppUpdater: restart helper failed to launch; "
                 "update applies on next manual Steam start");
}

} // namespace AppUpdater
