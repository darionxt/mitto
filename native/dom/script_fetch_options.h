// The HTML Standard's script fetch options, as Blink's ScriptFetchOptions has
// them: what a script element's attributes ask of its fetch, which a module
// script keeps and passes on to the modules that it imports, and the import()
// calls of a script.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/script_fetch_options.h

#ifndef MITTO_DOM_SCRIPT_FETCH_OPTIONS_H_
#define MITTO_DOM_SCRIPT_FETCH_OPTIONS_H_

#include <optional>
#include <string>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/subresource_integrity.h"

namespace mitto {

// The HTML Standard's parser metadata.
enum class ParserDisposition {
  kParserInserted,
  kNotParserInserted,
};

// Whether a request blocks rendering.
enum class RenderBlockingBehavior {
  kNonBlocking,
  kBlocking,
};

struct ScriptFetchOptions {
  ScriptFetchOptions();
  ScriptFetchOptions(const ScriptFetchOptions&);
  ScriptFetchOptions& operator=(const ScriptFetchOptions&);
  ~ScriptFetchOptions();

  // The cryptographic nonce.
  std::u16string nonce;
  // The integrity metadata, and the text that it was parsed from, null where
  // there was none.
  IntegrityMetadataSet integrity_metadata;
  std::optional<std::u16string> integrity_attribute;
  ParserDisposition parser_state = ParserDisposition::kNotParserInserted;
  FetchCredentialsMode credentials_mode = FetchCredentialsMode::kSameOrigin;
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  FetchPriorityHint fetch_priority_hint = FetchPriorityHint::kAuto;
  RenderBlockingBehavior render_blocking = RenderBlockingBehavior::kNonBlocking;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCRIPT_FETCH_OPTIONS_H_
