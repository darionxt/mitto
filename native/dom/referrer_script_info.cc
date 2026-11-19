#include "mitto/dom/referrer_script_info.h"

#include <stdint.h>

#include <utility>

#include "base/check.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

enum HostDefinedOptionsIndex : int {
  kBaseUrl,
  kCredentialsMode,
  kNonce,
  kParserState,
  kReferrerPolicy,
  kLength,
};

// The base URL to store: none where it is the script's name, or empty, both
// of which give the name back.
GURL GetStoredBaseUrl(const GURL& base_url,
                      const GURL& script_origin_resource_name) {
  if (base_url == script_origin_resource_name || base_url.is_empty()) {
    return GURL();
  }
  return base_url;
}

ReferrerScriptInfo Default(const GURL& script_origin_resource_name) {
  return ReferrerScriptInfo(
      script_origin_resource_name, FetchCredentialsMode::kSameOrigin, u"",
      ParserDisposition::kNotParserInserted, FetchReferrerPolicy::kDefault);
}

// The unsigned integer at `index`, which nothing but this file puts there.
uint32_t GetUint32(v8::Isolate* isolate,
                   v8::Local<v8::PrimitiveArray> options,
                   int index) {
  const v8::Local<v8::Primitive> value = options->Get(isolate, index);
  CHECK(value->IsUint32());
  return value.As<v8::Uint32>()->Value();
}

std::u16string GetString(v8::Isolate* isolate,
                         v8::Local<v8::PrimitiveArray> options,
                         int index) {
  const v8::Local<v8::Primitive> value = options->Get(isolate, index);
  CHECK(value->IsString());
  std::u16string text;
  bindings::FromV8String(isolate, value, &text);
  return text;
}

}  // namespace

ReferrerScriptInfo::ReferrerScriptInfo() = default;

ReferrerScriptInfo::ReferrerScriptInfo(const GURL& base_url,
                                       const ScriptFetchOptions& options)
    : ReferrerScriptInfo(base_url,
                         options.credentials_mode,
                         options.nonce,
                         options.parser_state,
                         options.referrer_policy) {}

ReferrerScriptInfo::ReferrerScriptInfo(const GURL& base_url,
                                       FetchCredentialsMode credentials_mode,
                                       std::u16string nonce,
                                       ParserDisposition parser_state,
                                       FetchReferrerPolicy referrer_policy)
    : base_url_(base_url),
      credentials_mode_(credentials_mode),
      nonce_(std::move(nonce)),
      parser_state_(parser_state),
      referrer_policy_(referrer_policy) {}

ReferrerScriptInfo::ReferrerScriptInfo(const ReferrerScriptInfo&) = default;
ReferrerScriptInfo& ReferrerScriptInfo::operator=(const ReferrerScriptInfo&) =
    default;
ReferrerScriptInfo::~ReferrerScriptInfo() = default;

bool ReferrerScriptInfo::IsDefaultValue(
    const GURL& script_origin_resource_name) const {
  return GetStoredBaseUrl(base_url_, script_origin_resource_name).is_empty() &&
         credentials_mode_ == FetchCredentialsMode::kSameOrigin &&
         nonce_.empty() &&
         parser_state_ == ParserDisposition::kNotParserInserted &&
         referrer_policy_ == FetchReferrerPolicy::kDefault;
}

// static
ReferrerScriptInfo ReferrerScriptInfo::FromV8HostDefinedOptions(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    const GURL& script_origin_resource_name) {
  if (host_defined_options.IsEmpty() || !host_defined_options->IsFixedArray()) {
    return Default(script_origin_resource_name);
  }
  const v8::Local<v8::PrimitiveArray> options =
      host_defined_options.As<v8::PrimitiveArray>();
  if (options->Length() == 0) {
    return Default(script_origin_resource_name);
  }
  CHECK_EQ(options->Length(), kLength);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const std::u16string base_url_string = GetString(isolate, options, kBaseUrl);
  // An empty base URL defers to the script's name.
  GURL base_url = base_url_string.empty() ? script_origin_resource_name
                                          : GURL(base_url_string);
  const uint32_t credentials_mode =
      GetUint32(isolate, options, kCredentialsMode);
  const uint32_t parser_state = GetUint32(isolate, options, kParserState);
  const uint32_t referrer_policy = GetUint32(isolate, options, kReferrerPolicy);
  CHECK_LE(credentials_mode,
           static_cast<uint32_t>(FetchCredentialsMode::kInclude));
  CHECK_LE(parser_state,
           static_cast<uint32_t>(ParserDisposition::kNotParserInserted));
  CHECK_LE(referrer_policy,
           static_cast<uint32_t>(FetchReferrerPolicy::kUnsafeUrl));
  return ReferrerScriptInfo(std::move(base_url),
                            static_cast<FetchCredentialsMode>(credentials_mode),
                            GetString(isolate, options, kNonce),
                            static_cast<ParserDisposition>(parser_state),
                            static_cast<FetchReferrerPolicy>(referrer_policy));
}

v8::Local<v8::Data> ReferrerScriptInfo::ToV8HostDefinedOptions(
    v8::Isolate* isolate,
    const GURL& script_origin_resource_name) const {
  if (IsDefaultValue(script_origin_resource_name)) {
    return v8::Local<v8::Data>();
  }
  const v8::Local<v8::PrimitiveArray> options =
      v8::PrimitiveArray::New(isolate, kLength);
  // Blink stores the base URL itself, not the one that GetStoredBaseUrl()
  // gives, which reads back the same.
  options->Set(
      isolate, kBaseUrl,
      bindings::ToV8StringFromUtf8(isolate, base_url_.possibly_invalid_spec()));
  options->Set(isolate, kCredentialsMode,
               v8::Integer::NewFromUnsigned(
                   isolate, static_cast<uint32_t>(credentials_mode_)));
  options->Set(isolate, kNonce, bindings::ToV8String(isolate, nonce_));
  options->Set(isolate, kParserState,
               v8::Integer::NewFromUnsigned(
                   isolate, static_cast<uint32_t>(parser_state_)));
  options->Set(isolate, kReferrerPolicy,
               v8::Integer::NewFromUnsigned(
                   isolate, static_cast<uint32_t>(referrer_policy_)));
  return options;
}

}  // namespace mitto
