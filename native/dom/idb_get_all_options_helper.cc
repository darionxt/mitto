#include "mitto/dom/idb_get_all_options_helper.h"

#include <limits>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_idb_key_range.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-value.h"

namespace mitto {
namespace {

// TryCreateGetAllOptions(): whether `range_or_options` is the dictionary,
// which is then converted into `*options`. Returns false if that threw.
bool TryCreateGetAllOptions(v8::Isolate* isolate,
                            v8::Local<v8::Value> range_or_options,
                            bindings::ExceptionState& exception_state,
                            bindings::IDBGetAllOptions* options,
                            bool* is_dictionary) {
  *is_dictionary = false;
  if (range_or_options->IsNullOrUndefined()) {
    return true;
  }
  // A key that is no object, such as a number or a string.
  if (!range_or_options->IsObject()) {
    return true;
  }
  if (bindings::V8IDBKeyRange::ToWrappable(isolate, range_or_options)) {
    return true;
  }
  // An object that is a key, not a dictionary.
  if (range_or_options->IsDate() || range_or_options->IsArray() ||
      range_or_options->IsArrayBuffer() ||
      range_or_options->IsArrayBufferView()) {
    return true;
  }
  *is_dictionary = true;
  return bindings::FromV8(isolate, range_or_options, exception_state, options);
}

}  // namespace

bool IDBGetAllOptionsFromArgumentsOrDictionary(
    v8::Isolate* isolate,
    v8::Local<v8::Value> range_or_options,
    uint32_t max_count,
    bindings::ExceptionState& exception_state,
    bindings::IDBGetAllOptions* options) {
  bool is_dictionary = false;
  if (!TryCreateGetAllOptions(isolate, range_or_options, exception_state,
                              options, &is_dictionary)) {
    return false;
  }
  if (!is_dictionary) {
    // The query and the count of the arguments.
    *options = bindings::IDBGetAllOptions();
    options->query = range_or_options;
    options->count = max_count;
  }
  return true;
}

uint32_t IDBGetAllOptionsCount(const bindings::IDBGetAllOptions& options) {
  if (options.count && *options.count > 0) {
    return *options.count;
  }
  return std::numeric_limits<uint32_t>::max();
}

}  // namespace mitto
