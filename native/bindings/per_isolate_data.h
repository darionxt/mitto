// What the bindings keep for a Page's isolate: its features, its time origin,
// the template of each interface, made once, and how to make a DOMException.
//
// Upstream: third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h

#ifndef MITTO_BINDINGS_PER_ISOLATE_DATA_H_
#define MITTO_BINDINGS_PER_ISOLATE_DATA_H_

#include <map>
#include <memory>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

class PerIsolateData {
 public:
  // Makes a DOMException with `name` and `message`, as the DOM's class for it
  // does, which the bindings themselves cannot see.
  using DOMExceptionFactory = v8::Local<v8::Value> (*)(v8::Isolate* isolate,
                                                       std::string_view name,
                                                       std::string_view message);

  // Gives `isolate` its data, with the features of its Page. The data lasts as
  // long as the returned object, which must outlive every use of the bindings
  // in the isolate.
  static std::unique_ptr<PerIsolateData> Create(v8::Isolate* isolate,
                                                RuntimeFeatureSet features);
  static PerIsolateData* From(v8::Isolate* isolate);

  PerIsolateData(const PerIsolateData&) = delete;
  PerIsolateData& operator=(const PerIsolateData&) = delete;
  ~PerIsolateData();

  const RuntimeFeatureSet& features() const { return features_; }

  // When the Page started, which DOMHighResTimeStamp counts from.
  base::TimeTicks time_origin() const { return time_origin_; }

  // The interface's template, which this makes the first time.
  v8::Local<v8::FunctionTemplate> InterfaceTemplate(
      v8::Isolate* isolate,
      const WrapperTypeInfo* info);

  // Whether `value` is an object of the interface or of one that inherits from
  // it.
  bool HasInstance(v8::Isolate* isolate,
                   const WrapperTypeInfo& info,
                   v8::Local<v8::Value> value) const;

  void SetDOMExceptionFactory(DOMExceptionFactory factory) {
    dom_exception_factory_ = factory;
  }
  DOMExceptionFactory dom_exception_factory() const {
    return dom_exception_factory_;
  }

 private:
  PerIsolateData(v8::Isolate* isolate, RuntimeFeatureSet features);

  const raw_ptr<v8::Isolate> isolate_;
  const RuntimeFeatureSet features_;
  const base::TimeTicks time_origin_;
  std::map<const WrapperTypeInfo*, v8::Eternal<v8::FunctionTemplate>>
      templates_;
  DOMExceptionFactory dom_exception_factory_ = nullptr;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_PER_ISOLATE_DATA_H_
