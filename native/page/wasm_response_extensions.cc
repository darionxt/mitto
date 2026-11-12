#include "mitto/page/wasm_response_extensions.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_response.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/response.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-wasm.h"

namespace mitto {
namespace {

// What ApplyContextToException() puts in front of a message, for the
// operation "compile" of "WebAssembly".
constexpr std::string_view kContext =
    "Failed to execute 'compile' on 'WebAssembly': ";

v8::Local<v8::String> Message(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

// PropagateExceptionToWasmStreaming(), with a TypeError.
void AbortWithTypeError(v8::Isolate* isolate,
                        v8::WasmStreaming& streaming,
                        std::string_view message) {
  streaming.Abort(v8::Exception::TypeError(
      Message(isolate, base::StrCat({kContext, message}))));
}

// FetchDataLoaderForWasmStreaming, once the whole body has come: V8 gets it
// and compiles, or the compilation is aborted as when the body fails.
void OnBody(std::shared_ptr<v8::WasmStreaming> streaming,
            std::optional<std::string> body) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  if (!body) {
    // AbortCompilation(), with the empty message of the data pipe's error.
    streaming->Abort(v8::Exception::TypeError(
        Message(isolate, "WebAssembly compilation aborted: Network error: ")));
    return;
  }
  if (!body->empty()) {
    const base::span<const uint8_t> bytes = base::as_byte_span(*body);
    streaming->OnBytesReceived(bytes.data(), bytes.size());
  }
  streaming->Finish({});
}

// StreamFromResponseCallback().
void StreamFromResponse(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(isolate, info.DataV2());
  Response* const response = bindings::ToWrappableOf<Response>(
      isolate, bindings::V8Response::kWrapperTypeInfo, info[0]);
  if (!response) {
    AbortWithTypeError(isolate, *streaming,
                       "An argument must be provided, which must be a "
                       "Response or Promise<Response> object");
    return;
  }
  if (!response->Ok()) {
    AbortWithTypeError(isolate, *streaming, "HTTP status code is not ok");
    return;
  }
  // The whole Content-Type, with no parameters allowed, but in any case.
  const std::optional<std::string> content_type =
      response->Headers()->GetValue("content-type");
  if (!content_type ||
      !base::EqualsCaseInsensitiveASCII(*content_type, "application/wasm")) {
    AbortWithTypeError(
        isolate, *streaming,
        "Incorrect response MIME type. Expected 'application/wasm'.");
    return;
  }
  if (response->BodyUsed()) {
    AbortWithTypeError(
        isolate, *streaming,
        "Cannot compile WebAssembly.Module from an already read Response");
    return;
  }
  if (response->HasNullBody()) {
    // A status of 204 or 205, or a 200 with no body.
    streaming->Abort(v8::Exception::WasmCompileError(Message(
        isolate, base::StrCat({kContext, "Empty WebAssembly module"}))));
    return;
  }
  const std::string url = base::UTF16ToUTF8(response->Url());
  streaming->SetUrl(url.data(), url.size());
  response->ReadAll(base::BindOnce(&OnBody, std::move(streaming)));
}

}  // namespace

void InitializeWasmResponseExtensions(v8::Isolate* isolate) {
  isolate->SetWasmStreamingCallback(&StreamFromResponse);
}

}  // namespace mitto
