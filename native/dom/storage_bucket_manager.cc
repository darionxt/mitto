#include "mitto/dom/storage_bucket_manager.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_storage_bucket_manager.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/storage_bucket.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

constexpr char kDestroyed[] = "The window/worker has been destroyed.";
constexpr char kDenied[] =
    "Access to Storage Buckets API is denied in this context.";

// IsValidName(): 1 to 63 of lowercase Latin letters and digits, and "-" and
// "_" but first.
bool IsValidName(const std::u16string& name) {
  if (name.empty() || name.size() >= 64) {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    const char16_t c = name[i];
    const bool alphanumeric =
        (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
    if (!alphanumeric && (i == 0 || (c != u'_' && c != u'-'))) {
      return false;
    }
  }
  return true;
}

v8::Local<v8::Value> TypeError(v8::Isolate* isolate,
                               std::u16string_view message) {
  return v8::Exception::TypeError(bindings::ToV8String(isolate, message));
}

v8::Local<v8::Value> UnknownError(v8::Isolate* isolate,
                                  std::u16string_view message) {
  return DOMException::Create(std::u16string(message), u"UnknownError")
      ->ToV8(isolate);
}

// The Window and its host for a call, or an exception thrown for none.
StorageBucketHost* HostForCall(bindings::ExceptionState& exception_state) {
  Window& window = Window::Current();
  if (!window.HasContext()) {
    exception_state.ThrowTypeError(kDestroyed);
    return nullptr;
  }
  // CanAccessStorageBuckets(): an origin that is not opaque, which only a
  // document that the Session loaded over HTTP or HTTPS has.
  StorageBucketHost* const host = window.GetDocument().GetStorageBucketHost();
  if (!host) {
    exception_state.ThrowDOMException("SecurityError", kDenied);
  }
  return host;
}

using Settle =
    base::OnceCallback<void(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Promise::Resolver> resolver,
                            PageStorageBucketAnswer answer)>;

// Asks the Session, and settles the promise of `resolver` with what `settle`
// makes of the answer in a task of its own, unless the window has gone.
void Ask(v8::Isolate* isolate,
         v8::Local<v8::Promise::Resolver> resolver,
         StorageBucketHost& host,
         PageStorageBucketCall call,
         Settle settle) {
  host.Ask(std::move(call),
           base::BindOnce(
               [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
                  v8::Global<v8::Promise::Resolver> resolver_handle,
                  Settle settle, PageStorageBucketAnswer answer) {
                 RunInCacheContext(isolate, context_handle,
                                   [&](v8::Local<v8::Context> context) {
                                     std::move(settle).Run(
                                         isolate, context,
                                         resolver_handle.Get(isolate),
                                         std::move(answer));
                                   });
               },
               isolate,
               v8::Global<v8::Context>(isolate, isolate->GetCurrentContext()),
               v8::Global<v8::Promise::Resolver>(isolate, resolver),
               std::move(settle)));
}

}  // namespace

// static
StorageBucketManager* StorageBucketManager::Create() {
  return bindings::MakeGarbageCollected<StorageBucketManager>();
}

StorageBucketManager::StorageBucketManager() = default;
StorageBucketManager::~StorageBucketManager() = default;

const bindings::WrapperTypeInfo* StorageBucketManager::GetWrapperTypeInfo()
    const {
  return &bindings::V8StorageBucketManager::kWrapperTypeInfo;
}

