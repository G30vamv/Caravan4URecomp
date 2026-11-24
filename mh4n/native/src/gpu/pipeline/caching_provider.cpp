// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
// Our own code, written from: caching_provider.h, provider_host.h, gpu_pipeline_test, existing cache files and the Vulkan pipeline cache header specification.

#include "caching_provider.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "cache_bytes.h"
#include "files.h"
#include "key_codec.h"
#include "mh4n/log.h"

namespace mh4n::gpu::pipeline {
namespace {

using Clock = std::chrono::steady_clock;
using native::PipelineHandle;
using native::PipelineKey;
using native::PipelineReadiness;

constexpr char kLog[] = "Pipelines";
constexpr u64 kFileVersion = 1;

enum class BuildState { New, Queued, Building, Ready, Failed };
enum class Queue { Urgent, Startup };

struct Entry {
    explicit Entry(const PipelineKey& key_) : key(key_) {}

    PipelineKey key;
    BuildState state = BuildState::New;
    Queue queue = Queue::Urgent;
    std::list<Entry*>::iterator position;
    std::shared_ptr<rhi::Pipeline> pipeline;
    std::string error;
    u32 uses = 0;
    u32 order = 0;
    bool listed = false;
};

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

u32 DefaultWorkers() {
    const u32 cores = std::thread::hardware_concurrency();
    return std::clamp(cores / 2, 2u, 5u) - 1;
}

std::filesystem::path DriverFile(rhi::Backend& backend) {
    // The version-one Vulkan header identifies the blob even before the first pipeline is built.
    const auto header = backend.SavePipelineCache();
    if (header.size() < 32) {
        return {};
    }
    disk::Reader in(header);
    const u32 size = in.Read<u32>();
    const u32 version = in.Read<u32>();
    if (size < 32 || size > header.size() || version != 1) {
        return {};
    }
    const u32 vendor = in.Read<u32>();
    const u32 device = in.Read<u32>();
    std::string name = fmt::format("driver-{:04x}-{:04x}-", vendor, device);
    for (const u8 byte : in.Take(8)) {
        name += fmt::format("{:02x}", byte);
    }
    return name + ".bin";
}

class SpirvCache final : public native::SpirvStore {
public:
    bool Find(const native::ShaderTextHash& hash, std::vector<u32>& spirv) override {
        std::lock_guard lock(mutex_);
        const auto it = shaders_.find(hash);
        if (it == shaders_.end()) {
            return false;
        }
        it->second.used = true;
        spirv = it->second.words;
        return true;
    }

    void Put(const native::ShaderTextHash& hash, std::span<const u32> spirv) override {
        std::lock_guard lock(mutex_);
        auto [it, inserted] = shaders_.try_emplace(hash);
        auto& entry = it->second;
        entry.used = true;
        if (inserted || !std::ranges::equal(spirv, entry.words)) {
            words_ -= entry.words.size();
            entry.words.assign(spirv.begin(), spirv.end());
            words_ += entry.words.size();
            ++revision_;
        }
    }

    void Load(const Records& records) {
        for (const auto& record : records) {
            constexpr size_t kHashSize = 24;
            if (record.size() <= kHashSize || (record.size() - kHashSize) % sizeof(u32) != 0) {
                continue;
            }
            disk::Reader in(record);
            native::ShaderTextHash hash;
            hash.a = in.Read<u64>();
            hash.b = in.Read<u64>();
            hash.length = in.Read<u32>();
            const u32 stage = in.Read<u32>();
            if (stage > static_cast<u32>(native::ShaderStage::Compute)) {
                continue;
            }
            hash.stage = static_cast<native::ShaderStage>(stage);
            if (auto [it, inserted] = shaders_.try_emplace(hash); inserted) {
                auto& words = it->second.words;
                words.resize(in.Remaining() / sizeof(u32));
                for (u32& word : words) {
                    word = in.Read<u32>();
                }
                words_ += words.size();
            }
        }
    }

