// What the bindings keep for a Page's isolate: its features, its time origin,
// the template of each interface, made once, the dictionaries being converted
// and how to make a DOMException.
//
// Upstream: third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h

#ifndef MITTO_BINDINGS_PER_ISOLATE_DATA_H_
#define MITTO_BINDINGS_PER_ISOLATE_DATA_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

class PerIsolateData {
 public:
  // What the bindings need of DOMException, whose class is the DOM's, which
  // the bindings themselves cannot see.
  struct DOMExceptionHooks {
    // Makes a DOMException with `name` and `message`.
    v8::Local<v8::Value> (*create)(v8::Isolate* isolate,
                                   std::string_view name,
                                   std::string_view message) = nullptr;
    // Puts the context of a call in front of the message of `exception`, and
    // returns true, if it is a DOMException.
    bool (*add_context)(v8::Isolate* isolate,
                        v8::Local<v8::Value> exception,
                        v8::ExceptionContext context_type,
                        std::string_view class_name,
                        std::string_view property_name) = nullptr;
  };

  // A dictionary being converted, and the member it reads.
  struct DictionaryContext {
    std::string dictionary_name;
    std::string property_name;
  };

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

  // Whether the Page's context is a secure context, which decides whether it
  // gets the members that Web IDL's [SecureContext] puts behind one. A Page's
  // first document is about:blank, which is. The document it loads decides it
  // for the context made for that document.
  bool secure_context() const { return secure_context_; }
  void set_secure_context(bool secure_context) {
    secure_context_ = secure_context;
  }

  // The interface's template, which this makes the first time.
  v8::Local<v8::FunctionTemplate> InterfaceTemplate(
      v8::Isolate* isolate,
      const WrapperTypeInfo* info);

  // Whether `value` is an object of the interface or of one that inherits from
  // it.
  bool HasInstance(v8::Isolate* isolate,
                   const WrapperTypeInfo& info,
                   v8::Local<v8::Value> value) const;

  void SetDOMExceptionHooks(DOMExceptionHooks hooks) {
    dom_exception_hooks_ = hooks;
  }
  const DOMExceptionHooks& dom_exception_hooks() const {
    return dom_exception_hooks_;
  }

  // What DictionaryConversionScope keeps.
  void PushDictionaryContext(std::string_view dictionary_name);
  void SetDictionaryPropertyName(std::string_view property_name);
  void PopDictionaryContext();
  std::vector<DictionaryContext> dictionary_contexts_innermost_first() const;

 private:
  PerIsolateData(v8::Isolate* isolate, RuntimeFeatureSet features);

  const raw_ptr<v8::Isolate> isolate_;
  const RuntimeFeatureSet features_;
  const base::TimeTicks time_origin_;
  bool secure_context_ = true;
  std::map<const WrapperTypeInfo*, v8::Eternal<v8::FunctionTemplate>>
      templates_;
  DOMExceptionHooks dom_exception_hooks_;
  std::vector<DictionaryContext> dictionary_contexts_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_PER_ISOLATE_DATA_H_