v8::Local<v8::Promise> StorageBucketManager::Open(
    const std::u16string& name,
    const bindings::StorageBucketOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  StorageBucketHost* const host = HostForCall(exception_state);
  if (!host) {
    return v8::Local<v8::Promise>();
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  if (!IsValidName(name)) {
    std::ignore = resolver->Reject(
        context, TypeError(isolate, base::StrCat({u"The bucket name '", name,
                                                  u"' is not a valid name."})));
    return resolver->GetPromise();
  }
  if (options.quota && *options.quota == 0) {
    std::ignore = resolver->Reject(
        context, TypeError(isolate, u"The bucket's quota cannot equal zero."));
    return resolver->GetPromise();
  }
  // ToMojoBucketPolicies().
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kOpen;
  call.name = base::UTF16ToUTF8(name);
  call.persisted = options.persisted;
  call.quota = options.quota;
  if (options.durability) {
    call.durability = *options.durability == u"strict" ? 1 : 0;
  }
  if (options.expires) {
    call.expires = base::Time::FromMillisecondsSinceUnixEpoch(*options.expires)
                       .ToDeltaSinceWindowsEpoch()
                       .InMicroseconds();
  }
  Ask(isolate, resolver, *host, std::move(call),
      base::BindOnce(
          [](cppgc::WeakPersistent<Window> window, StorageBucketHost* host,
             std::u16string name, v8::Isolate* isolate,
             v8::Local<v8::Context> context,
             v8::Local<v8::Promise::Resolver> resolver,
             PageStorageBucketAnswer answer) {
            // DidOpen().
            switch (answer.error) {
              case StorageBucketError::kNone:
                std::ignore = resolver->Resolve(
                    context,
                    bindings::MakeGarbageCollected<StorageBucket>(
                        window.Get(), host, std::move(name), answer.bucket)
                        ->ToV8(isolate));
                return;
              case StorageBucketError::kUnknown:
                std::ignore = resolver->Reject(
                    context,
                    UnknownError(
                        isolate,
                        u"Unknown error occured while creating a bucket."));
                return;
              case StorageBucketError::kQuotaExceeded:
                // QuotaExceededError::Reject(), with neither quota nor
                // requested.
                if (bindings::PerIsolateData::From(isolate)
                        ->features()
                        .IsEnabled(bindings::RuntimeFeature::
                                       kQuotaExceededErrorUpdate)) {
                  std::ignore = resolver->Reject(
                      context,
                      QuotaExceededError::Create(u"Too many buckets created.",
                                                 std::nullopt, std::nullopt)
                          ->ToV8(isolate));
                } else {
                  std::ignore = resolver->Reject(
                      context,
                      DOMException::Create(u"Too many buckets created.",
                                           u"QuotaExceededError")
                          ->ToV8(isolate));
                }
                return;
              case StorageBucketError::kInvalidExpiration:
                std::ignore = resolver->Reject(
                    context,
                    TypeError(isolate, u"The bucket expiration is invalid."));
                return;
            }
          },
          cppgc::WeakPersistent<Window>(&Window::Current()), host, name));
  return resolver->GetPromise();
}

v8::Local<v8::Promise> StorageBucketManager::Keys(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  StorageBucketHost* const host = HostForCall(exception_state);
  if (!host) {
    return v8::Local<v8::Promise>();
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kKeys;
  Ask(isolate, resolver, *host, std::move(call),
      base::BindOnce([](v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Promise::Resolver> resolver,
                        PageStorageBucketAnswer answer) {
        if (!answer.success) {
          std::ignore = resolver->Reject(
              context,
              UnknownError(
                  isolate,
                  u"Unknown error occured while retrieving bucket names."));
          return;
        }
        std::vector<v8::Local<v8::Value>> names;
        names.reserve(answer.names.size());
        for (const std::string& name : answer.names) {
          names.push_back(bindings::ToV8StringFromUtf8(isolate, name));
        }
        std::ignore = resolver->Resolve(
            context, v8::Array::New(isolate, names.data(), names.size()));
      }));
  return resolver->GetPromise();
}

v8::Local<v8::Promise> StorageBucketManager::Delete(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  StorageBucketHost* const host = HostForCall(exception_state);
  if (!host) {
    return v8::Local<v8::Promise>();
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  if (!IsValidName(name)) {
    // Blink's message here quotes no name.
    std::ignore = resolver->Reject(
        context, TypeError(isolate, base::StrCat({u"The bucket name ", name,
                                                  u" is not a valid name."})));
    return resolver->GetPromise();
  }
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kDelete;
  call.name = base::UTF16ToUTF8(name);
  Ask(isolate, resolver, *host, std::move(call),
      base::BindOnce([](v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Promise::Resolver> resolver,
                        PageStorageBucketAnswer answer) {
        if (!answer.success) {
          std::ignore = resolver->Reject(
              context,
              UnknownError(isolate,
                           u"Unknown error occured while deleting a bucket."));
          return;
        }
        std::ignore = resolver->Resolve(context, v8::Undefined(isolate));
      }));
  return resolver->GetPromise();
}

}  // namespace mitto
