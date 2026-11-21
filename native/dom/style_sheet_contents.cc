#include "mitto/dom/style_sheet_contents.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/css/css_selector_parser.h"
#include "mitto/css/style_sheet_imports.h"
#include "mitto/dom/document.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/header_rules.h"

namespace mitto {
namespace {

// EqualIgnoringFragmentIdentifier().
bool EqualIgnoringFragment(const GURL& a, const GURL& b) {
  GURL::Replacements no_fragment;
  no_fragment.ClearRef();
  return a.is_valid() && b.is_valid() &&
         a.ReplaceComponents(no_fragment) == b.ReplaceComponents(no_fragment);
}

}  // namespace

bool IsLaxMimeTypeCheck(const Document& document, const Resource& resource) {
  // ResourceResponse::IsCorsSameOrigin(): a basic or cors response, not an
  // opaque one.
  return document.InQuirksMode() && !resource.Response().opaque;
}

FetchReferrerPolicy StyleSheetReferrerPolicy(const Resource& resource) {
  return FetchReferrerPolicyFromHeaderValue(
      HeaderField(resource.Response().headers, "referrer-policy"));
}

DecodedStyleSheet DecodeStyleSheet(const Resource& resource, bool lax) {
  // CSSStyleSheetResource's decoder: the request's charset hint, the
  // response's charset over it, and @charset or a byte order mark over both.
  TextResourceDecoder decoder(TextResourceDecoderOptions(
      TextResourceDecoderOptions::ContentType::kCss, resource.Charset()));
  const std::string charset =
      GetMimeTypeAndCharset(
          HeaderField(resource.Response().headers, "content-type"))
          .charset;
  if (!charset.empty()) {
    decoder.SetEncoding(
        CanonicalTextEncodingName(charset),
        TextResourceDecoder::EncodingSource::kEncodingFromHttpHeader);
  }
  DecodedStyleSheet sheet;
  if (resource.ErrorOccurred()) {
    sheet.encoding = decoder.encoding();
    return sheet;
  }
  // CanUseSheet(): Content-Type as it came, before any sniffing.
  if (!lax) {
    const std::string content_type =
        GetMimeTypeAndCharset(
            HeaderField(resource.Response().headers, "content-type"))
            .mime_type;
    if (!content_type.empty() && content_type != "text/css" &&
        content_type != "application/x-unknown-content-type") {
      sheet.encoding = decoder.encoding();
      return sheet;
    }
  }
  sheet.text = decoder.Decode(base::as_byte_span(resource.Body()));
  sheet.text += decoder.Flush();
  sheet.encoding = decoder.encoding();
  return sheet;
}

StyleSheetContents::StyleSheetContents(Document& document,
                                       GURL base_url,
                                       GURL original_url,
                                       StyleSheetReferrer referrer,
                                       std::string charset,
                                       StyleSheetOwner* owner,
                                       StyleRuleImport* owner_rule)
    : document_(&document),
      base_url_(std::move(base_url)),
      original_url_(std::move(original_url)),
      referrer_(std::move(referrer)),
      charset_(std::move(charset)),
      owner_(owner),
      owner_rule_(owner_rule) {}

StyleSheetContents::~StyleSheetContents() = default;

void StyleSheetContents::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  visitor->Trace(owner_);
  visitor->Trace(owner_rule_);
  visitor->Trace(imports_);
}

StyleSheetContents* StyleSheetContents::ParentStyleSheet() const {
  const StyleRuleImport* const rule = owner_rule_.Get();
  return rule ? rule->ParentStyleSheet() : nullptr;
}

void StyleSheetContents::ParseAuthorStyleSheet(std::u16string_view text) {
  Document* const document = document_.Get();
  if (!document) {
    return;
  }
  CssSelectorParserContext context;
  context.html_document = document->IsHTMLDocument();
  context.quirks_mode = document->InQuirksMode();
  for (StyleSheetImport& import : FindStyleSheetImports(text, context)) {
    imports_.push_back(bindings::MakeGarbageCollected<StyleRuleImport>(
        *this, std::move(import.url)));
  }
  // Each fetch starts once the sheet holds every rule, as Blink requests an
  // import as its rule is set in the sheet, in order.
  for (StyleRuleImport* import : imports_.ToVector()) {
    import->RequestStyleSheet();
  }
}

