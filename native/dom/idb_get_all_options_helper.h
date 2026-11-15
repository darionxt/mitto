// What getAll() and getAllKeys() of an object store or an index ask for, as
// Blink's IDBGetAllOptionsHelper reads it: their first argument is a query,
// a key or a key range, with the count as their second, or an
// IDBGetAllOptions dictionary, whose own count stands and the second argument
// goes unread. An object that is no key and no key range is the dictionary.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_get_all_options_helper.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_get_all_options_helper.cc

#ifndef MITTO_DOM_IDB_GET_ALL_OPTIONS_HELPER_H_
#define MITTO_DOM_IDB_GET_ALL_OPTIONS_HELPER_H_

#include <stdint.h>

#include "mitto/bindings/dictionaries.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

// CreateFromArgumentsOrDictionary(): the options of `range_or_options` into
// `*options`, with `max_count` unless it is the dictionary. Returns false if
// converting the dictionary threw.
bool IDBGetAllOptionsFromArgumentsOrDictionary(
    v8::Isolate* isolate,
    v8::Local<v8::Value> range_or_options,
    uint32_t max_count,
    bindings::ExceptionState& exception_state,
    bindings::IDBGetAllOptions* options);

// GetCount(): the options' count, unless there is none or it is 0, which
// asks for every record.
uint32_t IDBGetAllOptionsCount(const bindings::IDBGetAllOptions& options);

}  // namespace mitto

#endif  // MITTO_DOM_IDB_GET_ALL_OPTIONS_HELPER_H_
