#include "mitto/dom/idb_factory.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_idb_factory.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_factory_client.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/dom/idb_open_db_request.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

// DidGetDatabaseInfo(): rejects with the error the Session gave, a
// DOMException of the error's code, or resolves with an IDBDatabaseInfo of
// each database, its members in the order of Blink's ToV8 of a dictionary.
void SettleDatabaseInfo(v8::Isolate* isolate,
                        v8::Local<v8::Promise::Resolver> resolver,
                        const PageIndexedDbCallback& answer) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (answer.error != IndexedDbException::kNoError) {
    // A DOMException of the code, which is no QuotaExceededError object even
    // for a quota error.
    DOMException* const error =
        answer.error == IndexedDbException::kQuotaError
            ? DOMException::Create(answer.message, u"QuotaExceededError")
            : MakeIDBException(answer.error, answer.message);
    std::ignore = resolver->Reject(context, error->ToV8(isolate));
    return;
  }
  std::vector<v8::Local<v8::Value>> infos;
  infos.reserve(answer.databases.size());
  for (const auto& [name, version] : answer.databases) {
    const v8::Local<v8::Object> info = v8::Object::New(isolate);
    if (info->CreateDataProperty(
                context, v8::String::NewFromUtf8Literal(isolate, "name"),
                bindings::ToV8String(isolate, name))
            .IsNothing() ||
        info->CreateDataProperty(
                context, v8::String::NewFromUtf8Literal(isolate, "version"),
                v8::Number::New(isolate, static_cast<double>(version)))
            .IsNothing()) {
      return;
    }
    infos.push_back(info);
  }
  std::ignore = resolver->Resolve(
      context, v8::Array::New(isolate, infos.data(), infos.size()));
}

}  // namespace

IDBFactory::IDBFactory(Window* window) : window_(window) {}

IDBFactory::~IDBFactory() = default;

// static
bindings::ScriptWrappable* IDBFactory::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<IDBFactory>(
      info, bindings::V8IDBFactory::kWrapperTypeInfo, nullptr);
}

const bindings::WrapperTypeInfo* IDBFactory::GetWrapperTypeInfo() const {
  return &bindings::V8IDBFactory::kWrapperTypeInfo;
}

void IDBFactory::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

Window* IDBFactory::ValidWindow() const {
  Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    return nullptr;
  }
  return window;
}

v8::Local<v8::Promise> IDBFactory::GetDatabaseInfo(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  Window* const window = ValidWindow();
  if (!window) {
    // Blink's resolver->Reject(), with undefined.
    std::ignore = resolver->Reject(context, v8::Undefined(isolate));
    return resolver->GetPromise();
  }

  IndexedDbHost* const host = window->GetDocument().GetIndexedDbHost();
  if (!host) {
    // The bindings reject the promise with it.
    exception_state.ThrowDOMException(
        "SecurityError",
        "Access to the IndexedDB API is denied in this context.");
    return {};
  }

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kGetDatabaseInfo;
  host->Ask(std::move(call),
            base::BindRepeating(
                [](v8::Isolate* isolate, cppgc::WeakPersistent<Window> window,
                   v8::Global<v8::Promise::Resolver>* resolver,
                   PageIndexedDbCallback answer) {
                  // Nothing settles once the window's context has gone.
                  RunInIDBContext(window.Get(), [&] {
                    SettleDatabaseInfo(isolate, resolver->Get(isolate), answer);
                  });
                },
                isolate, cppgc::WeakPersistent<Window>(window),
                base::Owned(std::make_unique<v8::Global<v8::Promise::Resolver>>(
                    isolate, resolver))));
  return resolver->GetPromise();
}

IDBOpenDBRequest* IDBFactory::Open(const std::u16string& name,
                                   std::optional<uint64_t> version,
                                   bindings::ExceptionState& exception_state) {
  if (!version) {
    return OpenInternal(name, IDBDatabaseMetadata::kNoVersion, exception_state);
  }
  if (!*version) {
    exception_state.ThrowTypeError("The version provided must not be 0.");
    return nullptr;
  }
  // [EnforceRange] keeps it below 2^53.
  return OpenInternal(name, static_cast<int64_t>(*version), exception_state);
}

IDBOpenDBRequest* IDBFactory::OpenInternal(
    const std::u16string& name,
    int64_t version,
    bindings::ExceptionState& exception_state) {
  DCHECK(version >= 1 || version == IDBDatabaseMetadata::kNoVersion);
  Window* const window = ValidWindow();
  if (!window) {
    return nullptr;
  }
  IndexedDbHost* const host = window->GetDocument().GetIndexedDbHost();
  if (!host) {
    exception_state.ThrowDOMException(
        "SecurityError",
        "access to the Indexed Database API is denied in this context.");
    return nullptr;
  }

  const uint64_t connection = host->NextNumber();
  const uint64_t transaction_id = host->NextNumber();
  IDBOpenDBRequest* const request =
      bindings::MakeGarbageCollected<IDBOpenDBRequest>(
          window, this, host, name, connection, transaction_id, version);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kOpen;
  call.connection = connection;
  call.transaction = transaction_id;
  call.name = name;
  call.version = version;
  host->Ask(std::move(call),
            base::BindRepeating(&IDBFactoryClient::OnAnswer,
                                base::MakeRefCounted<IDBFactoryClient>(
                                    request, host, connection)));
  return request;
}

IDBOpenDBRequest* IDBFactory::DeleteDatabase(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  Window* const window = ValidWindow();
  if (!window) {
    return nullptr;
  }
  IndexedDbHost* const host = window->GetDocument().GetIndexedDbHost();
  if (!host) {
    exception_state.ThrowDOMException(
        "SecurityError",
        "access to the Indexed Database API is denied in this context.");
    return nullptr;
  }

  IDBOpenDBRequest* const request =
      bindings::MakeGarbageCollected<IDBOpenDBRequest>(
          window, this, host, name, /*connection=*/0, /*transaction_id=*/0,
          IDBDatabaseMetadata::kDefaultVersion);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDeleteDatabase;
  call.name = name;
  host->Ask(std::move(call),
            base::BindRepeating(&IDBFactoryClient::OnAnswer,
                                base::MakeRefCounted<IDBFactoryClient>(
                                    request, host, /*connection=*/0)));
  return request;
}

int16_t IDBFactory::Cmp(v8::Local<v8::Value> first,
                        v8::Local<v8::Value> second,
                        bindings::ExceptionState& exception_state) {
  return CompareValuesAsIDBKeys(v8::Isolate::GetCurrent(), first, second,
                                exception_state);
}

}  // namespace mitto
