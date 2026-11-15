// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later

// FEAT-GT7-186: the trophy pop-up, drawn like the owner's Trophy-Notifier (src/toast.c there):
// layout in 1080p units scaled to the output height, card 660x112 radius 16, 28 px from the top
// and 30 px from the right, art 72 px, grade cup 30 px; in 340 ms ease-out sliding 48 px from the
// right with a fade, hold 4 s (+2.5 s for Platinum), out 280 ms ease-in; one Platinum light sweep.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include <imgui.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "imgui/imgui_layer.h"
#include "imgui/imgui_texture.h"
#include "imgui/trophy_toast_layer.h"

namespace ImGui::TrophyToast {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float CardW = 660.0f;
constexpr float CardH = 112.0f;
constexpr float Radius = 16.0f;
constexpr float Top = 28.0f;
constexpr float Right = 30.0f;
constexpr float InMs = 340.0f;
constexpr float OutMs = 280.0f;
constexpr float HoldMs = 4000.0f;
constexpr float PlatinumExtraMs = 2500.0f;
constexpr float SlidePx = 48.0f;
constexpr std::size_t QueueMax = 32;

// Trophy-Notifier's src/toast.c draws "Trophy earned!" / "All trophies earned!"; the brief for this
// port asked for these PS5 wordings instead.
constexpr const char* SubtitleTrophy = "You earned a trophy!";
constexpr const char* SubtitlePlatinum = "You earned the Platinum trophy!";

struct Toast {
    std::string name;
    std::string icon; // UTF-8 path, may be empty
    char grade = 'B';
};

int GradeIndex(char grade) {
    switch (grade) {
    case 'S':
    case 's':
        return 1;
    case 'G':
    case 'g':
        return 2;
    case 'P':
    case 'p':
        return 3;
    default:
        return 0;
    }
}

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_TROPHY_TOAST");
        return !(env && env[0] == '0');
    }();
    return on;
}

std::filesystem::path FromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool FileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

float EaseOut(float t) {
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

float EaseIn(float t) {
    return t * t * t;
}

ImU32 Col(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 255.0f)));
}

class TrophyToastUI final : public ImGui::Layer {
public:
    void Draw() override;
    bool ShouldKeepDrawing() override {
        // Keeps the pop-up moving while the game shows no new frame (a load).
        return active_.load(std::memory_order_relaxed);
    }

    void LoadAssets();
    void Push(Toast t);

private:
    void Start(Toast t);
    void PlayGradeSound(int grade);
    void DrawCup(ImDrawList* dl, int grade, ImVec2 pos, float size, float alpha);
    void DrawLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float max_w, ImU32 col,
                  const std::string& text);

    std::mutex mutex_;
    std::deque<Toast> queue_;

    // Render thread only (active_ is also read by the video out thread's MustKeepDrawing).
    std::atomic<bool> active_{false};
    Toast current_;
    RefCountedTexture art_;
    Clock::time_point t0_;
    RefCountedTexture cups_[4];
    ImFont* font_ = nullptr;
    std::filesystem::path wav_trophy_;
    std::filesystem::path wav_platinum_;
    Clock::time_point registered_;
    bool test_pending_ = false;
    char test_grade_ = 'G';
};

TrophyToastUI g_layer;

