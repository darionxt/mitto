// Resource Timing's PerformanceResourceTiming: how a request of the Page went,
// from what the Session's network stack said of it, with what the TAO check
// and CORS hide, as Blink's class gives it. A Page has no service worker, so
// the worker's times are 0 and its source types empty.
//
// Upstream: third_party/blink/renderer/core/timing/performance_resource_timing.h
// Upstream: third_party/blink/renderer/core/timing/performance_resource_timing.cc

#ifndef MITTO_DOM_PERFORMANCE_RESOURCE_TIMING_H_
#define MITTO_DOM_PERFORMANCE_RESOURCE_TIMING_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/time/time.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/performance_entry.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class PerformanceServerTiming;

class PerformanceResourceTiming : public PerformanceEntry {
 public:
  // The size that Blink counts for a response's headers.
  static constexpr uint64_t kHeaderSize = 300;

  PerformanceResourceTiming(ResourceTimingInfo info,
                            std::u16string initiator_type,
                            base::TimeTicks time_origin,
                            uint64_t navigation_id);
  ~PerformanceResourceTiming() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& InitiatorType() const { return initiator_type_; }
  std::u16string NextHopProtocol() const;
  virtual std::u16string DeliveryType() const;
  double WorkerStart() const { return 0; }
  double WorkerRouterEvaluationStart() const { return 0; }
  double WorkerCacheLookupStart() const { return 0; }
  std::u16string WorkerMatchedSourceType() const { return std::u16string(); }
  std::u16string WorkerFinalSourceType() const { return std::u16string(); }
  virtual double RedirectStart() const;
  virtual double RedirectEnd() const;
  virtual double FetchStart() const;
  double DomainLookupStart() const;
  double DomainLookupEnd() const;
  double ConnectStart() const;
  double ConnectEnd() const;
  double SecureConnectionStart() const;
  double RequestStart() const;
  double ResponseStart() const;
  virtual double ResponseEnd() const;
  uint64_t TransferSize() const;
  uint64_t EncodedBodySize() const { return Info().encoded_body_size; }
  uint64_t DecodedBodySize() const { return Info().decoded_body_size; }
  v8::Local<v8::Value> ServerTiming() const;
  std::u16string RenderBlockingStatus() const;
  uint16_t ResponseStatus() const { return Info().response_status; }
  const std::u16string& ContentType() const { return Info().content_type; }
  const std::u16string& ContentEncoding() const {
    return Info().content_encoding;
  }
  double FinalResponseHeadersStart() const;
  double FirstInterimResponseStart() const;

  // PerformanceEntry:
  Kind GetKind() const override;
  std::u16string_view TypeName() const override;

  // GetTransferSize(): what came over the network, headers counted as
  // kHeaderSize, none from the cache.
  static uint64_t GetTransferSize(uint64_t encoded_body_size,
                                  CacheState cache_state);

 protected:
  // What the entry was made from. The navigation's reads its window's, whose
  // sizes come once the document has all of its body.
  virtual const ResourceTimingInfo& Info() const;
  base::TimeTicks TimeOrigin() const { return time_origin_; }
  // Performance::MonotonicTimeToDOMHighResTimeStamp() from the time origin.
  double ToDOMHighResTimeStamp(base::TimeTicks time) const;
  std::u16string GetDeliveryType() const;
  // The server's metrics as a new array that is not frozen.
  v8::Local<v8::Value> ServerTimingSequence() const;

  void BuildJSONValue(V8ObjectBuilder& builder) const override;

 private:
  const ResourceTimingInfo info_;
  const std::u16string initiator_type_;
  const base::TimeTicks time_origin_;
  // Made once, as Blink makes them with the entry.
  HeapVector<PerformanceServerTiming> server_timing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_RESOURCE_TIMING_H_
