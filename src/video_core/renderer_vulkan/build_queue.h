// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>

#include "common/types.h"

namespace Vulkan {

/// FIX-GT7-100: pipeline builds queued or running, for the compile notice.
inline std::atomic<u32> g_build_jobs{0};

/// FIX-GT7-109: the game is in play (a race: slow frames for a second; FIX-GT7-120: kept by a
/// heavy scene), as PipelineCache::UpdateCatchUp measures it. Compile work then stays on its
/// cores at its usual width, and background compiles that read it pause.
inline std::atomic<bool> g_game_in_play{false};

/// The pipeline build workers' jobs (PERF-019). Urgent jobs (a draw waits) run first, the last
/// one first; then FIX-GT7-009's screen ones (draws skipped again: each builds the one on screen
/// skipped most) in order; then PERF-GT7-025's soon ones (draws that went on without their
/// pipeline) in draw order; then PERF-171's tessellated read-ahead ones (the cars) in order; then
/// normal ones (read-ahead) in order; on at most `max_running` workers at once. FIX-GT7-022: the
/// read-ahead ones (tessellated and normal) only while fewer than `background_limit` run.
/// FEAT-GT7-185 (from shadGT PERF-038): background jobs (stored pipelines built at start) run only
/// when no other job waits, on at most `max_background` workers at once, and not while paused:
/// each pipeline a draw needs pauses them for a while, so they never compete with the builds the
/// game is waiting for. PERF-098: while boosted (the splash screen), on up to `max_boost`.
class BuildQueue {
public:
    using Job = std::function<void()>;
    using Clock = std::chrono::steady_clock;
    enum class Kind { Urgent, Screen, Soon, Tessellated, Normal, Background };

    explicit BuildQueue(u32 max_running_, u32 max_background_ = 0, u32 max_boost_ = 0)
        : max_running{max_running_}, max_background{max_background_},
          max_boost{std::max(max_background_, max_boost_)} {}

    /// FEAT-GT7-185 (PERF-098): while the splash screen shows, background jobs run on up to
    /// `max_boost` workers and are never paused (no game draw waits for a pipeline yet).
    bool Boosted() const {
        return boost.load(std::memory_order_relaxed);
    }

    void SetBoost(bool on) {
        if (boost.exchange(on) != on) {
            cv.notify_all();
        }
    }

