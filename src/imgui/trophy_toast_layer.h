// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace ImGui::TrophyToast {

/// FEAT-GT7-186: the PS5-style trophy pop-up (the look of the owner's Trophy-Notifier): one card
/// at a time in the top-right corner, queued, with the trophy's art, its grade cup and name, and
/// the grade's sound. The NP library (libSceNpAuth.prx) cannot draw into this library's ImGui
/// context, so it calls the exported RytyTrophyToast instead (ryty_np_shims.cpp AddTrophyToQueue).
/// Assets from <root>\user\trophy_assets\ (fonts\SSTRg.ttf, bronze/silver/gold/platinum.png,
/// ps5_trophy.wav, ps5_platinum.wav) when present; drawn cups, the default font and no sound
/// otherwise. RYGT_TROPHY_TOAST=0 hides it; RYGT_TROPHY_TOAST_TEST=1 shows a demo 5 s after start.
void Register();
void Unregister();

/// Thread-safe; grade is 'B', 'S', 'G' or 'P'.
void Push(std::string name, std::string icon_path_utf8, char grade);

} // namespace ImGui::TrophyToast
