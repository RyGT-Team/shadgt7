// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// FEAT-GT7-185: trophies in the Ryty build. The launcher passes the trophy key in a temporary file
// (RYGT_TROPHY_KEY_FILE); the first time a context registers, the game's trophyNN.trp (found
// through the /app0 folder stack, FIX-GT7-175) is decrypted into user\trophy\<npCommId>\{Xml,Icons}
// and each player's unlock file (_sd\trophies_progress, FEAT-GT7-189; shadGT's format) is seeded
// from TROP.XML. Without the key nothing happens and trophies stay as before.

#include <filesystem>
#include <string>
#include "common/types.h"

namespace Ryty {

/// The shadGT user folder number for a game user ID: Ryty numbers its players 0x10000000 + slot,
/// shadGT's users.json 1000 + slot. Other IDs are returned unchanged.
s64 TrophyUserFolder(s64 user_id);

/// FEAT-GT7-189: the player's unlock file, next to the saves: _sd\trophies_progress\<npCommId>.xml
/// for the first player, _sd\trophies_progress\<1000 + slot>\<npCommId>.xml for the others.
/// RYGT_TROPHY_PROGRESS_SD=0 returns `legacy` (user\home\<1000 + slot>\trophy) instead.
std::filesystem::path TrophyProgressFile(s64 user_id, const std::string& np_comm_id,
                                         const std::filesystem::path& legacy);

/// Extracts the trophy set `service_label` (npCommId `np_comm_id`) into `extract_dir` if it is not
/// there yet. If `user_xml` does not exist it is copied from `legacy_xml` (earlier progress) when
/// that exists, else seeded from TROP.XML. Extraction needs RYGT_TROPHY_KEY_FILE.
void PrepareTrophies(const std::string& np_comm_id, u32 service_label,
                     const std::filesystem::path& extract_dir,
                     const std::filesystem::path& user_xml,
                     const std::filesystem::path& legacy_xml);

} // namespace Ryty
