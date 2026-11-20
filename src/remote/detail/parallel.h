#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// ThreadPoolExecutor(max_workers).map(fn, items) for the client's scans: every item's result in
// the items' order, at most `workers` at a time, the first exception raised again once every
// worker has stopped.
namespace fv::remote::detail {

template <class Result, class Item, class F>
std::vector<Result> parallel_map(std::size_t workers, const std::vector<Item>& items, F fn) {
    std::vector<Result> results(items.size());
    if (items.empty()) {
        return results;
    }
    std::atomic<std::size_t> next{0};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto run = [&] {
        while (true) {
            const std::size_t index = next.fetch_add(1);
            if (index >= items.size()) {
                return;
            }
            try {
                results[index] = fn(items[index]);
            } catch (...) {
                std::lock_guard lock(failure_mutex);
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
    };
    std::vector<std::thread> threads;
    const std::size_t count = std::max<std::size_t>(1, std::min(workers, items.size()));
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads.emplace_back(run);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    return results;
}

} // namespace fv::remote::detail
