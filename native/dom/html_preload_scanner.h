// Chrome's preload scanner: a second tokenizer that reads the document's
// markup ahead of the parser, for the subresources that the elements to come
// will ask for, and fetches them through the document's ResourceFetcher, so
// that the requests go out in Chrome's order and at Chrome's priorities. The
// parser's elements take the resources it fetched. It scans the first part of
// the markup, and the rest each time the parser waits for a script or a
// style sheet, as Blink's main-thread scanner does with
// ThreadedPreloadScanner off, as Chrome 157 ships it; and what a write gives
// while the parser waits, as its insertion scanner does.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_preload_scanner.cc
// Upstream: third_party/blink/renderer/core/html/parser/css_preload_scanner.cc
// Upstream: third_party/blink/renderer/core/html/parser/preload_request.cc
// Upstream: third_party/blink/renderer/core/html/html_dimension.cc

#ifndef MITTO_DOM_HTML_PRELOAD_SCANNER_H_
#define MITTO_DOM_HTML_PRELOAD_SCANNER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/subresource_integrity.h"
#include "mitto/html/html_token.h"
#include "mitto/html/html_tokenizer.h"
#include "url/gurl.h"

namespace mitto {

class Document;

// ParseDimensionValue(): the rules for parsing dimension values, with
// Blink's deviations, as `value` and whether it is absolute: neither a
// percentage nor followed by `*`.
struct HtmlDimension {
  double value = 0;
  bool absolute = true;
};
std::optional<HtmlDimension> ParseDimensionValue(std::u16string_view input);

// Blink's PreloadRequest: what the scanner fetches, and how.
struct PreloadRequest {
  enum class RequestType { kPreload, kPreconnect, kLinkRelPreload };

  PreloadRequest();
  PreloadRequest(const PreloadRequest&);
  PreloadRequest(PreloadRequest&&);
  PreloadRequest& operator=(const PreloadRequest&);
  PreloadRequest& operator=(PreloadRequest&&);
  ~PreloadRequest();

  // Start(): fetches it through `document`'s ResourceFetcher, where the
  // element that asks for it later takes it.
  void Start(Document& document) const;

  // As the markup says it, resolved against `base_url`, or the document's
  // URL if that is empty.
  std::u16string resource_url;
  GURL base_url;
  ResourceType resource_type = ResourceType::kImage;
  RequestType request_type = RequestType::kPreload;
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  std::optional<float> resource_width;
  std::optional<float> resource_height;
  CrossOriginAttribute cross_origin = CrossOriginAttribute::kNotSet;
  FetchPriorityHint priority_hint = FetchPriorityHint::kAuto;
  // Its charset attribute, for a script or a style sheet.
  std::u16string charset;
  // FetchParameters::kLazyLoad, as an async or deferred script has.
  bool lazy = false;
  // RenderBlockingBehavior::kBlocking.
  bool render_blocking = false;
  IntegrityMetadataSet integrity;
  bool from_insertion_scanner = false;
};

// Blink's CSSPreloadScanner: the @import rules at the start of a style
// element's text, by a scan of its characters rather than a parse.
class CssPreloadScanner {
 public:
  CssPreloadScanner();
  CssPreloadScanner(const CssPreloadScanner&) = delete;
  CssPreloadScanner& operator=(const CssPreloadScanner&) = delete;
  ~CssPreloadScanner();

  void Reset();
  void Scan(std::u16string_view data,
            const GURL& predicted_base_element_url,
            std::vector<PreloadRequest>& requests);
  void SetReferrerPolicy(FetchReferrerPolicy policy) {
    referrer_policy_ = policy;
  }
  // Whether the style element comes after the body, or an img, starts.
  void SetInBody(bool in_body) { in_body_ = in_body; }
  void SetMediaMatches(bool matches) { media_matches_ = matches; }

