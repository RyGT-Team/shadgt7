#pragma once

// FIX-GT7-175: the game's sce_sys files (npbind.dat, trophy\*.trp, icon0.png, param.sfo, pic1.png)
// read through the same folder stack the runtime lays over /app0 (rygt7 core/libs/prx/libc
// AppOverlay): ryGT7\app0 first, then the update folder, then the base game folder (RyGT.ini
// update= / game=, or RYGT_APP0_LAYERS). Since FEAT-GTS-251 Set up no longer links the game's files
// into ryGT7\app0, so reading only ryGT7\app0\sce_sys found nothing ("No npCommId found").
// RYGT_SCE_SYS_LAYERS=0 restores the old ryGT7\app0-only lookup.

#include <filesystem>
#include <vector>

namespace Ryty {

/// The folders /app0 is read from, in order: `root`\app0, then the update and base game folders
/// (only `root`\app0 with RYGT_SCE_SYS_LAYERS=0). `root` is the ryGT7 folder.
std::vector<std::filesystem::path> App0Layers(const std::filesystem::path& root);

/// The first layer's copy of `relative` (a path under /app0, e.g. "sce_sys/npbind.dat"), else
/// where it would be in `root`\app0.
std::filesystem::path App0File(const std::filesystem::path& root,
                               const std::filesystem::path& relative);

/// The merged files of the folder `relative` under /app0: a name in an earlier layer hides the
/// same name (compared case-insensitively) in later ones.
std::vector<std::filesystem::path> App0ListFiles(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative);

} // namespace Ryty
