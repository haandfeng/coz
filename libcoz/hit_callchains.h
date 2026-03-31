#ifndef COZ_LIBCOZ_HIT_CALLCHAINS_H
#define COZ_LIBCOZ_HIT_CALLCHAINS_H

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "perf.h"

namespace hit_callchains {

constexpr uint32_t kBucketCount = 128;
constexpr uint16_t kMaxDepth = 32;

struct Bucket {
  uint8_t occupied = 0;
  uint8_t reserved8 = 0;
  uint16_t depth = 0;
  uint32_t count = 0;
  uint64_t hash = 0;
  uint64_t leaf_ip = 0;
  uint64_t pcs[kMaxDepth] = {};
};

struct Table {
  std::atomic<uint32_t> writers{0};
  std::atomic<uint64_t> dropped{0};
  uint64_t experiment_id = 0;
  Bucket buckets[kBucketCount];

  void reset(uint64_t new_experiment_id);
};

void record_hit(Table& table,
                uint64_t experiment_id,
                perf_event::record& sample,
                uint32_t count);

void collect_table(Table& table,
                   uint64_t experiment_id,
                   std::unordered_map<std::string, size_t>& out);

uint64_t collect_dropped(Table& table, uint64_t experiment_id);

}  // namespace hit_callchains

#endif
