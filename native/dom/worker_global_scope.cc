#include "mitto/dom/worker_global_scope.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_worker_global_scope.h"
#include "mitto/dom/cache_storage.h"
#include "mitto/dom/classic_pending_script.h"
#include "mitto/dom/crypto.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/idb_factory.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/referrer_script_info.h"
#include "mitto/dom/scheduler.h"
#include "mitto/dom/script_fetch_options.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/worker_location.h"
#include "mitto/dom/worker_navigator.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "url/url_constants.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {

ImportedScript::ImportedScript() = default;
ImportedScript::ImportedScript(ImportedScript&&) = default;
ImportedScript& ImportedScript::operator=(ImportedScript&&) = default;
ImportedScript::~ImportedScript() = default;

WorkerGlobalScope::WorkerGlobalScope() = default;
WorkerGlobalScope::~WorkerGlobalScope() = default;

const bindings::WrapperTypeInfo* WorkerGlobalScope::GetWrapperTypeInfo() const {
  return &bindings::V8WorkerGlobalScope::kWrapperTypeInfo;
}

void WorkerGlobalScope::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  ExecutionContext::Trace(visitor);
  visitor->Trace(performance_);
  visitor->Trace(location_);
  visitor->Trace(navigator_);
  visitor->Trace(crypto_);
  visitor->Trace(scheduler_);
  visitor->Trace(caches_);
  visitor->Trace(indexed_db_);
  visitor->Trace(modulator_);
}

mitto::Crypto* WorkerGlobalScope::Crypto() {
  if (!crypto_) {
    crypto_ = mitto::Crypto::Create();
  }
  return crypto_.Get();
}

mitto::Scheduler* WorkerGlobalScope::Scheduler() {
  if (!scheduler_) {
    scheduler_ = bindings::MakeGarbageCollected<mitto::Scheduler>(this);
  }
  return scheduler_.Get();
}

mitto::CacheStorage* WorkerGlobalScope::Caches(
    bindings::ExceptionState& exception_state) {
  // GlobalCacheStorage::CanCreateCacheStorage(): not for an opaque origin.
  if (!cache_storage_host_) {
    exception_state.ThrowDOMException("SecurityError",
                                      "Access to cache storage is denied.");
    return nullptr;
  }
  if (!caches_) {
    caches_ = bindings::MakeGarbageCollected<mitto::CacheStorage>(this);
  }
  return caches_.Get();
}

CacheStorageHost* WorkerGlobalScope::GetCacheStorageHost() const {
  return cache_storage_host_;
}

IDBFactory* WorkerGlobalScope::IndexedDB() {
  if (!indexed_db_) {
    indexed_db_ = bindings::MakeGarbageCollected<IDBFactory>(this);
  }
  return indexed_db_.Get();
}

IndexedDbHost* WorkerGlobalScope::GetIndexedDbHost() const {
  return indexed_db_host_;
}

StorageBucketHost* WorkerGlobalScope::GetStorageBucketHost() const {
  return storage_bucket_host_;
}

// WorkerModulatorImpl, made the first time.
Modulator* WorkerGlobalScope::GetModulator() {
  if (!modulator_) {
    modulator_ = bindings::MakeGarbageCollected<Modulator>(*this);
  }
  return modulator_.Get();
}

WorkerNavigator* WorkerGlobalScope::Navigator() {
  if (!navigator_) {
    navigator_ = WorkerNavigator::Create(Start(), Connection());
  }
  return navigator_.Get();
}

WorkerLocation* WorkerGlobalScope::Location() {
  if (!location_) {
    location_ = WorkerLocation::Create(url_);
  }
  return location_.Get();
}

mitto::Performance* WorkerGlobalScope::Performance() {
  if (!performance_) {
    performance_ = Performance::CreateForWorker(TimeOrigin());
  }
  return performance_.Get();
}

std::u16string WorkerGlobalScope::Origin() const {
  return base::UTF8ToUTF16(origin_.Serialize());
}

bool WorkerGlobalScope::IsSecureContextForBindings() const {
  // A worker is a secure context if its creator is, which the Page decides
  // for its context when it starts the worker.
  return IsSecureContext();
}

