// User-Agent Client Hints' NavigatorUAData, what navigator.userAgentData gives:
// the values that the Session's user agent client hints send, from the same
// UserAgentMetadata.
//
// Upstream: third_party/blink/renderer/core/frame/navigator_ua_data.h
// Upstream: third_party/blink/renderer/core/frame/navigator_ua_data.cc

#ifndef MITTO_DOM_NAVIGATOR_UA_DATA_H_
#define MITTO_DOM_NAVIGATOR_UA_DATA_H_

#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class NavigatorUAData final : public bindings::ScriptWrappable {
 public:
  static NavigatorUAData* Create(const PageUserAgentData& data);

  explicit NavigatorUAData(const PageUserAgentData& data);
  ~NavigatorUAData() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  // A new frozen array each time, as Blink converts its list on each read.
  v8::Local<v8::Value> Brands() const;
  bool Mobile() const;
  std::u16string Platform() const;
  v8::Local<v8::Promise> GetHighEntropyValues(
      const std::vector<std::u16string>& hints) const;
  v8::Local<v8::Object> ToJSON() const;

 private:
  const PageUserAgentData data_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATOR_UA_DATA_H_
