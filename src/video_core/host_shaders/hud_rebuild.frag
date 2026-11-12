// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DLSS-GT7-001 (RYGT_DLSS_TEMPORAL_HUD_ALPHA=1, vk_rasterizer_native.cpp): the game's display
// picture rebuilt from the interface picture and the scene. The interface is drawn once, at twice
// the display's size, into a picture whose colour is what it leaves over black and whose alpha is
// how much of what lies under it still shows (cleared to 0, 0, 0, 1). Where the game reads its
// display back (blurred panels, page copies), and at the end of the frame, the display becomes
// interface + transmittance x scene again. Sampled at the display's pixel centres, the larger
// picture gives each pixel the average of its four (linear filtering).

#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D hud;
layout(binding = 1) uniform sampler2D scene;

void main() {
    const vec4 interface_ = texture(hud, uv);
    const vec4 under = texture(scene, uv);
    color = vec4(interface_.rgb + interface_.a * under.rgb, under.a);
}
