// The link element, which preloads what rel=preload names through its
// document's ResourceFetcher, and the module script that rel=modulepreload
// names through its window's module map, as Blink's LinkLoader and
// PreloadHelper do, and loads the style sheet that rel=stylesheet names, as
// Blink's LinkStyle does, with its imports, holding back scripts, rendering
// or the parser as the sheet's place says, and fires load or error at the
// link once that ends. Icons, prefetch and the network hints are later work
// of #284.
//
// Upstream: third_party/blink/renderer/core/html/html_link_element.h
// Upstream: third_party/blink/renderer/core/html/html_link_element.cc
// Upstream: third_party/blink/renderer/core/html/link_rel_attribute.cc
// Upstream: third_party/blink/renderer/core/html/link_style.cc
// Upstream: third_party/blink/renderer/core/css/pending_sheet_type.cc
// Upstream: third_party/blink/renderer/core/loader/link_loader.cc
// Upstream: third_party/blink/renderer/core/loader/preload_helper.cc
// Upstream: third_party/blink/renderer/core/loader/pending_link_preload.cc

#ifndef MITTO_DOM_HTML_LINK_ELEMENT_H_
#define MITTO_DOM_HTML_LINK_ELEMENT_H_

#include <cstdint>
#include <optional>
#include <string>

#include "mitto/dom/document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/style_sheet_contents.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

// Blink's LinkRelAttribute: the link types of rel, in any case.
struct LinkRelAttribute {
  explicit LinkRelAttribute(std::u16string_view rel);

  bool is_style_sheet = false;
  bool is_alternate = false;
  bool is_icon = false;
  bool is_link_prefetch = false;
  bool is_dns_prefetch = false;
  bool is_preconnect = false;
  bool is_link_preload = false;
  bool is_module_preload = false;
  bool is_expect = false;
};

class HTMLLinkElement final : public HTMLElement,
                              public ResourceClient,
                              public StyleSheetOwner {
 public:
  static HTMLLinkElement* Create(Document& document);

  explicit HTMLLinkElement(Document& document);
  ~HTMLLinkElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: its token lists, of the link types and
  // the blocking tokens that Blink supports; sizes supports none.
  DOMTokenList* RelList() {
    return EnsureTokenList(u"rel", DOMTokenList::SupportedTokens::kRel);
  }
  DOMTokenList* Sizes() {
    return EnsureTokenList(u"sizes", DOMTokenList::SupportedTokens::kNone);
  }
  DOMTokenList* Blocking() {
    return EnsureTokenList(u"blocking",
                           DOMTokenList::SupportedTokens::kBlocking);
  }

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

  // ResourceClient:
  void NotifyFinished(Resource& resource) override;
  // StyleSheetOwner:
  void SheetLoaded(bool error) override;

  // The parser made it, which makes its style sheet block scripts.
  void SetCreatedByParser() { created_by_parser_ = true; }

 private:
  class ModulePreloadClient;

  // LinkStyle's disabled state: unset until the disabled attribute comes or
  // goes.
  enum class DisabledState { kUnset, kEnabledViaScript, kDisabled };

  // LinkStyle::LoadStylesheetIfNeeded(): fetches the style sheet that the
  // link names, or returns false if it names none.
  bool LoadStylesheetIfNeeded();
  // LinkStyle::NotifyFinished().
  void StyleSheetFinished(Resource& resource);
  // LinkStyle::SetDisabledState().
  void SetDisabledState(bool disabled);
  // Whether its sheet, or an import of it, still loads.
  bool StyleSheetIsLoading() const;
  // Lets go of its sheet, which tells it nothing more.
  void ClearSheet();
  // An alternate sheet that no script has enabled, which loads lazily and
  // holds nothing back.
  bool IsAlternate() const;
  // A blocking=render link, or a style sheet's that the parser made.
  bool IsPotentiallyRenderBlocking() const;
  // LinkStyle's pending sheet, which the document counts.
  void AddPendingSheet(Document::PendingSheetType type);
  void RemovePendingSheet();
  // HTMLLinkElement::ScheduleEvent(): load or error at the link from a task,
  // which holds the load event back.
  void ScheduleEvent(bool loaded);

  // HTMLLinkElement::Process(): loads what the link asks for, in place of
  // what it loaded before. A change of media changes nothing of a sheet that
  // has come.
  void Process(bool media_change = false);
  bool ShouldLoadLink() const;
  // LinkLoader::Abort(): lets go of the resource that it waits for, and of
  // an event that a task will fire for one that has ended.
  void Abort();
  // PreloadHelper::DnsPrefetchIfNeeded() and PreconnectIfNeeded(): the
  // hints that a link of rel dns-prefetch or preconnect gives the Session.
  void HintIfNeeded();
  // PreloadHelper::PreloadIfNeeded().
  Resource* PreloadIfNeeded();
  // PreloadHelper::ModulePreloadIfNeeded(): fetches the single module script
  // that rel=modulepreload names, which tells module_preload_ once it ends.
  void ModulePreloadIfNeeded();
  // LinkLoader::NotifyModuleLoadFinished(), from `client`.
  void ModulePreloadFinished(ModulePreloadClient& client, bool loaded);
  // HTMLLinkElement::LinkLoaded() and LinkLoadingErrored().
  void DispatchLinkEvent(bool loaded);

  cppgc::Member<Resource> pending_resource_;
  // What a module preload tells of its module script, until Abort() lets it
  // go, as Blink's PendingLinkPreload does.
  cppgc::Member<ModulePreloadClient> module_preload_;
  // The style sheet's fetch, and once it has come, the sheet with its
  // imports.
  cppgc::Member<Resource> style_resource_;
  cppgc::Member<StyleSheetContents> sheet_;
  Document::PendingSheetType pending_sheet_type_ =
      Document::PendingSheetType::kNone;
  DisabledState disabled_state_ = DisabledState::kUnset;
  // Whether a script has taken the disabled attribute away.
  bool explicitly_enabled_ = false;
  // Whether the sheet blocks rendering, which its imports' fetches take.
  bool render_blocking_ = false;
  // LinkStyle's loading_: from the sheet's fetch until it has parsed, so that
  // an import that ends during the parse tells the link nothing yet.
  bool loading_ = false;
  bool created_by_parser_ = false;
  // Whether Process() is adding the resource as a client, which hears of a
  // resource that has ended at once.
  bool adding_client_ = false;
  // How many times Abort() has run. A task that fires the event of a
  // resource that had ended fires nothing once it differs, as Blink's
  // PendingLinkPreload::FinishObserver, disposed, hears nothing.
  uint64_t load_generation_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_LINK_ELEMENT_H_
