// A classic script that a script element has prepared: its source text, or
// its fetch through its document's ResourceFetcher and the checks that Blink
// makes of what comes, until it is ready, and then running it, as Blink's
// ClassicPendingScript and ClassicScript do.
//
// Upstream: third_party/blink/renderer/core/script/classic_pending_script.cc
// Upstream: third_party/blink/renderer/core/script/classic_script.cc
// Upstream: third_party/blink/renderer/platform/loader/allowed_by_nosniff.cc

#ifndef MITTO_DOM_CLASSIC_PENDING_SCRIPT_H_
#define MITTO_DOM_CLASSIC_PENDING_SCRIPT_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/dom/pending_script.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/script_fetch_options.h"
#include "mitto/dom/subresource_integrity.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ScriptElementBase;

// The checks of a fetched classic script's response that make it a network
// error, with the console error of each: AllowedByNosniff::MimeTypeAsScript()
// for an element's script, which Blink holds to the lax check.
std::optional<std::string> RefusedAsScript(const GURL& url,
                                           std::string_view headers);

class ClassicPendingScript final : public PendingScript, public ResourceClient {
 public:
  // An inline script of `element`, which is ready, whose errors name
  // `source_url`, empty for a script that the parser did not insert, and
  // count lines and columns from `start_line` and `start_column`, where its
  // text starts in the document. Its import() calls resolve against
  // `base_url`, with `options`.
  static ClassicPendingScript* CreateInline(ScriptElementBase& element,
                                            std::u16string source,
                                            GURL source_url,
                                            GURL base_url,
                                            const ScriptFetchOptions& options,
                                            int start_line,
                                            int start_column);
  // `element`'s script that `params` asks for, which this fetches through
  // its document's ResourceFetcher, with the mode, credentials mode, referrer
  // policy and priority hint that it has, as a script that blocks rendering
  // if it says so, and checks against its integrity metadata. It decodes as
  // the encoding of the request that made the resource, which a request of
  // the same URL may have made before, unless its response or a byte order
  // mark names another. Its import() calls take `options`.
  static ClassicPendingScript* Fetch(ScriptElementBase& element,
                                     const FetchParameters& params,
                                     const ScriptFetchOptions& options);

  ClassicPendingScript(ScriptElementBase& element,
                       bool is_external,
                       const ScriptFetchOptions& options);
  ~ClassicPendingScript() override;

  void Trace(cppgc::Visitor* visitor) const override;

  // PendingScript:
  bool IsExternal() const override;
  bool IsReady() const override;
  bool IsEligibleForLowPriorityAsyncScriptExecution() const override;

  // ResourceClient: ClassicPendingScript::NotifyFinished(), the checks of the
  // response.
  void NotifyFinished(Resource& resource) override;

 private:
  enum class State {
    kWaitingForResource,
    kReady,
    kErrorOccurred,
  };

  // PendingScript:
  bool HasScript() const override;
  bool IsModule() const override;
  void RunScript() override;
  // ClassicPendingScript::DisposeInternal(): a script that goes lets go of
  // its fetch, which is cancelled when nothing else uses it.
  void DisposeInternal() override;

  void StartFetch(const FetchParameters& params);
  void AdvanceReadyState(State state);

  const bool is_external_;
  State state_;
  const ScriptFetchOptions options_;
  std::u16string source_;
  GURL source_url_;
  // The script's base URL, which its import() calls resolve against.
  GURL base_url_;
  int start_line_ = 0;
  int start_column_ = 0;
  // Whether it came from another origin without CORS, whose errors scripts
  // do not see.
  bool muted_errors_ = false;
  GURL fetch_url_;
  IntegrityMetadataSet integrity_;
  cppgc::Member<Resource> resource_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CLASSIC_PENDING_SCRIPT_H_
