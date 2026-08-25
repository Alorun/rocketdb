#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "dbformat.h"

namespace rocketdb {

inline void AtomicMax(std::atomic<uint64_t>* target, uint64_t value) {
    uint64_t current = target->load(std::memory_order_relaxed);
    while (current < value &&
           !target->compare_exchange_weak(current, value, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

struct TimedEventSnapshot {
    uint64_t count;
    uint64_t total_micros;
    uint64_t max_micros;
};

class TimedEventCounter {
    public:
        void Record(uint64_t micros) {
            count_.fetch_add(1, std::memory_order_relaxed);
            total_micros_.fetch_add(micros, std::memory_order_relaxed);
            AtomicMax(&max_micros_, micros);
        }

        TimedEventSnapshot Snapshot() const {
            TimedEventSnapshot result;
            result.count = count_.load(std::memory_order_relaxed);
            result.total_micros = total_micros_.load(std::memory_order_relaxed);
            result.max_micros = max_micros_.load(std::memory_order_relaxed);
            return result;
        }

    private:
        std::atomic<uint64_t> count_{0};
        std::atomic<uint64_t> total_micros_{0};
        std::atomic<uint64_t> max_micros_{0};
};

struct FlushStatsSnapshot {
    uint64_t count;
    uint64_t total_micros;
    uint64_t max_micros;
    uint64_t bytes_written;
    std::array<uint64_t, config::kNumLevels> count_by_output_level;
};

struct MajorCompactionStatsSnapshot {
    uint64_t count;
    uint64_t total_micros;
    uint64_t max_micros;
    uint64_t bytes_read;
    uint64_t bytes_written;
};

enum class WriteStallType {
    kImmutableMemtableWait,
    kL0Slowdown,
    kL0Stop,
};

class DBPerfCounters {
    public:
        DBPerfCounters() {
            for (int level = 0; level < config::kNumLevels; ++level) {
                flush_count_by_output_level_[level].store(0, std::memory_order_relaxed);
                trivial_move_count_[level].store(0, std::memory_order_relaxed);
            }
        }

        void RecordFlush(int output_level, uint64_t micros, uint64_t bytes_written) {
            flush_.Record(micros);
            flush_bytes_written_.fetch_add(bytes_written, std::memory_order_relaxed);
            flush_count_by_output_level_[output_level].fetch_add(1, std::memory_order_relaxed);
        }

        void RecordMajorCompaction(int output_level, uint64_t micros, uint64_t bytes_read,
                                   uint64_t bytes_written) {
            MajorCompactionCounters& counters = major_compaction_[output_level];
            counters.time.Record(micros);
            counters.bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
            counters.bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
        }

        void RecordTrivialMove(int output_level) {
            trivial_move_count_[output_level].fetch_add(1, std::memory_order_relaxed);
        }

        void RecordWriteStall(WriteStallType type, uint64_t micros) {
            WriteStallCounter(type)->Record(micros);
        }

        FlushStatsSnapshot FlushSnapshot() const {
            const TimedEventSnapshot time = flush_.Snapshot();
            FlushStatsSnapshot result;
            result.count = time.count;
            result.total_micros = time.total_micros;
            result.max_micros = time.max_micros;
            result.bytes_written = flush_bytes_written_.load(std::memory_order_relaxed);
            for (int level = 0; level < config::kNumLevels; ++level) {
                result.count_by_output_level[level] =
                    flush_count_by_output_level_[level].load(std::memory_order_relaxed);
            }
            return result;
        }

        MajorCompactionStatsSnapshot MajorCompactionSnapshot(int output_level) const {
            const MajorCompactionCounters& counters = major_compaction_[output_level];
            const TimedEventSnapshot time = counters.time.Snapshot();
            MajorCompactionStatsSnapshot result;
            result.count = time.count;
            result.total_micros = time.total_micros;
            result.max_micros = time.max_micros;
            result.bytes_read = counters.bytes_read.load(std::memory_order_relaxed);
            result.bytes_written = counters.bytes_written.load(std::memory_order_relaxed);
            return result;
        }

        uint64_t TrivialMoveCount(int output_level) const {
            return trivial_move_count_[output_level].load(std::memory_order_relaxed);
        }

        TimedEventSnapshot WriteStallSnapshot(WriteStallType type) const {
            return WriteStallCounter(type)->Snapshot();
        }

    private:
        struct MajorCompactionCounters {
            TimedEventCounter time;
            std::atomic<uint64_t> bytes_read{0};
            std::atomic<uint64_t> bytes_written{0};
        };

        TimedEventCounter* WriteStallCounter(WriteStallType type) {
            switch (type) {
                case WriteStallType::kImmutableMemtableWait:
                    return &immutable_memtable_wait_;
                case WriteStallType::kL0Slowdown:
                    return &l0_slowdown_;
                case WriteStallType::kL0Stop:
                    return &l0_stop_;
            }
            return &immutable_memtable_wait_;
        }

        const TimedEventCounter* WriteStallCounter(WriteStallType type) const {
            switch (type) {
                case WriteStallType::kImmutableMemtableWait:
                    return &immutable_memtable_wait_;
                case WriteStallType::kL0Slowdown:
                    return &l0_slowdown_;
                case WriteStallType::kL0Stop:
                    return &l0_stop_;
            }
            return &immutable_memtable_wait_;
        }

        TimedEventCounter flush_;
        std::atomic<uint64_t> flush_bytes_written_{0};
        std::array<std::atomic<uint64_t>, config::kNumLevels> flush_count_by_output_level_{};
        std::array<MajorCompactionCounters, config::kNumLevels> major_compaction_{};
        std::array<std::atomic<uint64_t>, config::kNumLevels> trivial_move_count_{};
        TimedEventCounter immutable_memtable_wait_;
        TimedEventCounter l0_slowdown_;
        TimedEventCounter l0_stop_;
};

}  // namespace rocketdb
