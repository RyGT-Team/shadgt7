// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace Vulkan {
// Only file preparation is parallel. Installation stays on its owning thread,
// in source order, with at most window claimed records (including active reads).
template<class Prepare, class Consume, class Init>
bool OrderedParallelRead(size_t count, unsigned workers, size_t window,
                         std::stop_token stop, Prepare prepare, Consume consume, Init init) {
    using Result = std::invoke_result_t<Prepare, size_t>;
    struct Slot { std::optional<Result> value; std::exception_ptr error; bool ready=false; };
    window=std::max<size_t>(window,1);
    workers=static_cast<unsigned>(std::min<size_t>(std::max(workers,1u),count));
    std::vector<Slot> slots(window);
    std::mutex mutex;
    std::condition_variable_any cv;
    size_t issued=0, consumed=0;
    std::stop_callback wake{stop,[&]{cv.notify_all();}};
    std::vector<std::jthread> threads;
    for(unsigned w=0;w<workers;++w) threads.emplace_back([&](std::stop_token local) {
        init();
        while(!stop.stop_requested() && !local.stop_requested()) {
            size_t index;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock,local,[&]{return stop.stop_requested() || issued==count || issued-consumed<window;});
                if(stop.stop_requested() || local.stop_requested() || issued==count) return;
                index=issued++;
            }
            std::optional<Result> result;
            std::exception_ptr error;
            try { result.emplace(prepare(index)); } catch(...) { error=std::current_exception(); }
            {
                std::scoped_lock lock{mutex};
                auto& slot=slots[index%window];
                slot.value=std::move(result);slot.error=error;slot.ready=true;
            }
            cv.notify_all();
        }
    });
    for(size_t i=0;i<count;++i) {
        std::unique_lock lock{mutex};
        cv.wait(lock,stop,[&]{return slots[i%window].ready;});
        if(stop.stop_requested()) return false;
        auto& slot=slots[i%window];
        if(slot.error) std::rethrow_exception(slot.error);
        auto value=std::move(*slot.value);
        slot.value.reset();slot.ready=false;
        ++consumed;
        lock.unlock();cv.notify_all();
        consume(i,std::move(value));
    }
    return true;
}
}
