#pragma once

#include <string>

// AcfSizeRepair — fix library entries whose appmanifest_*.acf reports
// SizeOnDisk=0.
//
// Steam writes SizeOnDisk when an install/update passes through its real
// disk-write path. Installations that get stamped FullyInstalled without that
// pass (the "instant-complete" window documented in
// Hooks_Manifest::BuildDepotDependency) keep SizeOnDisk=0 forever and the
// library shows the game as 0 B. Steam's own verify does not reliably
// recompute it either — observed recomputing for one game and leaving 0 for
// another with the same state.
//
// RepairAll() scans every appmanifest_*.acf in every library, recomputes the
// installed size from the install directory on disk (falling back to the sum
// of InstalledDepots/*/size), writes a one-time .bak, and rewrites
// SizeOnDisk atomically. Idempotent: entries that already have a non-zero
// size are left alone, so a second run does nothing.
namespace AcfSizeRepair {

    // Scan <steamRoot> and every library listed in its libraryfolders.vdf.
    // Returns the number of appmanifest files repaired. Best-effort: any
    // unreadable file or directory is skipped and logged, never thrown.
    int RepairAll(const std::string& steamRoot);

}
