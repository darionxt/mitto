// IndexedDB's keys and key paths as a page's values meet them, as Blink's
// bindings for modules have them: a value converted to a key, which numbers,
// strings, dates, binary data and arrays of them make, a key back to a value,
// a key path parsed and evaluated on a value, the keys of a value for an
// index, and a key that a key generator made put into a value that comes back
// from the Session. And the comparison of two values as keys that
// indexedDB.cmp() makes.
//
// A page's getter or proxy may run where Blink reads a property of a value,
// and may throw. An ExceptionState here learns only of what is thrown through
// it, so a function that can throw returns false when anything did, with the
// exception pending, as the bindings' own conversions do, where Blink's
// callers ask their ExceptionState.
//
// The Runtime has no Blob or File, so no key path reads the size or type of a
// Blob, or the name, lastModified or lastModifiedDate of a File, which Blink's
// read without side effects, and none of those counts as a property that a
// generated key cannot go into.
//
// Upstream: third_party/blink/renderer/bindings/modules/v8/v8_binding_for_modules.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_key.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_key_path.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_object_store.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory.cc

#ifndef MITTO_DOM_IDB_KEY_CONVERSION_H_
#define MITTO_DOM_IDB_KEY_CONVERSION_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

// IDBDatabase::kNotValidKeyErrorMessage: the message of the DataError for a
// value that does not convert to a valid key.
inline constexpr char kIDBNotValidKeyErrorMessage[] =
    "The parameter is not a valid key.";

// Converts `value` to a key into `*key`, as Blink's CreateIDBKeyFromValue()
// does: a number but NaN, a string, a Date but an invalid one, the bytes of an
// ArrayBuffer, a SharedArrayBuffer or a view, or an array of such, in which a
// member that is none of these is an invalid key, as a multiEntry index takes
// it. Anything else is an invalid key, and so is an array with a hole, one
// that holds itself, one longer than IDBKey::kMaximumArraySize and one nested
// deeper than IDBKey::kMaximumDepth. Throws Chrome's errors for a resizable
// buffer, a detached one and one larger than Blink takes. Returns false if
// anything threw.
bool CreateIDBKeyFromValue(v8::Isolate* isolate,
                           v8::Local<v8::Value> value,
                           bindings::ExceptionState& exception_state,
                           IDBKey* key);

// The same, and throws Chrome's DataError, kIDBNotValidKeyErrorMessage, for a
// key that is not valid, as each of Blink's callers that needs a valid key
// does.
bool CreateValidIDBKeyFromValue(v8::Isolate* isolate,
                                v8::Local<v8::Value> value,
                                bindings::ExceptionState& exception_state,
                                IDBKey* key);

// IDBKey::ToV8(): `key` as a page sees it, in the current context: null for
// None, a number, a new Date, a string, a new ArrayBuffer of a binary key's
// bytes, or a new array of its members' values. Empty if putting a member into
// an array failed. `key` is not invalid.
v8::Local<v8::Value> IDBKeyToV8(v8::Isolate* isolate, const IDBKey& key);

// Blink's IDBParseKeyPath(): the identifiers that dots separate in
// `key_path`, none for the empty key path, or nullopt if it is not a valid key
// path. An identifier is ECMA-262's, read a UTF-16 code unit at a time, as
// Blink reads it.
std::optional<std::vector<std::u16string>> IDBParseKeyPath(
    std::u16string_view key_path);

// Blink's IDBIsValidKeyPath(): whether IDBParseKeyPath() takes `key_path`.
bool IDBIsValidKeyPath(std::u16string_view key_path);

// IDBKeyPath::IsValid(): a string that is a valid key path, or an array of at
// least one such string. A null key path is not valid.
bool IDBKeyPathIsValid(const IDBKeyPath& key_path);

// The IDL's (DOMString or sequence<DOMString>), as the bindings give it: the
// type of their DOMStringOrDOMStringSequence.
using IDBKeyPathUnion =
    std::variant<std::u16string, std::vector<std::u16string>>;