    bool Save(const std::filesystem::path& path, u64 version, std::string& error) {
        Records records;
        u64 revision;
        {
            std::lock_guard lock(mutex_);
            if (revision_ == saved_) {
                return false;
            }
            if (words_ > (64 * 1024 * 1024) / sizeof(u32)) {
                std::erase_if(shaders_, [&](const auto& item) {
                    if (item.second.used) {
                        return false;
                    }
                    words_ -= item.second.words.size();
                    return true;
                });
            }
            revision = revision_;
            records.reserve(shaders_.size());
            for (const auto& [hash, entry] : shaders_) {
                auto& bytes = records.emplace_back();
                bytes.reserve(24 + entry.words.size() * sizeof(u32));
                disk::Append(bytes, hash.a);
                disk::Append(bytes, hash.b);
                disk::Append(bytes, hash.length);
                disk::Append(bytes, static_cast<u32>(hash.stage));
                for (const u32 word : entry.words) {
                    disk::Append(bytes, word);
                }
            }
        }
        if (!WriteRecords(path, kKindSpirv, kFileVersion, version, records, &error)) {
            return false;
        }
        saved_ = revision;
        return true;
    }

private:
    struct Shader {
        std::vector<u32> words;
        bool used = false;
    };

    std::mutex mutex_;
    std::unordered_map<native::ShaderTextHash, Shader, native::ShaderTextHashHasher> shaders_;
    size_t words_ = 0;
    u64 revision_ = 0;
    u64 saved_ = 0;
};

}  // namespace

class CachingProvider final : public native::PipelineProvider {
public:
    CachingProvider(const native::ProviderHost& host, const CachingOptions& options)
        : host_(host), options_(options),
          worker_count_(options_.background ? (options_.workers == 0 ? DefaultWorkers() : options_.workers) : 0) {
        Load();
        if (options_.spirv_cache && host_.set_spirv_store) {
            host_.set_spirv_store(&spirv_);
        }
    }

    void Start() {
        for (u32 i = 0; i < worker_count_; ++i) {
            workers_.emplace_back([this, i](std::stop_token stop) { Worker(stop, i); });
        }
        if (options_.warm) {
            if (worker_count_ == 0) {
                std::unique_lock lock(mutex_);
                while (!startup_.empty()) {
                    Build(lock, *startup_.front(), false);
                }
            }
            WaitIdle();
        }
        if (!options_.folder.empty() && options_.save_seconds != 0) {
            saver_ = std::jthread([this](std::stop_token stop) { Saver(stop); });
        }
    }

    ~CachingProvider() override {
        saver_.request_stop();
        if (saver_.joinable()) {
            saver_.join();
        }
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        workers_.clear();
        Save();
        if (options_.spirv_cache && host_.set_spirv_store) {
            host_.set_spirv_store(nullptr);
        }
    }

    rhi::Result<PipelineHandle> Lookup(const PipelineKey& key) override {
        std::unique_lock lock(mutex_);
        ++counters_.lookups;
        const bool can_fallback = !key.IsFallback() && key.vertex.kind != native::VertexStageKind::QuaternionPass;
        const bool forced = options_.force_ubershader && can_fallback;
        if (forced) {
            ++counters_.forced_uber;
            ++frame_[Forced];
        }

        if (can_fallback && !forced && options_.background && !entries_.contains(key)) {
            Get(key.AsFallback(), false);
        }
        auto& entry = Get(forced ? key.AsFallback() : key, true);
        if (entry.state == BuildState::Ready) {
            ++counters_.hits;
            return PipelineHandle{entry.pipeline, forced, false};
        }
        if (!can_fallback || forced || !options_.background) {
            if (options_.background && options_.never_wait && IsUnbuilt(entry)) {
                Enqueue(entry);
                return Skipped();
            }
            EnsureBuilt(lock, entry);
            return Handle(entry, forced);
        }

        auto& fallback = Get(key.AsFallback(), false);
        if (fallback.state == BuildState::Ready) {
            Enqueue(entry);
            if (entry.state != BuildState::Failed && !AllowFallback()) {
                return Skipped();
            }
            ++counters_.hits;
            return Handle(fallback, true);
        }
        if (options_.never_wait && (entry.state != BuildState::Failed || IsUnbuilt(fallback))) {
            Enqueue(entry);
            Enqueue(fallback);
            return Skipped();
        }
        Enqueue(fallback);
        EnsureBuilt(lock, entry);
        if (entry.state == BuildState::Ready) {
            return Handle(entry, false);
        }
        EnsureBuilt(lock, fallback);
        return fallback.state == BuildState::Ready ? Handle(fallback, true) : Handle(entry, false);
    }

