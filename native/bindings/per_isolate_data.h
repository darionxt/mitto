// What the bindings keep for a Page's isolate: its features, its time origin,
// the template of each interface, made once, the dictionaries being converted
// and how to make a DOMException.
//
// Upstream: third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h

#ifndef MITTO_BINDINGS_PER_ISOLATE_DATA_H_
#define MITTO_BINDINGS_PER_ISOLATE_DATA_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

struct ExposedConstructInfo;
class ScriptWrappable;

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

  // Reports an exception that a page's callback threw and nothing caught, as a
  // console reports it.
  using ExceptionReporter =
      base::RepeatingCallback<void(v8::Local<v8::Value> exception)>;

  // Tells the Session that a document reached a Known Gap, named as the
  // listing names it, such as "Navigator.prototype.getBattery".
  using KnownGapReporter = base::RepeatingCallback<void(std::string_view name)>;

  // How the bindings make an object of an interface without a C++ class whose
  // nearest ancestor with a class is EventTarget or Event: an object of that
  // class whose wrapper is of `info`, which the DOM makes, since the bindings
  // cannot see its classes.
  struct ObjectHooks {
    ScriptWrappable* (*create_event_target)(const WrapperTypeInfo& info) =
        nullptr;
    ScriptWrappable* (*create_event)(const WrapperTypeInfo& info,
                                     const std::u16string& type,
                                     bool bubbles,
                                     bool cancelable,
                                     bool composed) = nullptr;
    // The handler of the event handler attribute for `type` of `receiver`,
    // into `handler`, null for none. Returns false for a receiver that has no
    // EventTarget behind it.
    bool (*get_event_handler)(v8::Isolate* isolate,
                              v8::Local<v8::Object> receiver,
                              const std::u16string& type,
                              v8::Local<v8::Value>* handler) = nullptr;
    // Sets it, null for anything that is not an object. Returns false for a
    // receiver that has no EventTarget behind it.
    bool (*set_event_handler)(v8::Isolate* isolate,
                              v8::Local<v8::Object> receiver,
                              const std::u16string& type,
                              v8::Local<v8::Value> handler) = nullptr;
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

  // What DOMHighResTimeStamp counts from: when the Page started, or the start
  // of the navigation that loaded its document.
  base::TimeTicks time_origin() const { return time_origin_; }
  void set_time_origin(base::TimeTicks time_origin) {
    time_origin_ = time_origin;
  }

  // Where ReportException() sends what it reports. Without one, it goes to
  // standard error.
  const ExceptionReporter& exception_reporter() const {
    return exception_reporter_;
  }
  void SetExceptionReporter(ExceptionReporter reporter) {
    exception_reporter_ = std::move(reporter);
  }

  // Whether the Page's context is a secure context, which decides whether it
  // gets the members that Web IDL's [SecureContext] puts behind one. A Page's
  // first document is about:blank, which is. The document it loads decides it
  // for the context made for that document.
  bool secure_context() const { return secure_context_; }
  void set_secure_context(bool secure_context) {
    secure_context_ = secure_context;
  }

  // The template of an interface or a callback interface, which this makes
  // the first time.
  v8::Local<v8::FunctionTemplate> InterfaceTemplate(
      v8::Isolate* isolate,
      const WrapperTypeInfo* info);

  // The template of a namespace, which this makes the first time.
  v8::Local<v8::ObjectTemplate> NamespaceTemplate(v8::Isolate* isolate,
                                                  const WrapperTypeInfo* info);

  // The template of a [LegacyFactoryFunction], which this makes the first
  // time, as Blink's CreateLegacyFactoryFunctionFunction() does: its function
  // can be called with `new`, takes the factory's name and length, and
  // inherits from its interface's template.
  v8::Local<v8::FunctionTemplate> FactoryFunctionTemplate(
      v8::Isolate* isolate,
      const ExposedConstructInfo& construct);

  // Whether `value` is an object of the interface or of one that inherits from
  // it.
  bool HasInstance(v8::Isolate* isolate,
                   const WrapperTypeInfo& info,
                   v8::Local<v8::Value> value) const;

  // Where the bindings report Known Gaps. Without one, they report nothing.
  void SetKnownGapReporter(KnownGapReporter reporter) {
    known_gap_reporter_ = std::move(reporter);
  }

  // Whether the Known Gap of `number` is reached for the first time in this
  // document, which then remembers it, as Blink's UseCounter counts a feature
  // once for each document.
  bool FirstReachOfKnownGap(uint16_t number);
  // Reports a Known Gap, once for each document.
  void ReportKnownGap(uint16_t number, std::string_view name);
  // Forgets the Known Gaps reported, for a new document.
  void StartDocument() { reported_known_gaps_.clear(); }

  // The private symbol under which an object keeps what the setter of the
  // Known Gap of `number` stored, or the object its getter gave.
  v8::Local<v8::Private> KnownGapPrivate(v8::Isolate* isolate, uint16_t number);

  void SetObjectHooks(ObjectHooks hooks) { object_hooks_ = hooks; }
  const ObjectHooks& object_hooks() const { return object_hooks_; }

  void SetDOMExceptionHooks(DOMExceptionHooks hooks) {
    dom_exception_hooks_ = hooks;
  }
  const DOMExceptionHooks& dom_exception_hooks() const {
    return dom_exception_hooks_;
  }

  // What OmitExceptionContextScope sets: whether an error that leaves a
  // binding call now gets no context in front of its message.
  void set_omit_exception_context(bool omit) { omit_exception_context_ = omit; }
  bool omit_exception_context() const { return omit_exception_context_; }

  // What DictionaryConversionScope keeps.
  void PushDictionaryContext(std::string_view dictionary_name);
  void SetDictionaryPropertyName(std::string_view property_name);
  void PopDictionaryContext();
  std::vector<DictionaryContext> dictionary_contexts_innermost_first() const;

 private:
  PerIsolateData(v8::Isolate* isolate, RuntimeFeatureSet features);

  const raw_ptr<v8::Isolate> isolate_;
  const RuntimeFeatureSet features_;
  base::TimeTicks time_origin_;
  ExceptionReporter exception_reporter_;
  bool secure_context_ = true;
  std::map<const WrapperTypeInfo*, v8::Eternal<v8::Template>> templates_;
  std::map<const ExposedConstructInfo*, v8::Eternal<v8::FunctionTemplate>>
      factory_function_templates_;
  DOMExceptionHooks dom_exception_hooks_;
  KnownGapReporter known_gap_reporter_;
  std::vector<bool> reported_known_gaps_;
  std::vector<v8::Eternal<v8::Private>> known_gap_privates_;
  ObjectHooks object_hooks_;
  std::vector<DictionaryContext> dictionary_contexts_;
  bool omit_exception_context_ = false;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_PER_ISOLATE_DATA_H_
