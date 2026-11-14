// SPDX-FileCopyrightText: Copyright 2026 RyGT Team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Vulkan {
// Startup-local, immutable disk-read reuse. Reads and waits never hold the map
// lock. Concurrent requests share one in-flight read; failed reads can retry.
// Eviction drops only the cache's reference, so queued users keep their bytes.
template<class Key, class Value> class BoundedReadCache {
    using Result = std::shared_ptr<const Value>;
    struct Entry {
        std::shared_future<Result> result;
        std::size_t bytes{};
        std::uint64_t stamp{};
        bool complete{};
    };
    std::mutex mutex;
    std::unordered_map<Key, std::shared_ptr<Entry>> entries;
    std::size_t retained{}, max_bytes, max_entries;
    std::uint64_t clock{};
    void Trim() {
        while (retained > max_bytes || entries.size() > max_entries) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second->complete && (oldest == entries.end() ||
                    it->second->stamp < oldest->second->stamp)) oldest = it;
            }
            if (oldest == entries.end()) break; // only caller-owned in-flight reads remain
            retained -= oldest->second->bytes;
            entries.erase(oldest);
        }
    }
public:
    BoundedReadCache(std::size_t bytes, std::size_t count) : max_bytes(bytes), max_entries(count) {}
    template<class Load, class Weight>
    Result Get(const Key& key, Load&& load, Weight&& weight) {
        std::shared_ptr<Entry> entry;
        std::shared_ptr<std::promise<Result>> producer;
        {
            std::scoped_lock lock{mutex};
            auto it = entries.find(key);
            if (it != entries.end()) entry = it->second;
            else {
                producer = std::make_shared<std::promise<Result>>();
                entry = std::make_shared<Entry>();
                entry->result = producer->get_future().share();
                entries.emplace(key, entry);
            }
            entry->stamp = ++clock;
        }
        if (!producer) return entry->result.get();
        try {
            auto value = std::make_shared<const Value>(load());
            const auto bytes = weight(*value);
            producer->set_value(value);
            {
                std::scoped_lock lock{mutex};
                entry->bytes = bytes;
                entry->complete = true;
                retained += bytes;
                Trim();
            }
            return value;
        } catch (...) {
            producer->set_exception(std::current_exception());
            std::scoped_lock lock{mutex};
            auto it = entries.find(key);
            if (it != entries.end() && it->second == entry) entries.erase(it);
            throw;
        }
    }
};
}