// WorkerGlobalScope::ImportScriptsInternal().
void WorkerGlobalScope::ImportScripts(
    const std::vector<std::u16string>& urls,
    bindings::ExceptionState& exception_state) {
  if (module_) {
    exception_state.ThrowTypeError(
        "Module scripts don't support importScripts().");
    return;
  }
  if (urls.empty()) {
    return;
  }
  // Every URL parses before any script is fetched.
  std::vector<GURL> resolved;
  resolved.reserve(urls.size());
  for (const std::u16string& url : urls) {
    GURL complete = CompleteUrl(url);
    if (!complete.is_valid()) {
      exception_state.ThrowDOMException(
          "SyntaxError",
          base::StrCat({"The URL '", base::UTF16ToUTF8(url), "' is invalid."}));
      return;
    }
    resolved.push_back(std::move(complete));
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  for (const GURL& url : resolved) {
    const std::string failed =
        base::StrCat({"The script at '", ElidedUrl(url), "' failed to load."});
    std::optional<ImportedScript> script =
        imported_script_loader_ ? imported_script_loader_->Load(url)
                                : std::nullopt;
    // WorkerClassicScriptLoader::DidReceiveResponse(): a response that is not
    // 2xx, or not of a JavaScript MIME type, as importScripts() checks it
    // strictly, fails to load.
    bool loaded = script && (script->status == 0 ||
                             (script->status >= 200 && script->status < 300));
    if (loaded) {
      if (std::optional<std::string> refused = RefusedAsWorkerScript(
              script->url, script->headers, /*strict=*/true)) {
        AddConsoleMessage(ConsoleLevel::kError, *std::move(refused));
        loaded = false;
      }
    }
    if (!loaded) {
      exception_state.ThrowDOMException("NetworkError", failed);
      return;
    }
    const std::unique_ptr<TextResourceDecoder> decoder =
        BuildClassicScriptDecoder(
            "UTF-8",
            GetMimeTypeAndCharset(HeaderField(script->headers, "content-type"))
                .charset);
    std::u16string source = decoder->Decode(base::as_byte_span(script->body));
    source += decoder->Flush();
    // Named after the URL asked for, without its fragment, and its errors
    // muted if its response was opaque.
    GURL::Replacements no_fragment;
    no_fragment.ClearRef();
    const std::string name = url.ReplaceComponents(no_fragment).spec();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    bool muted = false;
    {
      v8::TryCatch try_catch(isolate);
      v8::Local<v8::String> name_string;
      v8::Local<v8::String> code;
      v8::Local<v8::Script> compiled;
      if (v8::String::NewFromUtf8(isolate, name.c_str())
              .ToLocal(&name_string) &&
          v8::String::NewFromTwoByte(
              isolate, reinterpret_cast<const uint16_t*>(source.data()),
              v8::NewStringType::kNormal,
              base::checked_cast<int>(source.size()))
              .ToLocal(&code)) {
        // ImportScriptsInternal(): its base URL, which its import() resolves
        // against, is its response's, or about:blank for an opaque response,
        // as a window's classic script's is, with the default fetch options.
        v8::ScriptOrigin origin = ClassicScriptOrigin(
            name_string, /*muted_errors=*/script->opaque, /*line_offset=*/0,
            /*column_offset=*/0,
            ReferrerScriptInfo(
                script->opaque ? GURL(url::kAboutBlankURL) : script->url,
                ScriptFetchOptions())
                .ToV8HostDefinedOptions(isolate, GURL(name)));
        v8::ScriptCompiler::Source script_source(code, origin);
        if (v8::ScriptCompiler::Compile(context, &script_source)
                .ToLocal(&compiled) &&
            !compiled->Run(context).IsEmpty()) {
          continue;
        }
      }
      // RethrowErrorsOption::Rethrow(): what the script threw goes on, as a
      // NetworkError for a script whose errors are muted, and the scripts
      // after it are not run.
      if (!try_catch.HasCaught() || !try_catch.CanContinue()) {
        return;
      }
      if (!script->opaque) {
        try_catch.ReThrow();
        return;
      }
      muted = true;
    }
    // Thrown once the TryCatch has gone, which would otherwise catch it.
    if (muted) {
      exception_state.ThrowDOMException("NetworkError", failed);
    }
    return;
  }
}

// As Window::StructuredClone(), in the worker's context.
v8::Local<v8::Value> WorkerGlobalScope::StructuredClone(
    v8::Local<v8::Value> value,
    const bindings::StructuredSerializeOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (!HasContext()) {
    return v8::Undefined(isolate);
  }
  Transferables transferables;
  std::optional<SerializedScriptValue> serialized =
      MessagePort::SerializeMessageByMove(isolate, value, options.transfer,
                                          transferables);
  if (!serialized) {
    return v8::Undefined(isolate);
  }
  std::optional<std::vector<MessagePortChannel>> channels =
      MessagePort::DisentanglePorts(isolate, transferables.message_ports);
  if (!channels) {
    return v8::Undefined(isolate);
  }
  const std::vector<cppgc::Persistent<MessagePort>> ports =
      MessagePort::EntanglePorts(this, *std::move(channels));
  return serialized->Deserialize(isolate, MessagePort::PortsOf(ports));
}

std::u16string WorkerGlobalScope::Url() const {
  return base::UTF8ToUTF16(url_.possibly_invalid_spec());
}

EventTarget* WorkerGlobalScope::ErrorEventTarget() {
  return this;
}

url::Origin WorkerGlobalScope::GetSecurityOrigin() const {
  return origin_;
}

GURL WorkerGlobalScope::BaseUrl() const {
  return url_;
}

GURL WorkerGlobalScope::CompleteUrl(std::u16string_view url) const {
  // A worker's URLs are UTF-8, whatever its creator's document has.
  return url_.Resolve(url);
}

void WorkerGlobalScope::AssociateWithContext(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