void TrophyToastUI::LoadAssets() {
    registered_ = Clock::now();
    if (const char* env = std::getenv("RYGT_TROPHY_TOAST_TEST"); env && env[0] != '\0' &&
                                                                  env[0] != '0') {
        test_pending_ = true;
        if (env[0] == 'B' || env[0] == 'S' || env[0] == 'G' || env[0] == 'P') {
            test_grade_ = env[0];
        }
    }
    // FEAT-GT7-189: each file comes from the player's custom folder (user\trophy_assets) when it
    // is there, else from the launcher's built-in set (user\trophy_builtin, written at Play).
    // Older names (Trophy-Notifier's) are still accepted in the custom folder.
    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    const auto custom = user_dir / "trophy_assets";
    const auto builtin = user_dir / "trophy_builtin";
    int custom_files = 0;
    const auto pick = [&](std::initializer_list<const char*> names) -> std::filesystem::path {
        for (const char* name : names) {
            if (FileExists(custom / name)) {
                ++custom_files;
                return custom / name;
            }
        }
        for (const char* name : names) {
            if (FileExists(builtin / name)) {
                return builtin / name;
            }
        }
        return {};
    };
    const auto font_path = pick({"font.ttf", "font.otf", "fonts/SSTRg.ttf"});
    if (!font_path.empty()) {
        // ImGui 1.92 renders any size from one font, so it is added once (sizes come with AddText).
        ImFontConfig cfg{};
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        const auto font_u8 = font_path.u8string();
        const std::string utf8(font_u8.begin(), font_u8.end());
        font_ = ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8.c_str(), 30.0f, &cfg);
    }
    static const char* const cup_files[4] = {"bronze.png", "silver.png", "gold.png",
                                             "platinum.png"};
    int cups = 0;
    for (int i = 0; i < 4; ++i) {
        if (const auto p = pick({cup_files[i]}); !p.empty()) {
            cups_[i] = RefCountedTexture::DecodePngFile(p);
            ++cups;
        }
    }
    wav_trophy_ = pick({"trophy.wav", "ps5_trophy.wav"});
    wav_platinum_ = pick({"platinum.wav", "ps5_platinum.wav"});
    LOG_INFO(Lib_NpTrophy,
             "FEAT-GT7-186 trophy toast: {}; font {}, {} cup images, sounds {}/{}, {} custom "
             "file(s) from {}{}",
             Enabled() ? "on" : "off (RYGT_TROPHY_TOAST=0)",
             font_ ? Common::FS::PathToUTF8String(font_path.filename()) : std::string("default"),
             cups, wav_trophy_.empty() ? "no" : "yes", wav_platinum_.empty() ? "no" : "yes",
             custom_files, Common::FS::PathToUTF8String(custom),
             test_pending_ ? "; test toast in 5 s (RYGT_TROPHY_TOAST_TEST)" : "");
}

void TrophyToastUI::Push(Toast t) {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= QueueMax) {
        LOG_WARNING(Lib_NpTrophy, "FEAT-GT7-186 trophy toast queue full, dropped one");
        return;
    }
    queue_.push_back(std::move(t));
}

void TrophyToastUI::PlayGradeSound(int grade) {
#ifdef _WIN32
    const auto& wav = grade == 3 ? wav_platinum_ : wav_trophy_;
    if (wav.empty()) {
        return;
    }
    // winmm is loaded here rather than linked, so the renderer does not import it at start.
    using PlaySoundWFn = BOOL(WINAPI*)(LPCWSTR, HMODULE, DWORD);
    static const auto play = [] {
        HMODULE winmm = LoadLibraryW(L"winmm.dll");
        return winmm ? reinterpret_cast<PlaySoundWFn>(GetProcAddress(winmm, "PlaySoundW"))
                     : nullptr;
    }();
    // mmsystem.h values (WIN32_LEAN_AND_MEAN leaves the header out).
    constexpr DWORD SndAsync = 0x0001, SndNoDefault = 0x0002, SndFilename = 0x00020000;
    if (play) {
        play(wav.c_str(), nullptr, SndAsync | SndFilename | SndNoDefault);
    }
#else
    (void)grade;
#endif
}

void TrophyToastUI::Start(Toast t) {
    current_ = std::move(t);
    art_ = RefCountedTexture{};
    const auto icon = FromUtf8(current_.icon);
    if (FileExists(icon)) {
        art_ = RefCountedTexture::DecodePngFile(icon);
    }
    t0_ = Clock::now();
    active_ = true;
    PlayGradeSound(GradeIndex(current_.grade));
}