bool StyleSheetContents::IsLoading() const {
  for (const StyleRuleImport* import : imports_.ToVector()) {
    if (import->IsLoading()) {
      return true;
    }
  }
  return false;
}

void StyleSheetContents::CheckLoaded() {
  if (IsLoading()) {
    return;
  }
  if (StyleSheetContents* const parent = ParentStyleSheet()) {
    parent->CheckLoaded();
    return;
  }
  if (StyleSheetOwner* const owner = owner_.Get()) {
    owner->SheetLoaded(did_load_error_occur_);
  }
}

StyleRuleImport::StyleRuleImport(StyleSheetContents& parent,
                                 std::u16string href)
    : parent_(&parent), href_(std::move(href)) {}

StyleRuleImport::~StyleRuleImport() = default;

void StyleRuleImport::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(parent_);
  visitor->Trace(resource_);
  visitor->Trace(style_sheet_);
}

bool StyleRuleImport::IsLoading() const {
  return loading_ || (style_sheet_ && style_sheet_->IsLoading());
}

void StyleRuleImport::RequestStyleSheet() {
  StyleSheetContents* const parent = parent_.Get();
  Document* const document = parent ? parent->GetDocument() : nullptr;
  if (!document) {
    return;
  }
  const std::u16string_view href = StripHTMLSpaces(href_);
  const GURL url = parent->BaseUrl().is_valid()
                       ? parent->BaseUrl().Resolve(href)
                       : document->CompleteUrl(href);
  // A sheet that imports itself, or one above it, imports nothing.
  const StyleSheetContents* root = parent;
  for (const StyleSheetContents* sheet = parent; sheet;
       sheet = sheet->ParentStyleSheet()) {
    if (EqualIgnoringFragment(url, sheet->BaseUrl()) ||
        EqualIgnoringFragment(url, sheet->OriginalUrl())) {
      return;
    }
    root = sheet;
  }
  FetchParameters params;
  params.url = url;
  params.type = ResourceType::kCSSStyleSheet;
  params.referrer_policy = parent->Referrer().policy;
  // The sheet's URL is the referrer, which the Session takes of the
  // document's origin, or as the final URL of a response that the document
  // has had whole, as a sheet's of another origin is.
  const GURL referrer(parent->Referrer().referrer);
  const Window* const window = document->DefaultView();
  const Fetcher* const fetcher = window ? window->GetFetcher() : nullptr;
  if (referrer.is_valid() &&
      (document->GetOrigin().IsSameOriginWith(referrer) ||
       (fetcher && fetcher->HasFinishedResponse(referrer.spec())))) {
    params.referrer = referrer.spec();
  }
  params.charset = parent->Charset();
  params.render_blocking = root->RenderBlocking();
  Resource* const resource = document->Fetcher().RequestResource(params);
  if (!resource) {
    return;
  }
  resource_ = resource;
  loading_ = true;
  resource->AddClient(this);
}

void StyleRuleImport::NotifyFinished(Resource& resource) {
  if (&resource != resource_.Get() || !loading_) {
    return;
  }
  resource.RemoveClient(this);
  resource_ = nullptr;
  StyleSheetContents* const parent = parent_.Get();
  Document* const document = parent ? parent->GetDocument() : nullptr;
  if (document && !resource.ErrorOccurred()) {
    // The imported sheet's imports resolve against its response's URL and go
    // with it as referrer, under the policy that its response names. A sheet
    // of the wrong type is empty.
    const GURL response_url(resource.Response().url);
    StyleSheetReferrer referrer;
    referrer.referrer = response_url.spec();
    referrer.policy = StyleSheetReferrerPolicy(resource);
    DecodedStyleSheet decoded =
        DecodeStyleSheet(resource, IsLaxMimeTypeCheck(*document, resource));
    style_sheet_ = bindings::MakeGarbageCollected<StyleSheetContents>(
        *document, response_url, resource.Url(), std::move(referrer),
        std::move(decoded.encoding), nullptr, this);
    style_sheet_->ParseAuthorStyleSheet(decoded.text);
  }
  loading_ = false;
  if (parent) {
    // StyleSheetContents::NotifyLoadedSheet().
    if (resource.ErrorOccurred()) {
      parent->SetLoadErrorOccurred();
    }
    parent->CheckLoaded();
  }
}

}  // namespace mitto
