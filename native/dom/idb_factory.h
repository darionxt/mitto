// The window's indexedDB, IndexedDB's IDBFactory, as Blink's: open() and
// deleteDatabase() make a request that the Session answers, databases()
// lists the databases of the document's storage key, and cmp() compares two
// keys. A document whose origin may not reach IndexedDB, which has no
// IndexedDbHost, gets Chrome's SecurityError from each but cmp().
//
// Chrome asks its content settings once whether a frame may use IndexedDB,
// before the first open; a Page's profile allows it, so each call goes to the
// Session at once.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/global_indexed_db.cc

#ifndef MITTO_DOM_IDB_FACTORY_H_
#define MITTO_DOM_IDB_FACTORY_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class IDBOpenDBRequest;
class IndexedDbHost;
class StorageBucket;
class Window;

class IDBFactory final : public bindings::ScriptWrappable {
 public:
  explicit IDBFactory(Window* window);
  // The factory of `bucket`'s databases, which StorageBucket's indexedDB gives.
  IDBFactory(Window* window, StorageBucket* bucket);
  ~IDBFactory() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  IDBOpenDBRequest* Open(const std::u16string& name,
                         std::optional<uint64_t> version,
                         bindings::ExceptionState& exception_state);
  IDBOpenDBRequest* DeleteDatabase(const std::u16string& name,
                                   bindings::ExceptionState& exception_state);
  int16_t Cmp(v8::Local<v8::Value> first,
              v8::Local<v8::Value> second,
              bindings::ExceptionState& exception_state);
  // databases().
  v8::Local<v8::Promise> GetDatabaseInfo(
      bindings::ExceptionState& exception_state);

 private:
  // GetValidContext(): the window, while it has its context, or null.
  Window* ValidWindow() const;
  IDBOpenDBRequest* OpenInternal(const std::u16string& name,
                                 int64_t version,
                                 bindings::ExceptionState& exception_state);

  // The number of the bucket that each of its calls names, 0 for the default.
  uint64_t BucketNumber() const;

  const cppgc::WeakMember<Window> window_;
  // Kept for as long as the factory is, since its number names the bucket.
  const cppgc::Member<StorageBucket> bucket_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_FACTORY_H_