// IDBKeyPath's constructor from the union: a string or an array key path, and
// for a nullable one, a null key path for nullopt.
IDBKeyPath IDBKeyPathFromUnion(const IDBKeyPathUnion& key_path);
IDBKeyPath IDBKeyPathFromUnion(const std::optional<IDBKeyPathUnion>& key_path);

// IDBKeyPath::ToV8(): null, a string, or a new array of strings.
v8::Local<v8::Value> IDBKeyPathToV8(v8::Isolate* isolate,
                                    const IDBKeyPath& key_path);

// Evaluates `key_path`, a valid one that is not null, on `value` into `*key`,
// as Blink's CreateIDBKeyFromValueAndKeyPath() does: the key that the value
// at the path converts to, which may be invalid, or for an array key path, an
// array of the key of each of its strings, whose members may be invalid; or
// nullopt where evaluating fails, when a step reaches a value that is not an
// object or has no own property of its name. The length of a string or an
// array counts as one. Returns false if anything threw.
bool CreateIDBKeyFromValueAndKeyPath(v8::Isolate* isolate,
                                     v8::Local<v8::Value> value,
                                     const IDBKeyPath& key_path,
                                     bindings::ExceptionState& exception_state,
                                     std::optional<IDBKey>* key);

// The same for the key path of an index, `index_key_path`, in an object store
// whose key path is `store_key_path`, as Blink's
// CreateIDBKeyFromValueAndKeyPaths() does. An array key path gives an invalid
// key if any of its keys is not valid, and a hole, None, where a string of it
// that is the store's own key path finds nothing: the primary key that the
// store's key generator will make goes there.
bool CreateIDBKeyFromValueAndKeyPaths(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value,
                                      const IDBKeyPath& store_key_path,
                                      const IDBKeyPath& index_key_path,
                                      bindings::ExceptionState& exception_state,
                                      std::optional<IDBKey>* key);

// Blink's GenerateIndexKeysForValue(): the keys that `value`, the clone of
// what IDBObjectStore.put() or add() stores, has in the index of
// `index_metadata`, in an object store whose key path is `store_key_path`,
// which may be null. None if the index's key path finds nothing or an invalid
// key; for a multiEntry index and an array, each of its valid members once, in
// the order of keys; and otherwise the one key, which may have holes. Blink
// evaluates with a NonThrowableExceptionState, which in Chrome drops what is
// thrown through it, as for a resizable ArrayBuffer that the clone holds: the
// value then has no key in the index, and nothing stays thrown.
IndexedDbIndexKeys GenerateIndexKeysForValue(
    v8::Isolate* isolate,
    const IDBKeyPath& store_key_path,
    const IndexedDbIndexMetadata& index_metadata,
    v8::Local<v8::Value> value);

// Blink's CanInjectIDBKeyIntoScriptValue(): whether a key that a key generator
// makes can go into `value`, the clone of what is stored, at `key_path`, a
// string, when the value comes back: the value is an object, and each step of
// the path reaches an object that lacks the property, which it can take, or
// has it as an own property that is not a string's or an array's length.
bool CanInjectIDBKeyIntoScriptValue(v8::Isolate* isolate,
                                    v8::Local<v8::Value> value,
                                    const IDBKeyPath& key_path);

// Blink's InjectV8KeyIntoV8Value(): puts `key`, the value of the primary key
// that a key generator made, into `value`, which deserializing a stored value
// gave, at `key_path`, a string that is not empty, with a new object for each
// step before the last that finds no own property. Nothing goes in for a last
// step that is a string's or an array's length. Returns false if a step
// reaches a value that is not an object, or anything failed, which a stored
// value that does not suit its object store can cause.
bool InjectV8KeyIntoV8Value(v8::Isolate* isolate,
                            v8::Local<v8::Value> key,
                            v8::Local<v8::Value> value,
                            const IDBKeyPath& key_path);

// IDBFactory::cmp(): -1, 0 or 1, as `first` sorts before, with or after
// `second`, each converted to a valid key, or Chrome's DataError for one that
// does not convert to one, the first first. 0 if anything threw.
int16_t CompareValuesAsIDBKeys(v8::Isolate* isolate,
                               v8::Local<v8::Value> first,
                               v8::Local<v8::Value> second,
                               bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_IDB_KEY_CONVERSION_H_