    PipelineReadiness Prepare(const PipelineKey& key, const PipelineKey& fallback_key) override {
        std::unique_lock lock(mutex_);
        const bool forced = options_.force_ubershader && key.vertex.kind != native::VertexStageKind::QuaternionPass;
        if (!forced && options_.background && key != fallback_key &&
            key.vertex.kind != native::VertexStageKind::QuaternionPass && !entries_.contains(key)) {
            Get(fallback_key, false);
        }
        auto& entry = Get(forced ? key.AsFallback() : key, false);
        if (entry.state == BuildState::Ready) {
            return PipelineReadiness::Ready;
        }
        if (entry.state == BuildState::Failed) {
            return PipelineReadiness::Failed;
        }
        if (forced || !options_.background || key == fallback_key || key.vertex.kind == native::VertexStageKind::QuaternionPass) {
            if (options_.background && options_.never_wait) {
                Enqueue(entry);
                return PipelineReadiness::Pending;
            }
            EnsureBuilt(lock, entry);
            return entry.state == BuildState::Ready ? PipelineReadiness::Ready : PipelineReadiness::Failed;
        }

        auto& fallback = Get(fallback_key, false);
        if (fallback.state == BuildState::Ready) {
            Enqueue(entry);
            ++frame_[Pending];
            return AllowFallback() ? PipelineReadiness::Fallback : PipelineReadiness::Pending;
        }
        Enqueue(fallback);
        if (options_.never_wait) {
            Enqueue(entry);
            ++frame_[Pending];
            return PipelineReadiness::Pending;
        }
        EnsureBuilt(lock, entry);
        return entry.state == BuildState::Ready ? PipelineReadiness::Ready : PipelineReadiness::Failed;
    }

    void ReportMiss(const PipelineKey& key, std::string_view reason) override {
        std::lock_guard lock(mutex_);
        ++counters_.reported_misses;
        Get(key, true);
        MH4N_LOG(Debug, kLog, "pipeline {:016x} left out: {}", key.Hash(), reason);
    }

    void Hurry(const PipelineKey& key) override {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.state != BuildState::Queued) {
            return;
        }
        auto& entry = found->second;
        auto& from = entry.queue == Queue::Urgent ? urgent_ : startup_;
        urgent_.splice(urgent_.begin(), from, entry.position);
        entry.queue = Queue::Urgent;
        work_.notify_all();
    }

    native::ProviderCounters Counters() const override {
        std::lock_guard lock(mutex_);
        return counters_;
    }

    std::vector<native::FrameCounter> TakeFrameCounters() override {
        static constexpr std::array names{
            "pipeline_sync_builds", "pipeline_sync_waits", "pipeline_sync_us", "pipeline_async_builds",
            "pipeline_pending", "pipeline_forced_uber", "pipeline_skipped",
        };
        std::lock_guard lock(mutex_);
        std::vector<native::FrameCounter> result;
        for (size_t i = 0; i < names.size(); ++i) {
            result.push_back({names[i], frame_[i]});
        }
        frame_.fill(0);
        return result;
    }

    CachingStats Stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    void WaitIdle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return urgent_.empty() && startup_.empty() && active_ == 0; });
    }

    void Save();

