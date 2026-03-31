#include "hit_callchains.h"

#include <algorithm>
#include <cinttypes>
#include <string>
#include <vector>

#include "inspect.h"

using namespace std;

namespace hit_callchains {

namespace {

static string json_escape(const string& s) {
  string result;
  result.reserve(s.size() + 8);
  for(char c : s) {
    switch(c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:   result += c; break;
    }
  }
  return result;
}

static string line_to_string(const line* l) {
  auto f = l->get_file();
  return json_escape(f->get_name() + ":" + to_string(l->get_line()));
}

static uint64_t hash_raw_callchain(perf_event::record& sample) {
  uint64_t h = 1469598103934665603ULL;

  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };

  mix(sample.get_ip());

  auto chain = sample.get_callchain();
  size_t depth = min<size_t>(chain.size(), kMaxDepth);
  mix(depth);
  for(size_t i = 0; i < depth; i++) {
    mix(chain[i]);
  }

  if(h == 0) {
    h = 1;
  }
  return h;
}

static bool raw_equals(const Bucket& bucket, perf_event::record& sample) {
  if(!bucket.occupied) {
    return false;
  }
  if(bucket.leaf_ip != sample.get_ip()) {
    return false;
  }

  auto chain = sample.get_callchain();
  size_t depth = min<size_t>(chain.size(), kMaxDepth);
  if(bucket.depth != depth) {
    return false;
  }

  for(size_t i = 0; i < depth; i++) {
    if(bucket.pcs[i] != chain[i]) {
      return false;
    }
  }
  return true;
}

static string addr_fallback(uint64_t addr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "[unknown@0x%" PRIx64 "]", addr);
  return string(buf);
}

static string build_callchain_string_from_bucket(const Bucket& bucket) {
  string result;

  // Leaf: use hex fallback instead of discarding the entire chain
  string leaf_str;
  {
    auto leaf = memory_map::get_instance().find_line(bucket.leaf_ip);
    if(leaf) {
      leaf_str = line_to_string(leaf.get());
    } else {
      leaf_str = addr_fallback(bucket.leaf_ip);
    }
  }

  vector<string> parts;
  for(uint16_t i = 0; i < bucket.depth; i++) {
    auto l = memory_map::get_instance().find_line(bucket.pcs[i] - 1);
    if(l) {
      parts.push_back(line_to_string(l.get()));
    } else {
      parts.push_back(addr_fallback(bucket.pcs[i]));
    }
  }
  parts.push_back(leaf_str);

  for(size_t i = 0; i < parts.size(); i++) {
    if(i > 0) {
      result += "|";
    }
    result += parts[i];
  }
  return result;
}

static void wait_for_writers(Table& table) {
  while(table.writers.load(memory_order_acquire) != 0) {
  }
}

}  // namespace

void Table::reset(uint64_t new_experiment_id) {
  experiment_id = new_experiment_id;
  for(uint32_t i = 0; i < kBucketCount; i++) {
    buckets[i].occupied = 0;
    buckets[i].reserved8 = 0;
    buckets[i].depth = 0;
    buckets[i].count = 0;
    buckets[i].hash = 0;
    buckets[i].leaf_ip = 0;
  }
  dropped.store(0, memory_order_relaxed);
}

void record_hit(Table& table,
                uint64_t experiment_id,
                perf_event::record& sample,
                uint32_t count) {
  auto chain = sample.get_callchain();
  if(chain.size() == 0) {
    return;
  }

  if(table.experiment_id != experiment_id) {
    return;
  }

  table.writers.fetch_add(1, memory_order_acquire);

  uint64_t hash = hash_raw_callchain(sample);
  uint32_t start = static_cast<uint32_t>(hash % kBucketCount);
  uint16_t depth = static_cast<uint16_t>(min<size_t>(chain.size(), kMaxDepth));

  for(uint32_t probe = 0; probe < kBucketCount; probe++) {
    uint32_t idx = (start + probe) % kBucketCount;
    Bucket& bucket = table.buckets[idx];

    if(bucket.occupied) {
      if(bucket.hash == hash && raw_equals(bucket, sample)) {
        bucket.count += count;
        table.writers.fetch_sub(1, memory_order_release);
        return;
      }
      continue;
    }

    bucket.occupied = 1;
    bucket.hash = hash;
    bucket.leaf_ip = sample.get_ip();
    bucket.depth = depth;
    bucket.count = count;
    for(uint16_t i = 0; i < depth; i++) {
      bucket.pcs[i] = chain[i];
    }

    table.writers.fetch_sub(1, memory_order_release);
    return;
  }

  table.dropped.fetch_add(1, memory_order_relaxed);
  table.writers.fetch_sub(1, memory_order_release);
}

void collect_table(Table& table,
                   uint64_t experiment_id,
                   unordered_map<string, size_t>& out) {
  wait_for_writers(table);

  if(table.experiment_id != experiment_id) {
    return;
  }

  for(uint32_t i = 0; i < kBucketCount; i++) {
    const Bucket& bucket = table.buckets[i];
    if(!bucket.occupied) {
      continue;
    }

    string chain = build_callchain_string_from_bucket(bucket);
    if(!chain.empty()) {
      out[chain] += bucket.count;
    }
  }
}

uint64_t collect_dropped(Table& table, uint64_t experiment_id) {
  wait_for_writers(table);

  if(table.experiment_id != experiment_id) {
    return 0;
  }

  return table.dropped.load(memory_order_relaxed);
}

}  // namespace hit_callchains
