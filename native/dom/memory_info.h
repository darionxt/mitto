// Blink's MemoryInfo, which performance.memory and console.memory give: the
// sizes of V8's heap, read at most once in 50 milliseconds when precise and
// once in 20 minutes when bucketized, from one cache for the thread, so that a
// page cannot time the garbage collector or what an action costs. Bucketized
// sizes are rounded up to one of 100 buckets.
//
// Upstream: third_party/blink/renderer/core/timing/memory_info.h
// Upstream: third_party/blink/renderer/core/timing/memory_info.cc

#ifndef MITTO_DOM_MEMORY_INFO_H_
#define MITTO_DOM_MEMORY_INFO_H_

#include <stddef.h>
#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

// What V8's heap statistics say, as MemoryInfo gives them.
struct HeapInfo {
  size_t used_js_heap_size = 0;
  size_t total_js_heap_size = 0;
  size_t js_heap_size_limit = 0;
};

class MemoryInfo final : public bindings::ScriptWrappable {
 public:
  // kPrecise for performance.memory in a process locked to a site, and
  // kBucketized otherwise, and always for console.memory.
  enum class Precision { kPrecise, kBucketized };

  static MemoryInfo* Create(Precision precision);

  explicit MemoryInfo(Precision precision);
  ~MemoryInfo() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint64_t TotalJSHeapSize() const { return info_.total_js_heap_size; }
  uint64_t UsedJSHeapSize() const { return info_.used_js_heap_size; }
  uint64_t JsHeapSizeLimit() const { return info_.js_heap_size_limit; }

  // Whether the process is locked to a site, as Chrome's site isolation locks
  // a desktop renderer to the site of the document it loads: a Page is while
  // its document is at an HTTP or HTTPS URL. Then performance.memory is
  // precise. Upstream: content/renderer/renderer_blink_platform_impl.cc
  static void SetProcessLockedToSite(bool locked);
  static bool IsProcessLockedToSite();

 private:
  HeapInfo info_;
};

// Rounds `size` up to the nearest of Blink's 100 buckets, which grow by the
// same factor from about 10 MB to about 4 GB, with three significant digits.
size_t QuantizeMemorySize(size_t size);

}  // namespace mitto

#endif  // MITTO_DOM_MEMORY_INFO_H_