private:
    enum Frame { SyncBuild, SyncWait, SyncTime, AsyncBuild, Pending, Forced, SkippedDraw, FrameCount };

    Entry& Get(const PipelineKey& key, bool count_use) {
        auto [it, inserted] = entries_.try_emplace(key, key);
        auto& entry = it->second;
        if (inserted) {
            entry.order = next_order_++;
        }
        if (count_use && entry.uses != UINT32_MAX) {
            ++entry.uses;
        }
        if (inserted || count_use) {
            ++list_revision_;
        }
        return entry;
    }

    static bool IsUnbuilt(const Entry& entry) {
        return entry.state != BuildState::Ready && entry.state != BuildState::Failed;
    }

    static rhi::Result<PipelineHandle> Handle(const Entry& entry, bool fallback) {
        if (entry.state == BuildState::Ready) {
            return PipelineHandle{entry.pipeline, fallback, false};
        }
        return rhi::Error{entry.error};
    }

    rhi::Result<PipelineHandle> Skipped() {
        ++frame_[SkippedDraw];
        return PipelineHandle{nullptr, false, true};
    }

    bool AllowFallback() {
        if (!options_.never_wait) {
            return true;
        }
        if (!options_.use_fallback) {
            return false;
        }
        if (options_.fallback_per_window == 0) {
            return true;
        }
        const auto now = Clock::now();
        if (now - fallback_window_ >= std::chrono::milliseconds(8)) {
            fallback_window_ = now;
            fallback_used_ = 0;
        }
        if (fallback_used_ >= options_.fallback_per_window) {
            return false;
        }
        ++fallback_used_;
        return true;
    }

    void Enqueue(Entry& entry, Queue queue = Queue::Urgent) {
        if (entry.state == BuildState::Queued && entry.queue == Queue::Startup && queue == Queue::Urgent) {
            urgent_.splice(urgent_.end(), startup_, entry.position);
            entry.queue = Queue::Urgent;
            work_.notify_all();
            return;
        }
        if (entry.state != BuildState::New) {
            return;
        }
        auto& list = queue == Queue::Urgent ? urgent_ : startup_;
        entry.queue = queue;
        entry.position = list.insert(list.end(), &entry);
        entry.state = BuildState::Queued;
        work_.notify_all();
    }

    void EnsureBuilt(std::unique_lock<std::mutex>& lock, Entry& entry) {
        if (entry.state == BuildState::Building) {
            const auto start = Clock::now();
            ++stats_.sync_waits;
            ++frame_[SyncWait];
            idle_.wait(lock, [&] { return entry.state != BuildState::Building; });
            AddSyncTime(Clock::now() - start);
        } else if (IsUnbuilt(entry)) {
            Build(lock, entry, false);
        }
    }

    void AddSyncTime(Clock::duration duration) {
        stats_.sync_ms += Milliseconds(duration);
        frame_[SyncTime] += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
    }

    void Build(std::unique_lock<std::mutex>& lock, Entry& entry, bool async);
    void Worker(std::stop_token stop, u32 index);
    void Saver(std::stop_token stop);
    void Load();

    const native::ProviderHost host_;
    const CachingOptions options_;
    const u32 worker_count_;
    mutable std::mutex mutex_;
    std::condition_variable_any work_;
    std::condition_variable idle_;
    std::unordered_map<PipelineKey, Entry, native::PipelineKeyHash> entries_;
    std::list<Entry*> urgent_, startup_;
    size_t active_ = 0;
    size_t startup_fallbacks_ = 0;
    u32 next_order_ = 0;
    native::ProviderCounters counters_;
    CachingStats stats_;
    std::array<u64, FrameCount> frame_{};
    Clock::time_point fallback_window_;
    u32 fallback_used_ = 0;
    u64 period_builds_ = 0;
    double period_ms_ = 0;
    double longest_ms_ = 0;

    std::mutex save_mutex_;
    SpirvCache spirv_;
    std::filesystem::path driver_file_;
    u64 list_revision_ = 0, list_saved_ = 0;
    u64 driver_revision_ = 0, driver_saved_ = 0;
    std::vector<std::jthread> workers_;
    std::jthread saver_;
};

