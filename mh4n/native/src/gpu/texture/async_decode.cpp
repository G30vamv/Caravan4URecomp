// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: async_decode.h and cache.h.

#include "async_decode.h"
#include "decode.h"

#include <algorithm>
#include <utility>

namespace mh4n::gpu::texture {

AsyncDecoder::AsyncDecoder() {
    const auto workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    threads_.reserve(workers);
    try {
        for (unsigned index = 0; index < workers; ++index) {
            threads_.emplace_back(&AsyncDecoder::Run, this);
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (auto& worker : threads_) {
            worker.join();
        }
        throw;
    }
}

AsyncDecoder::~AsyncDecoder() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    work_.notify_all();
    for (auto& worker : threads_) {
        worker.join();
    }
}

bool AsyncDecoder::Start(const CacheKey& key, const u8* bytes, std::size_t size) {
    {
        std::lock_guard lock(mutex_);
        if (jobs_.contains(key)) {
            return false;
        }
        Job job{key, {}};
        if (bytes && size != 0) {
            job.bytes.assign(bytes, bytes + size);
        }
        const auto it = jobs_.try_emplace(key).first;
        try {
            queue_.push_back(std::move(job));
        } catch (...) {
            jobs_.erase(it);
            throw;
        }
    }
    work_.notify_one();
    return true;
}

bool AsyncDecoder::Pending(const CacheKey& key) {
    std::lock_guard lock(mutex_);
    return jobs_.contains(key);
}

std::optional<AsyncDecoder::Result> AsyncDecoder::Take(const CacheKey& key) {
    std::lock_guard lock(mutex_);
    const auto it = jobs_.find(key);
    if (it == jobs_.end() || !it->second) {
        return std::nullopt;
    }
    auto result = std::move(it->second);
    jobs_.erase(it);
    return result;
}

void AsyncDecoder::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            work_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        Result result;
        try {
            result.hash = ContentHash(job.bytes.data(), job.bytes.size());
            DecodeToRgba8(job.bytes.data(), job.bytes.size(), job.key.width, job.key.height,
                          job.key.format, result.rgba8);
        } catch (const std::bad_alloc&) {
            result.rgba8.clear();
        }
        std::lock_guard lock(mutex_);
        jobs_.at(job.key) = std::move(result);
    }
}

} // namespace mh4n::gpu::texture
