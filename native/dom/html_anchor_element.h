// The a element, a hyperlink, and the base of it and the area element, which
// share the URL's parts of their href, and follow it when a click activates
// them, as Blink's HTMLAnchorElementBase::HandleClick() does.
//
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.h
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.cc

#ifndef MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
#define MITTO_DOM_HTML_ANCHOR_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"
#include "mitto/dom/url_utils.h"

class GURL;

namespace mitto {

class ResolvedURL;

// Blink's HTMLAnchorElementBase with its UrlUtils: each part of the URL that
// href resolves to, and each setter, which changes that part and writes the
// URL back into href, as Blink's do.
class HTMLAnchorElementBase : public HTMLElement, public UrlUtils {
 public:
  // The members of HTMLHyperlinkElementUtils that UrlUtils does not have.
  void SetHref(const std::u16string& value);
  // relList, of the link types that Blink's RelList supports.
  DOMTokenList* RelList() {
    return EnsureTokenList(u"rel", DOMTokenList::SupportedTokens::kRel);
  }

  // HTMLElement's: a link with href is draggable by default, and focusable.
  bool Draggable() const override;
  int32_t DefaultTabIndex() const override;
  // Node's: a link has activation behaviour, as Blink's has for one with
  // href, which a click that nothing canceled or handled runs before
  // DOMActivate: it follows the link.
  bool HasActivationBehavior() const override;
  void RunActivationBehaviorBeforeDOMActivate(
      Event& event,
      EventDispatchHandlingState* state) override;

 protected:
  HTMLAnchorElementBase(Document& document, const std::u16string& local_name);

 private:
  // UrlUtils's: the URL that href resolves to, null without href or for one
  // that does not resolve, Blink's Url(); SetUrl() writes a URL into href, and
  // Input() is the attribute itself.
  ResolvedURL Url() const override;
  void SetUrl(const ResolvedURL& url) override;
  std::u16string Input() const override;
  // HandleClick(): follows the link that a click activated.
  void HandleClick(Event& event);
  // NavigateToHyperlink(): the frame that the link's target names navigates
  // to `url`. A new window, which only the user's activation opens, does not
  // open.
  void NavigateToHyperlink(const GURL& url);
};

class HTMLAnchorElement final : public HTMLAnchorElementBase {
 public:
  static HTMLAnchorElement* Create(Document& document);

  explicit HTMLAnchorElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
