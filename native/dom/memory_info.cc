#include "mitto/dom/memory_info.h"

#include <math.h>

#include <array>
#include <limits>
#include <optional>

#include "base/check_op.h"
#include "base/time/time.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_memory_info.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-statistics.h"

namespace mitto {
namespace {

constexpr base::TimeDelta kTwentyMinutes = base::Minutes(20);
constexpr base::TimeDelta kFiftyMs = base::Milliseconds(50);

bool g_locked_to_site = false;

void GetHeapSize(HeapInfo& info) {
  v8::HeapStatistics heap_statistics;
  v8::Isolate::GetCurrent()->GetHeapStatistics(&heap_statistics);
  info.used_js_heap_size =
      heap_statistics.used_heap_size() + heap_statistics.external_memory();
  info.total_js_heap_size =
      heap_statistics.total_physical_size() + heap_statistics.external_memory();
  info.js_heap_size_limit = heap_statistics.heap_size_limit();
}

// One cache for both precisions, as Blink's thread has one: what one reading
// stored, the next of either precision gives until its own time is up.
class HeapSizeCache {
 public:
  HeapSizeCache() = default;
  HeapSizeCache(const HeapSizeCache&) = delete;
  HeapSizeCache& operator=(const HeapSizeCache&) = delete;

  static HeapSizeCache& Get() {
    static HeapSizeCache cache;
    return cache;
  }

  void GetCachedHeapSize(HeapInfo& info, MemoryInfo::Precision precision) {
    MaybeUpdate(precision);
    info = info_;
  }

 private:
  void MaybeUpdate(MemoryInfo::Precision precision) {
    const base::TimeTicks now = base::TimeTicks::Now();
    const base::TimeDelta delta_allowed =
        precision == MemoryInfo::Precision::kBucketized ? kTwentyMinutes
                                                        : kFiftyMs;
    if (!last_update_time_.has_value() ||
        now - last_update_time_.value() >= delta_allowed) {
      Update(precision);
      last_update_time_ = now;
    }
  }

  void Update(MemoryInfo::Precision precision) {
    GetHeapSize(info_);
    if (precision == MemoryInfo::Precision::kPrecise) {
      return;
    }
    info_.used_js_heap_size = QuantizeMemorySize(info_.used_js_heap_size);
    info_.total_js_heap_size = QuantizeMemorySize(info_.total_js_heap_size);
    info_.js_heap_size_limit = QuantizeMemorySize(info_.js_heap_size_limit);
  }

  std::optional<base::TimeTicks> last_update_time_;
  HeapInfo info_;
};

constexpr int kNumberOfBuckets = 100;

// Blink's buckets, worked out in single precision as Blink works them out.
std::array<size_t, kNumberOfBuckets> MakeBuckets() {
  std::array<size_t, kNumberOfBuckets> bucket_size_list = {};
  float size_of_next_bucket = 10000000.0;  // First bucket size is roughly 10M.
  const float kLargestBucketSize = 4000000000.0;  // Roughly 4GB.
  // We scale with the Nth root of the ratio, so that we use all the buckets.
  const float scaling_factor =
      exp(log(kLargestBucketSize / size_of_next_bucket) / kNumberOfBuckets);

  size_t next_power_of_ten =
      static_cast<size_t>(pow(10, floor(log10(size_of_next_bucket)) + 1) + 0.5);
  size_t granularity = next_power_of_ten / 1000;  // 3 significant digits.

  for (int i = 0; i < kNumberOfBuckets; ++i) {
    const size_t current_bucket_size = static_cast<size_t>(size_of_next_bucket);
    bucket_size_list[i] =
        current_bucket_size - (current_bucket_size % granularity);

    size_of_next_bucket *= scaling_factor;
    if (size_of_next_bucket >= next_power_of_ten) {
      if (std::numeric_limits<size_t>::max() / 10 <= next_power_of_ten) {
        next_power_of_ten = std::numeric_limits<size_t>::max();
      } else {
        next_power_of_ten *= 10;
        granularity *= 10;
      }
    }

    // Watch out for overflow, if the range is too large for size_t.
    if (i > 0 && bucket_size_list[i] < bucket_size_list[i - 1]) {
      bucket_size_list[i] = std::numeric_limits<size_t>::max();
    }
  }
  return bucket_size_list;
}

}  // namespace

size_t QuantizeMemorySize(size_t size) {
  static const std::array<size_t, kNumberOfBuckets> bucket_size_list =
      MakeBuckets();
  for (const size_t bucket_size : bucket_size_list) {
    if (size <= bucket_size) {
      return bucket_size;
    }
  }
  return bucket_size_list.back();
}

// static
MemoryInfo* MemoryInfo::Create(Precision precision) {
  return bindings::MakeGarbageCollected<MemoryInfo>(precision);
}

MemoryInfo::MemoryInfo(Precision precision) {
  // With PreciseMemoryInfo on, the sizes are neither bucketized nor cached.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (bindings::PerIsolateData::From(isolate)->features().IsEnabled(
          bindings::RuntimeFeature::kPreciseMemoryInfo)) {
    GetHeapSize(info_);
  } else {
    HeapSizeCache::Get().GetCachedHeapSize(info_, precision);
  }
  DCHECK_GT(TotalJSHeapSize(), 0u);
}

MemoryInfo::~MemoryInfo() = default;

const bindings::WrapperTypeInfo* MemoryInfo::GetWrapperTypeInfo() const {
  return &bindings::V8MemoryInfo::kWrapperTypeInfo;
}

// static
void MemoryInfo::SetProcessLockedToSite(bool locked) {
  g_locked_to_site = locked;
}

// static
bool MemoryInfo::IsProcessLockedToSite() {
  return g_locked_to_site;
}

}  // namespace mitto