    void Push(Job&& job, Kind kind) {
        {
            std::scoped_lock lk{mutex};
            if (kind == Kind::Urgent) {
                jobs.push_front(std::move(job));
                ++urgent;
            } else if (kind == Kind::Screen) {
                screen.push_back(std::move(job));
            } else if (kind == Kind::Soon) {
                soon.push_back(std::move(job));
            } else if (kind == Kind::Tessellated) {
                tessellated.push_back(std::move(job));
            } else if (kind == Kind::Background) {
                background.push_back(std::move(job));
            } else {
                jobs.push_back(std::move(job));
            }
        }
        g_build_jobs.fetch_add(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_one();
    }

    /// Waits for the next job. False once stopped. A job must be followed by Finished.
    bool Pop(Job& job, std::stop_token stop) {
        bool is_background = false;
        return Pop(job, is_background, stop);
    }

    /// FEAT-GT7-185: as above; `is_background` tells a background job, which must be followed by
    /// FinishedBackground instead.
    bool Pop(Job& job, bool& is_background, std::stop_token stop) {
        std::unique_lock lk{mutex};
        const auto drawn = [&] { return urgent != 0 || !screen.empty() || !soon.empty(); };
        const auto read_ahead = [&] { return !tessellated.empty() || jobs.size() > urgent; };
        const auto foreground_ready = [&] {
            const u32 limit = max_running + extra_running;
            return (drawn() && running < limit) ||
                   (read_ahead() && running < std::min(limit, background_limit));
        };
        const auto background_limit_now = [&] {
            return boost.load(std::memory_order_relaxed) ? max_boost : max_background;
        };
        const auto background_ready = [&] {
            // Not while a draw's or a read-ahead build waits for a worker.
            const u32 limit = background_limit_now();
            return !background.empty() && !drawn() && !read_ahead() &&
                   running_background < limit &&
                   running + running_background < std::max(max_running, limit);
        };
        while (true) {
            if (stop.stop_requested()) {
                return false;
            }
            if (foreground_ready()) {
                break;
            }
            const bool boosted = boost.load(std::memory_order_relaxed);
            const bool paused = !boosted && Clock::now() < pause_until;
            if (!paused && background_ready()) {
                job = std::move(background.front());
                background.pop_front();
                ++running_background;
                is_background = true;
                return true;
            }
            if (!background.empty() && paused) {
                cv.wait_until(lk, stop, pause_until, [&] {
                    return foreground_ready() || boost.load(std::memory_order_relaxed) ||
                           Clock::now() >= pause_until;
                });
            } else {
                cv.wait(lk, stop, [&] { return foreground_ready() || background_ready(); });
            }
        }
        is_background = false;
        if (urgent == 0 && !screen.empty()) {
            job = std::move(screen.front());
            screen.pop_front();
        } else if (urgent == 0 && !soon.empty()) {
            job = std::move(soon.front());
            soon.pop_front();
        } else if (urgent == 0 && !tessellated.empty()) {
            job = std::move(tessellated.front());
            tessellated.pop_front();
        } else {
            job = std::move(jobs.front());
            jobs.pop_front();
            urgent -= urgent != 0 ? 1 : 0;
        }
        ++running;
        return true;
    }

    void Finished() {
        {
            std::scoped_lock lk{mutex};
            --running;
        }
        g_build_jobs.fetch_sub(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_one();
    }

    /// FEAT-GT7-185: the end of a background job.
    void FinishedBackground() {
        {
            std::scoped_lock lk{mutex};
            --running_background;
        }
        g_build_jobs.fetch_sub(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_all();
    }

    /// FEAT-GT7-185: no background job starts before `until` (jobs already running finish).
    void PauseBackground(Clock::time_point until) {
        std::scoped_lock lk{mutex};
        pause_until = std::max(pause_until, until);
    }

    size_t BackgroundQueued() {
        std::scoped_lock lk{mutex};
        return background.size();
    }

    /// PERF-101: no draw or read-ahead build is queued or running.
    bool ForegroundIdle() {
        std::scoped_lock lk{mutex};
        return jobs.empty() && screen.empty() && soon.empty() && tessellated.empty() &&
               running == 0;
    }

    /// PERF-160: jobs waiting for a worker (FEAT-GT7-185: background ones too).
    size_t Pending() {
        std::scoped_lock lk{mutex};
        return jobs.size() + screen.size() + soon.size() + tessellated.size() + background.size();
    }

    /// Wakes the workers waiting for jobs or a pause, so they can see a stop request.
    void NotifyAll() {
        cv.notify_all();
    }

    /// PERF-GT7-034: the limit itself (fewer while the game runs late).
    void SetMaxRunning(u32 limit) {
        {
            std::scoped_lock lk{mutex};
            max_running = limit;
        }
        cv.notify_all();
    }

    /// PERF-GT7-021: this many more jobs may run at once (0: back to the limit).
    void SetExtraRunning(u32 extra) {
        {
            std::scoped_lock lk{mutex};
            extra_running = extra;
        }
        cv.notify_all();
    }

    /// FIX-GT7-022: a read-ahead job starts only while fewer than this many jobs run.
    void SetBackgroundLimit(u32 limit) {
        {
            std::scoped_lock lk{mutex};
            background_limit = limit;
        }
        cv.notify_all();
    }

private:
    u32 max_running;
    u32 background_limit = ~0u; ///< FIX-GT7-022
    const u32 max_background;   ///< FEAT-GT7-185
    const u32 max_boost;
    std::atomic<bool> boost{false};
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Job> jobs;        ///< urgent ones at the front, normal ones after
    std::deque<Job> screen;      ///< FIX-GT7-009
    std::deque<Job> soon;        ///< PERF-GT7-025
    std::deque<Job> tessellated; ///< PERF-171
    u32 urgent{};                ///< urgent jobs at the front of `jobs`
    u32 running{};
    u32 extra_running{}; ///< PERF-GT7-021
    std::deque<Job> background; ///< FEAT-GT7-185
    u32 running_background{};
    Clock::time_point pause_until{};
};

} // namespace Vulkan
