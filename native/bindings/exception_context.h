// Puts the context of a binding call in front of the message of an error that
// leaves it, as Chrome does: "Failed to execute 'x' on 'Y': ", "Failed to
// construct 'Y': " and the like, and for each dictionary being converted,
// "Failed to read the 'm' property from 'D': ".
//
// Chrome does this in V8's exception propagation callback, which V8 calls when
// an error is thrown directly in a call of a function template, with the
// template's interface name, name and kind of call. So an error that V8 throws
// during the call, such as converting a Symbol to a string, gets the context
// too, and one that a page's own function throws does not.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_initializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.cc

#ifndef MITTO_BINDINGS_EXCEPTION_CONTEXT_H_
#define MITTO_BINDINGS_EXCEPTION_CONTEXT_H_

#include <string_view>

#include "base/memory/raw_ptr.h"
#include "v8/include/v8-forward.h"

namespace mitto::bindings {

class PerIsolateData;

// Makes the isolate put the context in front of errors from binding calls.
void SetUpExceptionContext(v8::Isolate* isolate);

// While one exists, an error that leaves a binding call gets the context of a
// member of the dictionary that the call is converting, as Blink's
// DictionaryConversionContext gives it.
class DictionaryConversionScope {
 public:
  DictionaryConversionScope(v8::Isolate* isolate,
                            std::string_view dictionary_name);
  DictionaryConversionScope(const DictionaryConversionScope&) = delete;
  DictionaryConversionScope& operator=(const DictionaryConversionScope&) =
      delete;
  ~DictionaryConversionScope();

  // The member that the conversion reads now.
  void SetCurrentPropertyName(std::string_view property_name);

 private:
  const raw_ptr<PerIsolateData> data_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_EXCEPTION_CONTEXT_H_