void TrophyToastUI::DrawCup(ImDrawList* dl, int grade, ImVec2 pos, float size, float alpha) {
    if (cups_[grade]) {
        const auto img = cups_[grade].GetTexture();
        if (img.im_id && img.width && img.height) {
            // Fitted into the square, keeping its aspect.
            const float k = size / static_cast<float>(std::max(img.width, img.height));
            const float w = img.width * k, h = img.height * k;
            const ImVec2 p0(pos.x + (size - w) * 0.5f, pos.y + (size - h) * 0.5f);
            dl->AddImage(img.im_id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                         Col(255, 255, 255, 255.0f * alpha));
            return;
        }
    }
    // No cup image: a coloured disc in the grade's colour.
    static const int colors[4][3] = {{205, 127, 50}, {192, 192, 200}, {230, 190, 60},
                                     {170, 195, 235}};
    const auto& c = colors[grade];
    const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.42f;
    dl->AddCircleFilled(center, r, Col(c[0], c[1], c[2], 255.0f * alpha), 32);
    dl->AddCircle(center, r, Col(c[0] / 2, c[1] / 2, c[2] / 2, 200.0f * alpha), 32,
                  std::max(1.0f, size * 0.06f));
}

void TrophyToastUI::DrawLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float max_w,
                             ImU32 col, const std::string& text) {
    if (!font) {
        font = ImGui::GetFont();
    }
    const char* begin = text.c_str();
    const char* end = begin + text.size();
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end).x <= max_w) {
        dl->AddText(font, size, pos, col, begin, end);
        return;
    }
    // Trimmed with an ellipsis at a character boundary, like GDI+'s EllipsisCharacter trimming.
    const char* dots = "...";
    const float dots_w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, dots).x;
    const char* cut = end;
    while (cut > begin) {
        --cut;
        while (cut > begin && (static_cast<unsigned char>(*cut) & 0xC0) == 0x80) {
            --cut;
        }
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, cut).x + dots_w <= max_w) {
            break;
        }
    }
    std::string trimmed(begin, cut);
    trimmed += dots;
    dl->AddText(font, size, pos, col, trimmed.c_str());
}

