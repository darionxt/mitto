// The window's open(), as Blink's LocalDOMWindow::open() runs it. A target that
// names a frame navigates it, and any other asks for a new window, which
// Chrome's pop-up blocker refuses, since no user ever activates a Page: no
// window opens, open() gives null, and the console says nothing, as in Chrome.
//
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.cc
// Upstream: third_party/blink/renderer/core/page/create_window.cc

#include <stddef.h>

#include <string>
#include <string_view>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/frame_navigation.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

namespace {

// What a frame that is there takes of GetWindowFeaturesFromString(), whose
// other features only change a new window.
struct WindowFeatures {
  bool noopener = false;
  bool noreferrer = false;
};

WindowFeatures ParseWindowFeatures(const std::u16string& features) {
  WindowFeatures result;
  const std::string buffer = base::ToLowerASCII(base::UTF16ToUTF8(features));
  constexpr std::string_view kSeparators = " \t\n\r=,\f";
  const auto is_separator = [&](size_t i) {
    return kSeparators.find(buffer[i]) != std::string_view::npos;
  };
  const size_t length = buffer.size();
  for (size_t i = 0; i < length;) {
    while (i < length && is_separator(i)) {
      ++i;
    }
    const size_t key_begin = i;
    while (i < length && !is_separator(i)) {
      ++i;
    }
    const std::string_view key =
        std::string_view(buffer).substr(key_begin, i - key_begin);
    // The separators past the key, but for '='.
    while (i < length && buffer[i] != '=' && buffer[i] != ',' &&
           is_separator(i)) {
      ++i;
    }
    std::string_view value;
    if (i < length && is_separator(i)) {
      while (i < length && is_separator(i) && buffer[i] != ',') {
        ++i;
      }
      const size_t value_begin = i;
      while (i < length && !is_separator(i)) {
        ++i;
      }
      value = std::string_view(buffer).substr(value_begin, i - value_begin);
    }
    // Blink's loose StringToInt(): the number that starts the value, or 0 for
    // none or one too large.
    size_t number_end =
        value.starts_with('-') || value.starts_with('+') ? 1 : 0;
    while (number_end < value.size() && base::IsAsciiDigit(value[number_end])) {
      ++number_end;
    }
    int number = 0;
    if (!base::StringToInt(value.substr(0, number_end), &number)) {
      number = 0;
    }
    const bool set =
        value.empty() || value == "yes" || value == "true" || number != 0;
    if (key == "noopener") {
      result.noopener = set;
    } else if (key == "noreferrer") {
      result.noreferrer = set;
    }
  }
  result.noopener = result.noopener || result.noreferrer;
  return result;
}

// Whether the origins are of one net::SchemefulSite.
bool IsSameSite(const url::Origin& a, const url::Origin& b) {
  return a.IsSameOriginWith(b) ||
         (!a.opaque() && !b.opaque() && a.scheme() == b.scheme() &&
          net::registry_controlled_domains::SameDomainOrHost(
              a, b,
              net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES));
}

// Whether `target` names a frame by where it is, which open() gives back
// whatever the features.
bool IsKeyword(const std::u16string& target) {
  return base::EqualsCaseInsensitiveASCII(target, u"_top") ||
         base::EqualsCaseInsensitiveASCII(target, u"_parent") ||
         base::EqualsCaseInsensitiveASCII(target, u"_self");
}

}  // namespace

Window* Window::Open(const std::u16string& url,
                     const std::u16string& target,
                     const std::u16string& features,
                     bindings::ExceptionState& exception_state) {
  // A window that its frame no longer shows opens nothing, nor does a script
  // of one, the entered window's.
  Window* const entered = Entered();
  if (!HasContext() || !entered || !entered->HasContext() ||
      !entered->HasDocument()) {
    return nullptr;
  }
  mitto::Document& document = entered->GetDocument();
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
  WindowFeatures window_features = ParseWindowFeatures(features);
  // A blob: URL of another site than the storage key's top-level one, the
  // top-level page's, or an opaque document's own, takes no opener.
  if (completed.SchemeIsBlob() && frame_) {
    const url::Origin origin = entered->GetSecurityOrigin();
    if (!IsSameSite(BlobUrlRegistry::Get().OriginOf(completed),
                    origin.opaque()
                        ? origin
                        : frame_->Top().DomWindow()->GetSecurityOrigin())) {
      window_features.noopener = true;
    }
  }
  // An empty target is _blank. The Page program's own window has no frame, and
  // names itself alone.
  mitto::Frame* frame = nullptr;
  Window* opened = nullptr;
  if (frame_) {
    frame = FindFrameForNavigation(
        *frame_, target.empty() ? std::u16string_view(u"_blank") : target,
        completed);
    opened = frame ? frame->DomWindow() : nullptr;
  } else if (IsKeyword(target)) {
    opened = this;
  }
  if (!opened) {
    return nullptr;
  }
  // A frame that is there is navigated, but not to an empty URL, with no
  // reason, so that a document that loads does not replace its entry for it.
  if (!url.empty()) {
    FrameLoadRequest request;
    request.url = completed;
    request.reason = ClientNavigationReason::kNone;
    request.origin_window = entered;
    if (window_features.noreferrer) {
      request.referrer_policy = FetchReferrerPolicy::kNoReferrer;
    }
    Navigate(*opened, request, FrameLoadType::kStandard);
  }
  if (IsKeyword(target) || !frame) {
    return frame ? frame->DomWindow() : opened;
  }
  if (window_features.noopener) {
    return nullptr;
  }
  frame->SetOpener(frame_);
  return frame->DomWindow();
}

}  // namespace mitto