void CachingProvider::Build(std::unique_lock<std::mutex>& lock, Entry& entry, bool async) {
    if (entry.state == BuildState::Queued) {
        (entry.queue == Queue::Urgent ? urgent_ : startup_).erase(entry.position);
    }
    entry.state = BuildState::Building;
    ++active_;
    lock.unlock();
    const auto start = Clock::now();
    auto built = [&]() -> rhi::Result<std::shared_ptr<rhi::Pipeline>> {
        try {
            return host_.build(entry.key);
        } catch (const std::exception& error) {
            return rhi::Error{error.what()};
        }
    }();
    const auto duration = Clock::now() - start;
    if (Milliseconds(duration) > 500.0) {  // (a draw waits for its pipeline: this is the black screen of a few seconds)
        MH4N_LOG(Info, kLog, "slow pipeline: {:.0f} ms ({}, {}, {})", Milliseconds(duration), async ? "async" : "ON THE GAME'S THREAD",
                 entry.key.IsFallback() ? "fallback" : "full", built && *built ? "built" : "failed");
    }
    lock.lock();

    if (built && *built) {
        entry.pipeline = std::move(*built);
        entry.state = BuildState::Ready;
        ++driver_revision_;
    } else {
        entry.error = built ? "builder returned an empty pipeline" : built.error();
        entry.state = BuildState::Failed;
        ++stats_.failures;
        ++counters_.build_failures;
    }
    if (async) {
        ++stats_.async_builds;
        ++frame_[AsyncBuild];
        const double elapsed = Milliseconds(duration);
        stats_.async_ms += elapsed;
        ++period_builds_;
        period_ms_ += elapsed;
        longest_ms_ = std::max(longest_ms_, elapsed);
    } else {
        ++stats_.sync_builds;
        ++counters_.sync_builds;
        ++frame_[SyncBuild];
        AddSyncTime(duration);
    }
    if (entry.listed) {
        if (entry.key.IsFallback()) {
            --startup_fallbacks_;
        }
        if (++stats_.precompiled == stats_.listed) {
            MH4N_LOG(Info, kLog, "the list is compiled ({} pipelines)", stats_.listed);
            period_builds_ = 0;
            period_ms_ = 0;
            longest_ms_ = 0;
        }
    }
    --active_;
    work_.notify_all();
    idle_.notify_all();
}

void CachingProvider::Worker(std::stop_token stop, u32 index) {
    const bool can_precompile = options_.warm || worker_count_ == 1 || index != 0;
    std::unique_lock lock(mutex_);
    const auto startup_ready = [&] {
        return can_precompile && !startup_.empty() &&
               (startup_fallbacks_ == 0 || startup_.front()->key.IsFallback());
    };
    while (work_.wait(lock, stop, [&] { return !urgent_.empty() || startup_ready(); }) && !stop.stop_requested()) {
        Entry& entry = *(urgent_.empty() ? startup_.front() : urgent_.front());
        Build(lock, entry, true);
    }
}

void CachingProvider::Saver(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        work_.wait_for(lock, stop, std::chrono::seconds(options_.save_seconds), [] { return false; });
        if (stop.stop_requested()) {
            break;
        }
        const bool compiling = stats_.precompiled < stats_.listed;
        if (compiling || period_ms_ >= 1000.0) {
            const double average = period_builds_ == 0 ? 0 : period_ms_ / static_cast<double>(period_builds_);
            if (compiling) {
                MH4N_LOG(Info, kLog, "{} builds, {:.1f} ms each (the longest {:.1f} ms), the list not compiled yet ({}/{})",
                         period_builds_, average, longest_ms_, stats_.precompiled, stats_.listed);
            } else {
                MH4N_LOG(Info, kLog, "{} builds, {:.1f} ms each (the longest {:.1f} ms)", period_builds_, average, longest_ms_);
            }
        }
        period_builds_ = 0;
        period_ms_ = 0;
        longest_ms_ = 0;
        lock.unlock();
        Save();
        lock.lock();
    }
}