 private:
  enum class State {
    kInitial,
    kMaybeComment,
    kComment,
    kMaybeCommentEnd,
    kRuleStart,
    kRule,
    kAfterRule,
    kRuleValue,
    kAfterRuleValue,
    kMaybeLayerValue,
    kAfterMaybeLayerValue,
    kDoneParsingImportRules,
  };

  void Tokenize(char16_t c);
  bool HasFinishedRuleValue() const;
  bool CanPreloadImportRule() const;
  void EmitRule();

  State state_ = State::kInitial;
  std::u16string rule_;
  std::u16string rule_value_;
  std::u16string maybe_layer_value_;
  bool has_trailing_contents_ = false;
  bool in_body_ = false;
  bool media_matches_ = true;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
  // While Scan() runs.
  raw_ptr<std::vector<PreloadRequest>> requests_ = nullptr;
  raw_ptr<const GURL> predicted_base_element_url_ = nullptr;
};

// Blink's TokenPreloadScanner: the requests that each token of the markup
// makes.
class TokenPreloadScanner {
 public:
  enum class ScannerType { kMainDocument, kInsertion };

  // For `document`, whose referrer policy, media values and pixel ratio it
  // takes now, as Blink's CachedDocumentParameters and MediaValuesCached
  // keep them.
  TokenPreloadScanner(Document& document, ScannerType scanner_type);
  TokenPreloadScanner(const TokenPreloadScanner&) = delete;
  TokenPreloadScanner& operator=(const TokenPreloadScanner&) = delete;
  ~TokenPreloadScanner();

  // Adds the requests of `token`, and counts a CSP meta element.
  void Scan(const HtmlToken& token,
            std::vector<PreloadRequest>& requests,
            int& csp_meta_tag_count);
  void SetPredictedBaseElementUrl(const GURL& url) {
    predicted_base_element_url_ = url;
  }

 private:
  class StartTagScanner;
  enum class ForeignContentType { kSvg, kMath };

  // The source that a picture's first matching source element chose.
  struct PictureData {
    std::u16string source_url;
    float source_size = 0;
    bool source_size_set = false;
    bool picked = false;
  };

  void UpdatePredictedBaseUrl(const HtmlToken& token);

  const GURL document_url_;
  const ScannerType scanner_type_;
  FetchReferrerPolicy referrer_policy_;
  MediaValues media_values_;
  MediaQueryFeatures features_;
  float device_pixel_ratio_;
  bool in_style_ = false;
  bool in_picture_ = false;
  bool seen_body_ = false;
  bool seen_img_ = false;
  int template_count_ = 0;
  int dsd_count_ = 0;
  std::vector<ForeignContentType> foreign_content_stack_;
  PictureData picture_data_;
  GURL predicted_base_element_url_;
  CssPreloadScanner css_scanner_;
};

// Blink's HTMLPreloadScanner: its own tokenizer over the markup that it is
// given, and the requests of its tokens.
class HtmlPreloadScanner {
 public:
  HtmlPreloadScanner(Document& document,
                     TokenPreloadScanner::ScannerType scanner_type);
  HtmlPreloadScanner(const HtmlPreloadScanner&) = delete;
  HtmlPreloadScanner& operator=(const HtmlPreloadScanner&) = delete;
  ~HtmlPreloadScanner();

  void AppendToEnd(std::u16string_view source);

  // What Scan() found in what it was given since the last scan.
  struct Result {
    Result();
    Result(Result&&);
    Result& operator=(Result&&);
    ~Result();

    std::vector<PreloadRequest> requests;
    int csp_meta_tag_count = 0;
  };
  // Scan(): the requests of the markup it has, with `starting_base_url`,
  // the document's base element's URL if it has one, as the predicted base.
  // A CSP meta element ends the scan, and the scanner forgets the rest.
  Result Scan(const GURL& starting_base_url);

 private:
  std::unique_ptr<HtmlTokenizer> tokenizer_;
  TokenPreloadScanner scanner_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_PRELOAD_SCANNER_H_
