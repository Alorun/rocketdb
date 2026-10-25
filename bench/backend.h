#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#ifdef ROCKETDB_BENCH_USE_LEVELDB
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
namespace benchdb = leveldb;
#define BENCH_ENGINE_NAME "LevelDB"
#define BENCH_ENGINE_SLUG "leveldb"
#define BENCH_PROPERTY_PREFIX "leveldb."
#else
#include "cache.h"
#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "options.h"
#include "status.h"
namespace benchdb = rocketdb;
#define BENCH_ENGINE_NAME "RocketDB"
#define BENCH_ENGINE_SLUG "rocketdb"
#define BENCH_PROPERTY_PREFIX "rocketdb."
#endif

namespace bench_backend {

struct CacheStats {
  uint64_t hit;
  uint64_t miss;
};

#ifdef ROCKETDB_BENCH_USE_LEVELDB
// Match RocketDB's statistics decorator, including its relaxed atomic lookup
// accounting. TableCache still uses each engine's ordinary, separate cache.
class StatisticsCache : public benchdb::Cache {
 public:
  explicit StatisticsCache(size_t capacity) : target_(benchdb::NewLRUCache(capacity)) {}

  Handle* Insert(const benchdb::Slice& key, void* value, size_t charge,
                 void (*deleter)(const benchdb::Slice&, void*)) override {
    return target_->Insert(key, value, charge, deleter);
  }
  Handle* Lookup(const benchdb::Slice& key) override {
    Handle* handle = target_->Lookup(key);
    (handle == nullptr ? miss_ : hit_).fetch_add(1, std::memory_order_relaxed);
    return handle;
  }
  void Release(Handle* handle) override { target_->Release(handle); }
  void* Value(Handle* handle) override { return target_->Value(handle); }
  void Erase(const benchdb::Slice& key) override { target_->Erase(key); }
  uint64_t NewId() override { return target_->NewId(); }
  void Prune() override { target_->Prune(); }
  size_t TotalCharge() const override { return target_->TotalCharge(); }
  CacheStats GetStats() const {
    return {hit_.load(std::memory_order_relaxed), miss_.load(std::memory_order_relaxed)};
  }

 private:
  std::unique_ptr<benchdb::Cache> target_;
  std::atomic<uint64_t> hit_{0};
  std::atomic<uint64_t> miss_{0};
};

inline benchdb::Cache* NewStatisticsCache(size_t capacity) {
  return new StatisticsCache(capacity);
}

inline CacheStats ReadCacheStats(benchdb::Cache* cache) {
  return static_cast<StatisticsCache*>(cache)->GetStats();
}
#else
inline benchdb::Cache* NewStatisticsCache(size_t capacity) {
  return benchdb::NewLRUCacheWithStatistics(capacity);
}

inline CacheStats ReadCacheStats(benchdb::Cache* cache) {
  benchdb::CacheStats stats{};
  cache->GetStats(&stats);
  return {stats.hit, stats.miss};
}
#endif

}  // namespace bench_backend