void CachingProvider::Load() {
    if (options_.folder.empty()) {
        return;
    }
    if (options_.driver_cache && host_.backend) {
        const auto name = DriverFile(*host_.backend);
        if (!name.empty()) {
            driver_file_ = options_.folder / name;
            if (const auto bytes = ReadBytes(driver_file_)) {
                stats_.driver_blob_loaded = host_.backend->LoadPipelineCache(*bytes);
            }
        }
    }

    Records records;
    if (options_.spirv_cache &&
        ReadRecords(options_.folder / "spirv.bin", kKindSpirv, kFileVersion, host_.spirv_compiler_version, records) ==
            ReadStatus::Ok) {
        spirv_.Load(records);
    }
    if (!options_.list) {
        return;
    }
    std::unordered_set<u64> sources;
    if (host_.load_vertex_source &&
        ReadRecords(options_.folder / "vertex.bin", kKindVertex, kFileVersion, host_.vertex_source_version, records) ==
            ReadStatus::Ok) {
        for (const auto& record : records) {
            if (const auto id = host_.load_vertex_source(record)) {
                sources.insert(*id);
            }
        }
    }

    if (ReadRecords(options_.folder / "keys.bin", kKindKeys, kFileVersion, native::kPipelineKeyVersion, records) != ReadStatus::Ok) {
        return;
    }
    std::vector<Entry*> listed;
    for (const auto& record : records) {
        disk::Reader in(record);
        const u32 uses = in.Read<u32>();
        const u32 order = in.Read<u32>();
        const auto key = DecodeKey(in.Take(in.Remaining()));
        if (!key || (key->vertex.kind != native::VertexStageKind::Trivial && !sources.contains(key->vertex.program))) {
            continue;
        }
        auto [it, inserted] = entries_.try_emplace(*key, *key);
        if (inserted) {
            auto& entry = it->second;
            entry.uses = uses;
            entry.order = order;
            next_order_ = std::max(next_order_, order + 1);
            if (options_.background || options_.warm) {
                entry.listed = true;
                listed.push_back(&entry);
            }
        }
    }
    stats_.listed = listed.size();
    std::ranges::stable_sort(listed, [](const Entry* a, const Entry* b) {
        if (a->key.IsFallback() != b->key.IsFallback()) {
            return a->key.IsFallback();
        }
        if (a->uses != b->uses && !a->key.IsFallback()) {
            return a->uses > b->uses;
        }
        return a->order < b->order;
    });
    for (auto* entry : listed) {
        startup_fallbacks_ += entry->key.IsFallback();
        Enqueue(*entry, Queue::Startup);
    }
}

void CachingProvider::Save() {
    if (options_.folder.empty()) {
        return;
    }
    std::lock_guard save_lock(save_mutex_);
    std::string error;
    bool wrote = false;
    if (options_.list) {
        u64 revision;
        std::vector<std::tuple<u32, u32, PipelineKey>> entries;
        {
            std::lock_guard lock(mutex_);
            revision = list_revision_;
            if (revision != list_saved_) {
                entries.reserve(entries_.size());
                for (const auto& [key, entry] : entries_) {
                    entries.emplace_back(entry.order, entry.uses, key);
                }
            }
        }
        if (!entries.empty()) {
            std::ranges::sort(entries, {}, [](const auto& row) { return std::get<0>(row); });
            Records keys, vertices;
            keys.reserve(entries.size());
            std::unordered_set<u64> sources;
            for (const auto& [order, uses, key] : entries) {
                if (key.vertex.kind != native::VertexStageKind::Trivial && host_.save_vertex_source &&
                    sources.insert(key.vertex.program).second) {
                    auto source = host_.save_vertex_source(key.vertex.program);
                    if (!source.empty()) {
                        vertices.push_back(std::move(source));
                    }
                }
                auto& record = keys.emplace_back();
                disk::Append(record, uses);
                disk::Append(record, order);
                EncodeKey(key, record);
            }
            if (WriteRecords(options_.folder / "vertex.bin", kKindVertex, kFileVersion, host_.vertex_source_version, vertices, &error) &&
                WriteRecords(options_.folder / "keys.bin", kKindKeys, kFileVersion, native::kPipelineKeyVersion, keys, &error)) {
                list_saved_ = revision;
                wrote = true;
            } else {
                MH4N_LOG(Warning, kLog, "could not save the pipeline list: {}", error);
            }
        }
    }
    error.clear();
    if (options_.spirv_cache) {
        wrote |= spirv_.Save(options_.folder / "spirv.bin", host_.spirv_compiler_version, error);
        if (!error.empty()) {
            MH4N_LOG(Warning, kLog, "could not save the SPIR-V cache: {}", error);
        }
    }
    if (!driver_file_.empty()) {
        u64 revision;
        {
            std::lock_guard lock(mutex_);
            revision = driver_revision_;
        }
        if (revision != driver_saved_) {
            const auto blob = host_.backend->SavePipelineCache();
            if (!blob.empty() && WriteBytes(driver_file_, blob, &error)) {
                driver_saved_ = revision;
                wrote = true;
            } else if (!error.empty()) {
                MH4N_LOG(Warning, kLog, "could not save the driver cache: {}", error);
            }
        }
    }
    if (wrote) {
        std::lock_guard lock(mutex_);
        ++stats_.saves;
    }
}

