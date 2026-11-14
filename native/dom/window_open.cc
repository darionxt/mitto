// The window's open(), as Blink's LocalDOMWindow::open() runs it. A Page has
// one frame, its top-level one, and no user ever activates it, so a target
// that names it navigates it, and any other asks for a new window, which
// Chrome's pop-up blocker refuses: no window opens, open() gives null, and the
// console says nothing, as in Chrome.
//
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.cc
// Upstream: third_party/blink/renderer/core/page/create_window.cc
// Upstream: third_party/blink/renderer/core/page/frame_tree.cc

#include <string>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"

namespace mitto {

Window* Window::Open(const std::u16string& url,
                     const std::u16string& target,
                     const std::u16string& features,
                     bindings::ExceptionState& exception_state) {
  // A window that its frame no longer shows opens nothing.
  if (!HasContext()) {
    return nullptr;
  }
  // The entered window's document, which is this window's in a Page.
  mitto::Document& document = GetDocument();
  GURL completed;
  if (!url.empty()) {
    completed = document.CompleteUrl(url);
    if (!completed.is_valid()) {
      const ResolvedURL resolved(
          ResolvedURL(base::UTF8ToUTF16(document.BaseUrl().spec())), url);
      exception_state.ThrowDOMException(
          "SyntaxError",
          base::StrCat({"Unable to open a window with invalid URL '",
                        base::UTF16ToUTF8(resolved.GetString()), "'.\n"}));
      return nullptr;
    }
  }
  // FrameTree::FindFrameForNavigationInternal(), for a Page's one frame,
  // whose name is empty: an empty target is _blank. The features only change
  // a new window, and what open() gives for one.
  const bool names_this_frame =
      !target.empty() && (base::EqualsCaseInsensitiveASCII(target, u"_self") ||
                          base::EqualsCaseInsensitiveASCII(target, u"_top") ||
                          base::EqualsCaseInsensitiveASCII(target, u"_parent"));
  if (!names_this_frame) {
    // CreateNewWindow(), whose ChromeContentBrowserClient::CanCreateWindow()
    // blocks a pop-up that no user gesture asked for.
    return nullptr;
  }
  // A frame that is there is navigated, but not to an empty URL, with no
  // reason, so that a document that loads does not replace its entry for it,
  // and it is given back even for noopener.
  if (!url.empty()) {
    FrameLoadRequest request;
    request.url = completed;
    request.reason = ClientNavigationReason::kNone;
    Navigate(*this, request, FrameLoadType::kStandard);
  }
  return this;
}

}  // namespace mitto
