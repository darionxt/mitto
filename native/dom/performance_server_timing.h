// Server Timing's PerformanceServerTiming: a metric of a response's
// Server-Timing header, which a resource's entry and the navigation's give
// when the response passed the TAO check, parsed as Blink parses the header.
//
// Upstream: third_party/blink/renderer/core/timing/performance_server_timing.cc
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
// Upstream: third_party/blink/renderer/platform/network/header_field_tokenizer.cc
// Upstream: third_party/blink/renderer/platform/network/server_timing_header.cc

#ifndef MITTO_DOM_PERFORMANCE_SERVER_TIMING_H_
#define MITTO_DOM_PERFORMANCE_SERVER_TIMING_H_

#include <string>
#include <string_view>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document_load_timing.h"
#include "v8/include/v8-forward.h"

namespace mitto {

// ParseServerTimingHeader(): the metrics of a Server-Timing value, its bytes
// read as Latin-1, as many as parse before the first that does not.
std::vector<ServerTimingInfo> ParseServerTimingHeader(std::string_view value);

class PerformanceServerTiming final : public bindings::ScriptWrappable {
 public:
  explicit PerformanceServerTiming(const ServerTimingInfo& info);
  ~PerformanceServerTiming() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return info_.name; }
  double Duration() const { return info_.duration; }
  const std::u16string& Description() const { return info_.description; }
  v8::Local<v8::Object> ToJSONForBinding() const;

 private:
  const ServerTimingInfo info_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_SERVER_TIMING_H_
