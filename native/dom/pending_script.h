// A script that a script element has prepared, until it runs: classic, which
// classic_pending_script.h has, or a module graph, which
// module_pending_script.h has. Each is ready once its source, or its graph,
// has come or failed, and then the HTML Standard's "execute the script
// element" runs it, with document.currentScript for a classic script, and
// fires load or error, as Blink's PendingScript does.
//
// Upstream: third_party/blink/renderer/core/script/pending_script.cc

#ifndef MITTO_DOM_PENDING_SCRIPT_H_
#define MITTO_DOM_PENDING_SCRIPT_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class Element;
class ScriptElementBase;
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

// KURL::ElidedString(), for a URL in a console message.
std::string ElidedUrl(const GURL& url);

// The text of a header's bytes in UTF-8, each byte a character, as Blink's
// String of a header has it, for a console message that quotes it.
std::string HeaderBytesToUtf8(std::string_view bytes);

class PendingScript : public cppgc::GarbageCollected<PendingScript> {
 public:
  virtual ~PendingScript();

  virtual void Trace(cppgc::Visitor* visitor) const;

  Element* element() const { return element_.Get(); }
  ScriptSchedulingType scheduling_type() const { return scheduling_type_; }
  void set_scheduling_type(ScriptSchedulingType type) {
    scheduling_type_ = type;
  }
  // Whether it came from a src, which fires load once it has run.
  virtual bool IsExternal() const = 0;
  // Whether it has its source, or has failed: the HTML Standard's "ready to
  // be parser-executed".
  virtual bool IsReady() const = 0;
  // Runs `on_ready` once it is ready, at once if it is.
  void WatchForLoad(base::OnceClosure on_ready);
  void StopWatchingForLoad() { on_ready_.Reset(); }
  bool IsWatchingForLoad() const { return !on_ready_.is_null(); }
  // ClassicPendingScript::IsEligibleForLowPriorityAsyncScriptExecution(): only
  // a classic script may run at a low priority.
  virtual bool IsEligibleForLowPriorityAsyncScriptExecution() const;

  // The HTML Standard's "execute the script element", which disposes of this.
  void ExecuteScriptBlock();
  // Lets go of the element and stops watching.
  void Dispose();

 protected:
  explicit PendingScript(ScriptElementBase& element);

  // What the subclass has once it is ready.
  //
  // Whether the script's script is null, which fires error.
  virtual bool HasScript() const = 0;
  // Whether it is a module script, which has no current script, and whose
  // document ignores destructive writes while it runs.
  virtual bool IsModule() const = 0;
  // Runs the script's script in the current context. It is called after
  // Dispose(), which keeps what the script needs.
  virtual void RunScript() = 0;
  // What the subclass lets go of when the script is disposed of, such as its
  // fetch.
  virtual void DisposeInternal() {}

  // Tells the one that watches it that it is ready.
  void PendingScriptFinished();

  Document* original_element_document() const {
    return original_element_document_.Get();
  }
  Window* original_window() const { return original_window_.Get(); }

 private:
  cppgc::Member<Element> element_;
  // The document and window it was prepared in: it does not run in another.
  cppgc::WeakMember<Document> original_element_document_;
  cppgc::WeakMember<Window> original_window_;
  ScriptSchedulingType scheduling_type_ = ScriptSchedulingType::kNotSet;
  base::OnceClosure on_ready_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PENDING_SCRIPT_H_
