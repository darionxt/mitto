// What a Page keeps of a style sheet until M9 applies style: its @import
// rules, each fetched through the document's ResourceFetcher with the sheet's
// URL as referrer, as Blink's StyleRuleImport fetches one, and its imported
// sheets, which import in turn. A sheet is loading until each of its imports
// has loaded, and its owner, a link or a style element, hears of it then.
//
// Upstream: third_party/blink/renderer/core/css/style_sheet_contents.cc
// Upstream: third_party/blink/renderer/core/css/style_rule_import.cc

#ifndef MITTO_DOM_STYLE_SHEET_CONTENTS_H_
#define MITTO_DOM_STYLE_SHEET_CONTENTS_H_

#include <string>
#include <string_view>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/resource.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class StyleRuleImport;

// What owns a top-level sheet: a link's or a style element's.
class StyleSheetOwner : public cppgc::GarbageCollectedMixin {
 public:
  // Node::NotifyLoadedSheetAndAllCriticalSubresources(): the sheet and its
  // imports have all loaded, `error` if an import of the sheet failed.
  virtual void SheetLoaded(bool error) = 0;
};

// CSSParserContext's referrer, which the sheet's imports go with.
struct StyleSheetReferrer {
  // kClientReferrer for the document's, or the URL of a fetched sheet.
  std::string referrer = kClientReferrer;
  // The policy that fetched the sheet, or kDefault for the document's.
  FetchReferrerPolicy policy = FetchReferrerPolicy::kDefault;
};

class StyleSheetContents final
    : public cppgc::GarbageCollected<StyleSheetContents> {
 public:
  // A sheet of `owner`, a top-level one, or of `owner_rule`, an imported
  // one. Its imports resolve against `base_url`, null for the document's.
  StyleSheetContents(Document& document,
                     GURL base_url,
                     GURL original_url,
                     StyleSheetReferrer referrer,
                     std::string charset,
                     StyleSheetOwner* owner,
                     StyleRuleImport* owner_rule);
  StyleSheetContents(const StyleSheetContents&) = delete;
  StyleSheetContents& operator=(const StyleSheetContents&) = delete;
  ~StyleSheetContents();

  void Trace(cppgc::Visitor* visitor) const;

  const GURL& BaseUrl() const { return base_url_; }
  const GURL& OriginalUrl() const { return original_url_; }
  StyleSheetContents* ParentStyleSheet() const;
  Document* GetDocument() const { return document_.Get(); }
  const StyleSheetReferrer& Referrer() const { return referrer_; }
  const std::string& Charset() const { return charset_; }

  // ParseAuthorStyleSheet(): reads the sheet's text and fetches its imports.
  void ParseAuthorStyleSheet(std::u16string_view text);
  // Whether an import of it, or of what it imports, still loads.
  bool IsLoading() const;
  // CheckLoaded(): tells its parent, or its owner, once nothing loads.
  void CheckLoaded();
  // NotifyLoadedSheet(): an import of it failed, which its owner hears of.
  void SetLoadErrorOccurred() { did_load_error_occur_ = true; }
  // Lets go of its owner, which hears of nothing more.
  void ClearOwner() { owner_ = nullptr; }
  // Whether the top-level sheet blocks rendering, which boosts the fetch of
  // each import under it, as SetRenderBlocking() has it.
  bool RenderBlocking() const { return render_blocking_; }
  void SetRenderBlocking(bool render_blocking) {
    render_blocking_ = render_blocking;
  }

 private:
  cppgc::WeakMember<Document> document_;
  const GURL base_url_;
  const GURL original_url_;
  const StyleSheetReferrer referrer_;
  const std::string charset_;
  cppgc::WeakMember<StyleSheetOwner> owner_;
  cppgc::WeakMember<StyleRuleImport> owner_rule_;
  HeapVector<StyleRuleImport> imports_;
  bool did_load_error_occur_ = false;
  bool render_blocking_ = false;
};

// An @import rule, and the sheet that it fetched.
class StyleRuleImport final : public cppgc::GarbageCollected<StyleRuleImport>,
                              public ResourceClient {
 public:
  StyleRuleImport(StyleSheetContents& parent, std::u16string href);
  ~StyleRuleImport();

  void Trace(cppgc::Visitor* visitor) const override;

  StyleSheetContents* ParentStyleSheet() const { return parent_.Get(); }
  // RequestStyleSheet().
  void RequestStyleSheet();
  bool IsLoading() const;

  // ResourceClient:
  void NotifyFinished(Resource& resource) override;

 private:
  cppgc::WeakMember<StyleSheetContents> parent_;
  const std::u16string href_;
  cppgc::Member<Resource> resource_;
  cppgc::Member<StyleSheetContents> style_sheet_;
  bool loading_ = false;
};

// A fetched style sheet's text, and the encoding that decoded it, which its
// imports ask for, as Blink gives a sheet's parser context
// CSSStyleSheetResource::Encoding().
struct DecodedStyleSheet {
  std::u16string text;
  std::string encoding;
};

// CSSStyleSheetResource::SheetText(): the text of a fetched style sheet,
// decoded by a byte order mark, else its response's charset, else @charset,
// else the charset hint of the request that made it; or nothing for one that
// failed, or whose Content-Type is not text/css, none or
// application/x-unknown-content-type, unless `lax`, for a sheet of the
// document's origin in quirks mode.
DecodedStyleSheet DecodeStyleSheet(const Resource& resource, bool lax);

// Whether a sheet's text is held to its MIME type laxly: one that is not
// opaque, in a document in quirks mode.
bool IsLaxMimeTypeCheck(const Document& document, const Resource& resource);

}  // namespace mitto

#endif  // MITTO_DOM_STYLE_SHEET_CONTENTS_H_
