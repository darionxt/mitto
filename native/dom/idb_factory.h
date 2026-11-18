// The indexedDB of a window or a worker, IndexedDB's IDBFactory, as Blink's:
// open() and deleteDatabase() make a request that the Session answers,
// databases() lists the databases of the context's storage key, and cmp()
// compares two keys. A context whose origin may not reach IndexedDB, which has
// no IndexedDbHost, gets Chrome's SecurityError from each but cmp().
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
class ExecutionContext;

class IDBFactory final : public bindings::ScriptWrappable {
 public:
  explicit IDBFactory(ExecutionContext* execution_context);
  ~IDBFactory() override;

  // A factory of no context, for a Known Gap that gives one, as
  // StorageBucket's indexedDB: its calls do what they do once a context has
  // gone.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

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
  // GetValidContext(): the context, while it is there, or null.
  ExecutionContext* ValidContext() const;
  IDBOpenDBRequest* OpenInternal(const std::u16string& name,
                                 int64_t version,
                                 bindings::ExceptionState& exception_state);

  const cppgc::WeakMember<ExecutionContext> execution_context_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_FACTORY_H_