void TrophyToastUI::Draw() {
    const auto now = Clock::now();
    if (test_pending_ && now - registered_ >= std::chrono::seconds{5}) {
        test_pending_ = false;
        Push(Toast{test_grade_ == 'P' ? "Trophy toast test (Platinum)" : "Trophy toast test",
                   std::string{}, test_grade_});
    }
    if (!active_) {
        Toast next;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                return;
            }
            next = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!Enabled()) {
            return;
        }
        Start(std::move(next));
    }

    const int grade = GradeIndex(current_.grade);
    const bool plat = grade == 3;
    const float ms = std::chrono::duration<float, std::milli>(now - t0_).count();
    const float hold = HoldMs + (plat ? PlatinumExtraMs : 0.0f);
    float dx = 0.0f, a = 1.0f;
    if (ms < InMs) {
        const float p = EaseOut(ms / InMs);
        dx = (1.0f - p) * SlidePx;
        a = p;
    } else if (ms > InMs + hold) {
        float p = std::clamp((ms - InMs - hold) / OutMs, 0.0f, 1.0f);
        if (p >= 1.0f) {
            active_ = false;
            art_ = RefCountedTexture{};
            return;
        }
        p = EaseIn(p);
        dx = p * SlidePx;
        a = 1.0f - p;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp));
    const float s = vp->Size.y / 1080.0f;
    const float cw = CardW * s, ch = CardH * s;
    const float x = vp->Pos.x + vp->Size.x - Right * s - cw + dx * s;
    const float y = vp->Pos.y + Top * s;

    // Soft shadow: eight widening rounded rectangles, as in toast.c.
    for (int i = 8; i >= 1; --i) {
        const float fi = static_cast<float>(i);
        const ImVec2 p0(x - fi * s, y + (4.0f - fi) * s + 2.0f * s);
        dl->AddRectFilled(p0, ImVec2(p0.x + cw + 2.0f * fi * s, p0.y + ch + 2.0f * fi * s),
                          Col(0, 0, 0, 10.0f * a), (Radius + fi) * s);
    }
    // Card.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + ch), Col(28, 28, 30, 232.0f * a),
                      Radius * s);

    // Trophy art, 72 px at x + 20, centred vertically, corners rounded 6 px.
    const float as = 72.0f * s;
    const ImVec2 a0(x + 20.0f * s, y + (CardH - 72.0f) * 0.5f * s);
    const ImVec2 a1(a0.x + as, a0.y + as);
    const auto art = art_ ? art_.GetTexture() : RefCountedTexture::Image{};
    if (art.im_id) {
        dl->AddImageRounded(art.im_id, a0, a1, ImVec2(0, 0), ImVec2(1, 1),
                            Col(255, 255, 255, 255.0f * a), 6.0f * s);
    } else {
        dl->AddRectFilled(a0, a1, Col(44, 46, 54, 255.0f * a), 6.0f * s);
        DrawCup(dl, grade, ImVec2(a0.x + 8.0f * s, a0.y + 8.0f * s), as - 16.0f * s, a);
    }

    // Grade cup next to the title; none for Platinum (the big trophy is the art).
    const float tx = x + 114.0f * s;
    if (!plat) {
        DrawCup(dl, grade, ImVec2(tx, y + 22.0f * s), 30.0f * s, a);
    }

    // Title (the trophy's name) and subtitle. Names in scripts SST does not cover (CJK and the
    // like: UTF-8 lead bytes from 0xE0) use the default UI font, which carries them.
    bool wide_script = false;
    for (const unsigned char c : current_.name) {
        if (c >= 0xE0) {
            wide_script = true;
            break;
        }
    }
    const float ttx = plat ? tx : tx + 38.0f * s;
    const float text_right = x + cw - 20.0f * s;
    DrawLabel(dl, wide_script ? nullptr : font_, 30.0f * s, ImVec2(ttx, y + 17.0f * s),
             text_right - ttx, Col(245, 245, 247, 255.0f * a), current_.name);
    DrawLabel(dl, font_, 24.0f * s, ImVec2(tx, y + 59.0f * s), text_right - tx,
             Col(178, 178, 184, 255.0f * a), plat ? SubtitlePlatinum : SubtitleTrophy);

    // Platinum: one light sweep across the card at 2.5 s, lasting 0.9 s.
    if (plat) {
        const float sp = (ms - 2500.0f) / 900.0f;
        if (sp > 0.0f && sp < 1.0f) {
            dl->PushClipRect(ImVec2(x, y), ImVec2(x + cw, y + ch), true);
            const float sx = x - 120.0f * s + sp * (cw + 240.0f * s);
            for (int i = 0; i < 3; ++i) {
                const float fi = static_cast<float>(i);
                const float wv = (70.0f - fi * 20.0f) * s;
                const ImVec2 pts[4] = {ImVec2(sx + fi * 10.0f * s, y), ImVec2(sx + wv, y),
                                       ImVec2(sx + wv - 50.0f * s, y + ch),
                                       ImVec2(sx - 50.0f * s + fi * 10.0f * s, y + ch)};
                dl->AddConvexPolyFilled(pts, 4, Col(255, 255, 255, 12.0f * a));
            }
            dl->PopClipRect();
        }
    }
}

} // namespace

void Register() {
    g_layer.LoadAssets();
    ImGui::Layer::AddLayer(&g_layer);
}

void Unregister() {
    ImGui::Layer::RemoveLayer(&g_layer);
}

void Push(std::string name, std::string icon_path_utf8, char grade) {
    g_layer.Push(Toast{std::move(name), std::move(icon_path_utf8), grade});
}

} // namespace ImGui::TrophyToast

// FEAT-GT7-186: called by libSceNpAuth.prx (ryty_np_shims.cpp AddTrophyToQueue) through
// GetProcAddress, like RytyGpuNeoSupported. grade is 'B', 'S', 'G' or 'P'.
#ifdef _WIN32
#define RYTY_TOAST_EXPORT __declspec(dllexport)
#else
#define RYTY_TOAST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" RYTY_TOAST_EXPORT void RytyTrophyToast(const char* utf8_name, const char* utf8_icon_path,
                                                   char grade) {
    ImGui::TrophyToast::Push(utf8_name ? utf8_name : "", utf8_icon_path ? utf8_icon_path : "",
                             grade);
}
