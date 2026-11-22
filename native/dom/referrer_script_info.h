// What import() in a script needs of the script that calls it: its base URL
// and its fetch options, which Blink keeps in the host-defined options of the
// ScriptOrigin that each script is compiled with, and V8 hands back to the
// embedder's dynamic import callback, as Blink's ReferrerScriptInfo does.
// Code that eval() or the Function constructor makes in a script gets the
// script's.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/referrer_script_info.h
// Upstream: third_party/blink/renderer/bindings/core/v8/referrer_script_info.cc

#ifndef MITTO_DOM_REFERRER_SCRIPT_INFO_H_
#define MITTO_DOM_REFERRER_SCRIPT_INFO_H_

#include <string>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/script_fetch_options.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-data.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class ReferrerScriptInfo {
 public:
  ReferrerScriptInfo();
  ReferrerScriptInfo(const GURL& base_url, const ScriptFetchOptions& options);
  ReferrerScriptInfo(const GURL& base_url,
                     FetchCredentialsMode credentials_mode,
                     std::u16string nonce,
                     ParserDisposition parser_state,
                     FetchReferrerPolicy referrer_policy);
  ReferrerScriptInfo(const ReferrerScriptInfo&);
  ReferrerScriptInfo& operator=(const ReferrerScriptInfo&);
  ~ReferrerScriptInfo();

  // What the host-defined options of a script compiled with the resource
  // name `script_origin_resource_name` hold, as V8 hands them over. A script
  // whose options are the defaults has none, and its base URL is its name.
  static ReferrerScriptInfo FromV8HostDefinedOptions(
      v8::Local<v8::Context> context,
      v8::Local<v8::Data> host_defined_options,
      const GURL& script_origin_resource_name);
  // The host-defined options to compile a script named
  // `script_origin_resource_name` with: none for the defaults.
  v8::Local<v8::Data> ToV8HostDefinedOptions(
      v8::Isolate* isolate,
      const GURL& script_origin_resource_name) const;

  // The base URL, or an empty one where the script has none of its own, as
  // one that the caller evaluates.
  const GURL& BaseUrl() const { return base_url_; }
  FetchCredentialsMode CredentialsMode() const { return credentials_mode_; }
  const std::u16string& Nonce() const { return nonce_; }
  ParserDisposition ParserState() const { return parser_state_; }
  FetchReferrerPolicy GetReferrerPolicy() const { return referrer_policy_; }

 private:
  bool IsDefaultValue(const GURL& script_origin_resource_name) const;

  GURL base_url_;
  FetchCredentialsMode credentials_mode_ = FetchCredentialsMode::kSameOrigin;
  std::u16string nonce_;
  ParserDisposition parser_state_ = ParserDisposition::kNotParserInserted;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
};

}  // namespace mitto

#endif  // MITTO_DOM_REFERRER_SCRIPT_INFO_H_
