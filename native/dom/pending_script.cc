#include "mitto/dom/pending_script.h"

#include <utility>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/script_element_base.h"
#include "mitto/dom/window.h"

namespace mitto {

// Upstream: third_party/blink/renderer/platform/weborigin/kurl.cc
std::string ElidedUrl(const GURL& url) {
  const std::string& spec = url.possibly_invalid_spec();
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat(
      {spec.substr(0, 511), "...", spec.substr(spec.size() - 510)});
}

std::string HeaderBytesToUtf8(std::string_view bytes) {
  std::u16string characters;
  characters.reserve(bytes.size());
  for (const char byte : bytes) {
    characters.push_back(static_cast<unsigned char>(byte));
  }
  return base::UTF16ToUTF8(characters);
}

PendingScript::PendingScript(ScriptElementBase& element)
    : element_(&element.AsElement()),
      original_element_document_(&element.AsElement().GetDocument()),
      original_window_(element.AsElement().GetDocument().DefaultView()) {}

PendingScript::~PendingScript() = default;

void PendingScript::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(element_);
  visitor->Trace(original_element_document_);
  visitor->Trace(original_window_);
}

void PendingScript::WatchForLoad(base::OnceClosure on_ready) {
  DCHECK(!IsWatchingForLoad());
  on_ready_ = std::move(on_ready);
  if (IsReady()) {
    std::move(on_ready_).Run();
  }
}

bool PendingScript::IsEligibleForLowPriorityAsyncScriptExecution() const {
  return false;
}

void PendingScript::PendingScriptFinished() {
  if (IsWatchingForLoad()) {
    std::move(on_ready_).Run();
  }
}

void PendingScript::ExecuteScriptBlock() {
  Element* const element = element_.Get();
  const bool is_external = IsExternal();
  const bool is_module = IsModule();
  Window* const original_window = original_window_.Get();
  Document* const original_document = original_element_document_.Get();
  Dispose();
  if (!element) {
    return;
  }
  // A script that has moved to another context or document does not run.
  Document& document = element->GetDocument();
  Window* const window = document.DefaultView();
  if (!window || window != original_window || &document != original_document) {
    return;
  }
  // Step 2 of execute the script element: a script that is null fires
  // error.
  if (!HasScript()) {
    ToScriptElement(*element)->DispatchErrorEvent();
    return;
  }
  {
    // Step 3: a script from a file, or a module script, cannot write into
    // its document unless the document was opened for it: the HTML
    // Standard's ignore-destructive-writes counter.
    const IgnoreDestructiveWriteCountIncrementer ignore_destructive_writes(
        is_external || is_module ? &document : nullptr);
    // Step 4: a classic script is the current script while it runs, and a
    // module script leaves none.
    Element* const current_script = is_module ? nullptr : element;
    document.PushCurrentScript(current_script);
    RunScript();
    document.PopCurrentScript(current_script);
  }
  // Step 6.
  if (is_external) {
    ToScriptElement(*element)->DispatchLoadEvent();
  }
}

void PendingScript::Dispose() {
  StopWatchingForLoad();
  // Rendering is no longer blocked on the element, as Blink's Dispose()
  // unblocks it, which running the script does first.
  if (Element* const element = element_.Get()) {
    element->GetDocument().RemovePendingRenderBlockingScript(*element);
  }
  element_ = nullptr;
}

}  // namespace mitto
