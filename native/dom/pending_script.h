// A classic script that a script element has prepared: its source text, or
// its fetch through the Session and the checks that Blink makes of what comes,
// until it is ready, and then the HTML Standard's "execute the script
// element", with document.currentScript, and load or error.
//
// Upstream: third_party/blink/renderer/core/script/pending_script.cc
// Upstream: third_party/blink/renderer/core/script/classic_pending_script.cc
// Upstream: third_party/blink/renderer/platform/loader/allowed_by_nosniff.cc

#ifndef MITTO_DOM_PENDING_SCRIPT_H_
#define MITTO_DOM_PENDING_SCRIPT_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "mitto/dom/subresource_integrity.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class FetchController;
class HTMLScriptElement;
class Window;

// Blink's ScriptSchedulingType: who runs a script, and when.
enum class ScriptSchedulingType {
  kNotSet,
  // Once the document has finished parsing, in order.
  kDefer,
  // Before the parser goes on.
  kParserBlocking,
  // An inline script that waits for style sheets that block scripts.
  kParserBlockingInline,
  // In order, as soon as possible: a script the DOM inserted with async false.
  kInOrder,
  // As soon as it is ready.
  kAsync,
  // At once.
  kImmediate,
};

// The checks of a fetched classic script's response that make it a network
// error, with the console error of each: AllowedByNosniff::MimeTypeAsScript()
// for an element's script, which Blink holds to the lax check.
std::optional<std::string> RefusedAsScript(const GURL& url,
                                           std::string_view headers);
// KURL::ElidedString(), for a URL in a console message.
std::string ElidedUrl(const GURL& url);

class PendingScript final : public cppgc::GarbageCollected<PendingScript> {
 public:
  // An inline script of `element`, which is ready, whose errors name
  // `source_url`, empty for a script that the parser did not insert, and
  // count lines and columns from `start_line` and `start_column`, where its
  // text starts in the document.
  static PendingScript* CreateInline(HTMLScriptElement& element,
                                     std::u16string source,
                                     GURL source_url,
                                     int start_line,
                                     int start_column);
  // `element`'s script at `url`, which this fetches through its window's
  // Fetcher, as a script that blocks rendering if `render_blocking`, checks
  // against `integrity`, and decodes as `encoding` unless its response or a
  // byte order mark names another.
  static PendingScript* Fetch(HTMLScriptElement& element,
                              const GURL& url,
                              IntegrityMetadataSet integrity,
                              std::string encoding,
                              bool render_blocking);

  PendingScript(HTMLScriptElement& element, bool is_external);
  ~PendingScript();

  void Trace(cppgc::Visitor* visitor) const;

  HTMLScriptElement* element() const { return element_.Get(); }
  ScriptSchedulingType scheduling_type() const { return scheduling_type_; }
  void set_scheduling_type(ScriptSchedulingType type) {
    scheduling_type_ = type;
  }
  bool IsExternal() const { return is_external_; }
  // Whether it has its source, or has failed: the HTML Standard's "ready to
  // be parser-executed".
  bool IsReady() const { return state_ != State::kWaitingForResource; }
  // Runs `on_ready` once it is ready, at once if it is.
  void WatchForLoad(base::OnceClosure on_ready);
  void StopWatchingForLoad() { on_ready_.Reset(); }
  bool IsWatchingForLoad() const { return !on_ready_.is_null(); }
  // ClassicPendingScript::IsEligibleForLowPriorityAsyncScriptExecution(), for
  // an async script: one of another site, from a document that started less
  // than three seconds ago, not one that blocks rendering, on a machine of
  // three gigabytes or more.
  bool IsEligibleForLowPriorityAsyncScriptExecution() const;

  // The HTML Standard's "execute the script element", which disposes of this.
  void ExecuteScriptBlock();
  // Lets go of the element and stops watching.
  void Dispose();

 private:
  class FetchClient;
  friend class FetchClient;

  enum class State {
    kWaitingForResource,
    kReady,
    kErrorOccurred,
  };

  void StartFetch(const GURL& url, bool render_blocking);
  // ClassicPendingScript::NotifyFinished(): the checks of the response.
  void NotifyFinished(int net_error,
                      int status,
                      const GURL& response_url,
                      const std::string& headers,
                      bool opaque,
                      std::string body);
  void AdvanceReadyState(State state);

  cppgc::Member<HTMLScriptElement> element_;
  // The document and window it was prepared in: it does not run in another.
  cppgc::WeakMember<Document> original_element_document_;
  cppgc::WeakMember<Window> original_window_;
  const bool is_external_;
  State state_;
  ScriptSchedulingType scheduling_type_ = ScriptSchedulingType::kNotSet;
  base::OnceClosure on_ready_;
  std::u16string source_;
  GURL source_url_;
  int start_line_ = 0;
  int start_column_ = 0;
  // Whether it came from another origin without CORS, whose errors scripts
  // do not see.
  bool muted_errors_ = false;
  GURL fetch_url_;
  IntegrityMetadataSet integrity_;
  std::string encoding_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PENDING_SCRIPT_H_
