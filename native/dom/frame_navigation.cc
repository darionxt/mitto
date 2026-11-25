#include "mitto/dom/frame_navigation.h"

#include <stddef.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/remote_window.h"
#include "mitto/dom/sandbox_flags.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {

namespace {

// LocalFrame::PrintNavigationErrorMessage(). A frame of another process is
// known by its origin alone.
void PrintNavigationErrorMessage(Window& window,
                                 Window& target,
                                 std::string_view reason) {
  window.AddConsoleMessage(
      ConsoleLevel::kError,
      base::StrCat(
          {"Unsafe attempt to initiate navigation for frame ",
           IsInSameProcess(window.Process(), target.Process())
               ? base::StrCat(
                     {"with URL '",
                      base::UTF16ToUTF8(target.GetDocument().UrlForBinding())})
               : base::StrCat(
                     {"with origin '", target.GetSecurityOrigin().Serialize()}),
           "' from frame with URL '",
           base::UTF16ToUTF8(window.GetDocument().UrlForBinding()), "'. ",
           reason, "\n"}));
}

// CanAccessAncestor(): whether `origin` is that of `target` or of one of its
// ancestors.
bool CanAccessAncestor(const url::Origin& origin, const Frame& target) {
  for (const Frame* frame = &target; frame; frame = frame->Parent()) {
    if (origin.IsSameOriginWith(frame->DomWindow()->GetSecurityOrigin())) {
      return true;
    }
  }
  return false;
}

// network_utils::GetDomainAndRegistry() with private registries.
std::string DomainOf(std::string_view host) {
  return net::registry_controlled_domains::GetDomainAndRegistry(
      host, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

// The first of `root` and the frames under it, in tree order, that `found`
// takes, or null.
template <typename Found>
Frame* FindUnder(Frame& root, const Found& found) {
  std::vector<Frame*> pending = {&root};
  while (!pending.empty()) {
    Frame* const frame = pending.back();
    pending.pop_back();
    if (found(*frame)) {
      return frame;
    }
    for (size_t index = frame->ChildCount(); index > 0; --index) {
      pending.push_back(frame->ChildAt(index - 1));
    }
  }
  return nullptr;
}

// FrameTree::FindFrameForNavigationInternal(): the frame that `name` names, or
// null for none.
Frame* FindFrame(Frame& source, std::u16string_view name, const GURL& url) {
  if (name.empty() || base::EqualsCaseInsensitiveASCII(name, u"_self")) {
    return &source;
  }
  if (base::EqualsCaseInsensitiveASCII(name, u"_top")) {
    return &source.Top();
  }
  if (base::EqualsCaseInsensitiveASCII(name, u"_parent")) {
    return source.Parent() ? source.Parent() : &source;
  }
  if (base::EqualsCaseInsensitiveASCII(name, u"_blank")) {
    return nullptr;
  }
  // Its own frames first, then the rest of the tree.
  Frame* const under = FindUnder(source, [&](Frame& frame) {
    return frame.Name() == name && CanNavigate(source, frame, url);
  });
  return under ? under : FindUnder(source.Top(), [&](Frame& frame) {
    return frame.Name() == name && !frame.IsDescendantOf(source) &&
           CanNavigate(source, frame, url);
  });
}

}  // namespace

bool CanNavigate(Frame& source, Frame& target, const GURL& url) {
  if (&target == &source) {
    return true;
  }
  Window& window = *source.DomWindow();
  Window& target_window = *target.DomWindow();
  const url::Origin origin = window.GetSecurityOrigin();
  const url::Origin target_origin = target_window.GetSecurityOrigin();
  const auto refuse = [&](std::string_view reason) {
    PrintNavigationErrorMessage(window, target_window, reason);
    return false;
  };
  if (url.SchemeIs(url::kJavaScriptScheme) &&
      !origin.IsSameOriginWith(target_origin)) {
    return refuse(
        "The frame attempting navigation must be same-origin with the target "
        "if navigating to a javascript: url");
  }
  const Document& document = window.GetDocument();
  const bool is_top = &target == &source.Top();
  if (document.IsSandboxed(SandboxFlags::kNavigation)) {
    if (!is_top && !target.IsDescendantOf(source)) {
      return refuse(
          "The frame attempting navigation is sandboxed, and is therefore "
          "disallowed from navigating its ancestors.");
    }
    if (is_top) {
      if (!document.IsSandboxed(SandboxFlags::kTopNavigation)) {
        // A sandbox gives a document no more than its parent's has.
        return document.CanNavigateTop() ||
               refuse(
                   "The frame attempting to navigate the top-level window is "
                   "cross-origin and either it or one of its ancestors is not "
                   "allowed to navigate the top frame.\n");
      }
      return refuse(
          document.IsSandboxed(SandboxFlags::kTopNavigationByUserActivation)
              ? "The frame attempting navigation of the top-level window is "
                "sandboxed, but the flag of 'allow-top-navigation' or "
                "'allow-top-navigation-by-user-activation' is not set."
              : "The frame attempting navigation of the top-level window is "
                "sandboxed with the 'allow-top-navigation-by-user-activation' "
                "flag, but has no user activation (aka gesture). See "
                "https://www.chromestatus.com/feature/5629582019395584.");
    }
  }
  // A document navigates the frames under it, and more generally a frame that
  // it is of the origin of, or of an ancestor of it.
  if (CanAccessAncestor(origin, target)) {
    return true;
  }
  if (!is_top) {
    return refuse(
        "The frame attempting navigation is neither same-origin with the "
        "target, nor is it the target's parent or opener.");
  }
  // The top-level frame, which opened the document, or which one that the
  // document may navigate opened, or to its own site.
  if (source.Opener() == &target ||
      (target.Opener() && CanAccessAncestor(origin, *target.Opener())) ||
      target_origin.IsSameOriginWith(url)) {
    return true;
  }
  const std::string domain = DomainOf(target_origin.host());
  if (!domain.empty() && domain == DomainOf(url.host()) &&
      target_origin.scheme() == url.scheme()) {
    return true;
  }
  return refuse(
      "The frame attempting navigation is targeting its top-level window, but "
      "is neither same-origin with its target nor has it received a user "
      "gesture. See https://www.chromestatus.com/feature/5851021045661696.");
}

bool CanDisplay(Window& window, const GURL& url) {
  // The local scheme, and those that Chrome's renderer isolates.
  static constexpr auto kSchemes = std::to_array<std::string_view>(
      {"chrome", "chrome-distiller", "chrome-error", "chrome-native",
       "chrome-search", "devtools", "file", "googlechrome", "view-source"});
  const bool inner = url.SchemeIsBlob() || url.SchemeIsFileSystem();
  const url::Origin origin = url.SchemeIsBlob()
                                 ? BlobUrlRegistry::Get().OriginOf(url)
                                 : url::Origin::Create(url);
  if (inner ? origin.IsSameOriginWith(window.GetSecurityOrigin())
            : !std::ranges::contains(kSchemes, url.scheme())) {
    return true;
  }
  window.AddConsoleMessage(
      ConsoleLevel::kError,
      base::StrCat({"Not allowed to load local resource: ", ElidedUrl(url)}));
  return false;
}

Frame* FindFrameForNavigation(Frame& source,
                              std::u16string_view name,
                              const GURL& url) {
  if (Frame* const frame = FindFrame(source, name, url)) {
    return CanNavigate(source, *frame, url) ? frame : nullptr;
  }
  // CreateNewWindow().
  Window& window = *source.DomWindow();
  if (!window.GetDocument().PageDismissalEventBeingDispatched() &&
      CanDisplay(window, url) &&
      window.GetDocument().IsSandboxed(SandboxFlags::kPopups)) {
    window.AddConsoleMessage(
        ConsoleLevel::kError,
        base::StrCat({"Blocked opening '", ElidedUrl(url),
                      "' in a new window because the request was made in a "
                      "sandboxed frame whose 'allow-popups' permission is not "
                      "set."}));
  }
  return nullptr;
}

}  // namespace mitto