CachingOptions CachingOptionsFrom(const Options& options, bool force_ubershader) {
    CachingOptions cache;
    if (!options.Has("--no-pipeline-files")) {
        if (auto dir = options.Get("--pipeline-dir")) {
            cache.folder = std::filesystem::path(std::u8string(dir->begin(), dir->end()));
        } else if (auto user = options.Get("--user"); user && !user->empty()) {
            cache.folder = std::filesystem::path(std::u8string(user->begin(), user->end())) / "cache" / "pipelines";
        }
    }
    cache.list = !options.Has("--no-pipeline-list");
    cache.driver_cache = !options.Has("--no-driver-cache");
    cache.spirv_cache = !options.Has("--no-spirv-cache");
    cache.background = !options.Has("--no-ubershader");
    const char* wait = std::getenv("MH4N_PIPELINE_WAIT");
    cache.never_wait = !options.Has("--pipeline-wait") && !options.Has("--deterministic") &&
                       !(wait && std::string_view(wait) == "1");
    cache.use_fallback = !cache.never_wait;
    if (cache.never_wait) {
        if (const char* value = std::getenv("MH4N_PIPELINE_FALLBACK")) {
            char* end = nullptr;
            const long long limit = std::strtoll(value, &end, 10);
            cache.use_fallback = end != value && limit != 0;
            if (limit > 1) {
                cache.fallback_per_window = static_cast<u32>(std::min(limit, 100000ll));
            }
        }
    }
    cache.warm = options.Has("--warm");
    cache.force_ubershader = force_ubershader;
    try {
        cache.workers = static_cast<u32>(std::min<u64>(options.GetUInt("--pipeline-workers"), 16));
    } catch (const OptionError& error) {
        MH4N_LOG(Error, kLog, "--pipeline-workers: {}", error.what());
    }
    return cache;
}

std::unique_ptr<native::PipelineProvider> MakeCachingProvider(const native::ProviderHost& host, const CachingOptions& options) {
    auto provider = std::make_unique<CachingProvider>(host, options);
    provider->Start();
    return provider;
}

std::optional<CachingStats> StatsOf(const native::PipelineProvider& provider) {
    if (const auto* cache = dynamic_cast<const CachingProvider*>(&provider)) {
        return cache->Stats();
    }
    return std::nullopt;
}

void WaitIdle(native::PipelineProvider& provider) {
    if (auto* cache = dynamic_cast<CachingProvider*>(&provider)) {
        cache->WaitIdle();
    }
}

void SaveNow(native::PipelineProvider& provider) {
    if (auto* cache = dynamic_cast<CachingProvider*>(&provider)) {
        cache->Save();
    }
}

namespace {

std::unique_ptr<native::PipelineProvider> MakeFromHost(const native::ProviderHost& host) {
    const auto options = CachingOptionsFrom(host.options ? *host.options : Options{}, host.force_ubershader);
    return MakeCachingProvider(host, options);
}

MH4N_REGISTER(native::PipelineProviderFactory, native::kCachingProviderName, MakeFromHost);

#define MH4N_CACHE_OPTION(name, arg, help) \
    MH4N_OPTION("runner", name, arg, help); \
    MH4N_OPTION("capture-player", name, arg, help)

MH4N_CACHE_OPTION("--no-driver-cache", "", "disable the driver pipeline cache");
MH4N_CACHE_OPTION("--no-pipeline-files", "", "disable pipeline cache files");
MH4N_CACHE_OPTION("--no-pipeline-list", "", "disable the pipeline list");
MH4N_CACHE_OPTION("--no-spirv-cache", "", "disable shader binary caching");
MH4N_CACHE_OPTION("--no-ubershader", "", "compile missing pipelines on the renderer thread");
MH4N_CACHE_OPTION("--pipeline-dir", "DIR", "pipeline cache directory");
MH4N_CACHE_OPTION("--pipeline-wait", "", "wait for pipeline builds during rendering");
MH4N_CACHE_OPTION("--pipeline-workers", "N", "pipeline compilation worker count");
MH4N_CACHE_OPTION("--warm", "", "compile the pipeline list before the first draw");

#undef MH4N_CACHE_OPTION

}  // namespace
}  // namespace mh4n::gpu::pipeline




