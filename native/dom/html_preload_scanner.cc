#include "mitto/dom/html_preload_scanner.h"

#include <utility>

#include "base/memory/raw_ref.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/css/media_query_evaluator.h"
#include "mitto/css/sizes_attribute_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_write_intervention.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_image_element.h"
#include "mitto/dom/html_link_element.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/html_srcset_parser.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/url_with_encoding.h"
#include "mitto/mime_types.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

bool IsHtmlSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool IsVoidElement(std::u16string_view tag) {
  return tag == u"area" || tag == u"base" || tag == u"br" || tag == u"col" ||
         tag == u"embed" || tag == u"hr" || tag == u"img" || tag == u"input" ||
         tag == u"link" || tag == u"meta" || tag == u"source" ||
         tag == u"track" || tag == u"wbr";
}

const std::u16string* TokenAttribute(const HtmlToken& token,
                                     std::u16string_view name) {
  for (const HtmlAttribute& attribute : token.attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

// ContentType::GetType(): a MIME type without its parameters.
std::string ContentTypeType(std::u16string_view value) {
  if (const size_t semi = value.find(';'); semi != std::u16string_view::npos) {
    value = value.substr(0, semi);
  }
  return base::UTF16ToUTF8(base::TrimWhitespace(value, base::TRIM_ALL));
}

// HTMLTokenizer::UpdateStateFor(), with scripting on: the state that the
// tree builder would set after a start tag.
std::optional<HtmlTokenizer::State> StateForTag(std::u16string_view name) {
  if (name == u"textarea" || name == u"title") {
    return HtmlTokenizer::State::kRcdata;
  }
  if (name == u"plaintext") {
    return HtmlTokenizer::State::kPlaintext;
  }
  if (name == u"script") {
    return HtmlTokenizer::State::kScriptData;
  }
  if (name == u"style" || name == u"iframe" || name == u"xmp" ||
      name == u"noembed" || name == u"noframes" || name == u"noscript") {
    return HtmlTokenizer::State::kRawtext;
  }
  return std::nullopt;
}

// ParseCSSStringOrURL().
std::u16string ParseCssStringOrUrl(std::u16string_view string) {
  auto trim = [](std::u16string_view& view) {
    while (!view.empty() && IsHtmlSpace(view.front())) {
      view.remove_prefix(1);
    }
    while (!view.empty() && IsHtmlSpace(view.back())) {
      view.remove_suffix(1);
    }
  };
  trim(string);
  if (string.size() >= 5 && base::ToLowerASCII(string[0]) == 'u' &&
      base::ToLowerASCII(string[1]) == 'r' &&
      base::ToLowerASCII(string[2]) == 'l' && string[3] == '(' &&
      string.back() == ')') {
    string = string.substr(4, string.size() - 5);
  }
  trim(string);
  if (string.size() >= 2 && string.front() == string.back() &&
      (string.front() == '\'' || string.front() == '"')) {
    string = string.substr(1, string.size() - 2);
  }
  return std::u16string(string);
}

// PreloadRequest::CreateIfNeeded(): no request for an empty URL, one of a
// fragment alone, or a data: URL, but for a link preload's.
std::optional<PreloadRequest> CreatePreloadRequestIfNeeded(
    std::u16string_view resource_url,
    const GURL& base_url,
    ResourceType type,
    FetchReferrerPolicy referrer_policy,
    PreloadRequest::RequestType request_type) {
  if (resource_url.empty() || resource_url.front() == '#' ||
      (base::StartsWith(resource_url, u"data:",
                        base::CompareCase::INSENSITIVE_ASCII) &&
       request_type != PreloadRequest::RequestType::kLinkRelPreload)) {
    return std::nullopt;
  }
  PreloadRequest request;
  request.resource_url = std::u16string(resource_url);
  request.base_url = base_url;
  request.resource_type = type;
  request.referrer_policy = referrer_policy;
  request.request_type = request_type;
  return request;
}

}  // namespace

std::optional<HtmlDimension> ParseDimensionValue(std::u16string_view input) {
  size_t current = 0;
  while (current < input.size() && IsHtmlSpace(input[current])) {
    ++current;
  }
  const size_t number_start = current;
  if (current >= input.size() || !base::IsAsciiDigit(input[current])) {
    return std::nullopt;
  }
  while (current < input.size() && base::IsAsciiDigit(input[current])) {
    ++current;
  }
  if (current < input.size() && input[current] == '.') {
    ++current;
    while (current < input.size() && base::IsAsciiDigit(input[current])) {
      ++current;
    }
  }
  double value = 0;
  if (!base::StringToDouble(
          base::UTF16ToUTF8(input.substr(number_start, current - number_start)),
          &value)) {
    // "1." does not convert, though Blink's CharactersToDouble() takes it.
    if (!base::StringToDouble(base::UTF16ToUTF8(input.substr(
                                  number_start, current - number_start - 1)),
                              &value)) {
      return std::nullopt;
    }
  }
  HtmlDimension dimension;
  dimension.value = value;
  dimension.absolute = current >= input.size() ||
                       (input[current] != '%' && input[current] != '*');
  return dimension;
}

PreloadRequest::PreloadRequest() = default;
PreloadRequest::PreloadRequest(const PreloadRequest&) = default;
PreloadRequest::PreloadRequest(PreloadRequest&&) = default;
PreloadRequest& PreloadRequest::operator=(const PreloadRequest&) = default;
PreloadRequest& PreloadRequest::operator=(PreloadRequest&&) = default;
PreloadRequest::~PreloadRequest() = default;

void PreloadRequest::Start(Document& document) const {
  Window* const window = document.DefaultView();
  if (!window) {
    return;
  }
  const std::u16string_view stripped = StripHTMLSpaces(resource_url);
  // HTMLResourcePreloader::Preload(): PreconnectHost(), a connection for
  // requests with credentials unless crossorigin says anonymous. It resolves
  // the URL against the base element's alone, so with none only an absolute
  // URL connects; the link connects when the parser inserts it.
  if (request_type == RequestType::kPreconnect) {
    const GURL url = base_url.is_valid() ? base_url.Resolve(stripped)
                                         : GURL(base::UTF16ToUTF8(stripped));
    if (Fetcher* const fetcher = window->GetFetcher();
        fetcher && url.is_valid() && url.SchemeIsHTTPOrHTTPS()) {
      fetcher->Preconnect(url,
                          cross_origin != CrossOriginAttribute::kAnonymous);
    }
    return;
  }
  // PreloadRequest::CompleteURL(): against the predicted base, with the
  // document's encoding for the query, as Document::CompleteURLWithOverride()
  // resolves it.
  const GURL url =
      base_url.is_valid()
          ? ResolveUrlWithEncoding(base_url, stripped, document.Encoding())
          : document.CompleteUrl(stripped);
  if (!url.is_valid()) {
    return;
  }
  FetchParameters params;
  params.url = url;
  params.type = resource_type;
  params.referrer_policy = referrer_policy;
  params.priority_hint = priority_hint;
  switch (cross_origin) {
    case CrossOriginAttribute::kNotSet:
      break;
    case CrossOriginAttribute::kAnonymous:
      params.mode = FetchMode::kCors;
      params.credentials = FetchCredentialsMode::kSameOrigin;
      break;
    case CrossOriginAttribute::kUseCredentials:
      params.mode = FetchMode::kCors;
      params.credentials = FetchCredentialsMode::kInclude;
      break;
  }
  params.lazy = lazy;
  params.resource_width = resource_width;
  params.resource_height = resource_height;
  params.integrity = integrity;
  params.is_link_preload = request_type == RequestType::kLinkRelPreload;
  if (resource_type == ResourceType::kScript ||
      resource_type == ResourceType::kCSSStyleSheet) {
    const std::string named = CanonicalTextEncodingName(charset);
    params.charset = named.empty() ? document.Encoding() : named;
  }
  params.speculative_preload_type = from_insertion_scanner
                                        ? SpeculativePreloadType::kInserted
                                        : SpeculativePreloadType::kInDocument;
  if (resource_type == ResourceType::kScript) {
    MaybeDisallowFetchForDocWrittenScript(url, lazy, document);
  }
  params.render_blocking = render_blocking;
  document.Fetcher().RequestResource(params);
}

CssPreloadScanner::CssPreloadScanner() = default;

CssPreloadScanner::~CssPreloadScanner() = default;

void CssPreloadScanner::Reset() {
  state_ = State::kInitial;
  rule_.clear();
  rule_value_.clear();
  maybe_layer_value_.clear();
  has_trailing_contents_ = false;
  in_body_ = false;
  media_matches_ = true;
}

void CssPreloadScanner::Scan(std::u16string_view data,
                             const GURL& predicted_base_element_url,
                             std::vector<PreloadRequest>& requests) {
  requests_ = &requests;
  predicted_base_element_url_ = &predicted_base_element_url;
  for (const char16_t c : data) {
    if (state_ == State::kDoneParsingImportRules) {
      break;
    }
    Tokenize(c);
  }
  if (state_ == State::kRuleValue || state_ == State::kAfterRuleValue ||
      state_ == State::kAfterMaybeLayerValue) {
    EmitRule();
  }
  requests_ = nullptr;
  predicted_base_element_url_ = nullptr;
}

void CssPreloadScanner::Tokenize(char16_t c) {
  switch (state_) {
    case State::kInitial:
      if (IsHtmlSpace(c)) {
        break;
      }
      if (c == '@') {
        state_ = State::kRuleStart;
      } else if (c == '/') {
        state_ = State::kMaybeComment;
      } else {
        state_ = State::kDoneParsingImportRules;
      }
      break;
    case State::kMaybeComment:
      state_ = c == '*' ? State::kComment : State::kInitial;
      break;
    case State::kComment:
      if (c == '*') {
        state_ = State::kMaybeCommentEnd;
      }
      break;
    case State::kMaybeCommentEnd:
      if (c == '*') {
        break;
      }
      state_ = c == '/' ? State::kInitial : State::kComment;
      break;
    case State::kRuleStart:
      if (base::IsAsciiAlpha(c)) {
        rule_.clear();
        rule_value_.clear();
        rule_.push_back(c);
        state_ = State::kRule;
      } else {
        state_ = State::kInitial;
      }
      break;
    case State::kRule:
      if (IsHtmlSpace(c)) {
        state_ = State::kAfterRule;
      } else if (c == ';') {
        state_ = State::kInitial;
      } else {
        rule_.push_back(c);
      }
      break;
    case State::kAfterRule:
      if (IsHtmlSpace(c)) {
        break;
      }
      if (c == ';') {
        state_ = State::kInitial;
      } else if (c == '{') {
        state_ = State::kDoneParsingImportRules;
      } else {
        state_ = State::kRuleValue;
        rule_value_.push_back(c);
      }
      break;
    case State::kRuleValue:
      if (IsHtmlSpace(c)) {
        state_ = State::kAfterRuleValue;
        has_trailing_contents_ = false;
      } else if (c == ';') {
        if (HasFinishedRuleValue()) {
          EmitRule();
        } else {
          rule_value_.push_back(c);
        }
      } else {
        rule_value_.push_back(c);
      }
      break;
    case State::kAfterRuleValue:
      if (IsHtmlSpace(c)) {
        break;
      }
      if (c == ';') {
        EmitRule();
      } else if (c == '{') {
        state_ = State::kDoneParsingImportRules;
      } else {
        state_ = State::kMaybeLayerValue;
        maybe_layer_value_.clear();
        maybe_layer_value_.push_back(c);
      }
      break;
    case State::kMaybeLayerValue:
      if (IsHtmlSpace(c)) {
        state_ = State::kAfterMaybeLayerValue;
        has_trailing_contents_ = false;
      } else if (c == ';') {
        EmitRule();
      } else {
        maybe_layer_value_.push_back(c);
      }
      break;
    case State::kAfterMaybeLayerValue:
      if (IsHtmlSpace(c)) {
        break;
      }
      if (c == ';') {
        EmitRule();
      } else if (c == '{') {
        state_ = State::kDoneParsingImportRules;
      } else {
        has_trailing_contents_ = true;
      }
      break;
    case State::kDoneParsingImportRules:
      break;
  }
}

bool CssPreloadScanner::HasFinishedRuleValue() const {
  if (!base::EqualsCaseInsensitiveASCII(rule_, u"import")) {
    return true;
  }
  if (rule_value_.size() < 2 || rule_value_[rule_value_.size() - 2] == '\\') {
    return false;
  }
  if (rule_value_.front() == '\'' || rule_value_.front() == '"') {
    return rule_value_.front() == rule_value_.back();
  }
  return rule_value_.back() == ')';
}

bool CssPreloadScanner::CanPreloadImportRule() const {
  // Media conditions after the URL load nothing ahead.
  if (has_trailing_contents_) {
    return false;
  }
  if (maybe_layer_value_.empty() ||
      base::EqualsCaseInsensitiveASCII(maybe_layer_value_, u"layer")) {
    return true;
  }
  return maybe_layer_value_.size() >= 8 &&
         base::EqualsCaseInsensitiveASCII(
             std::u16string_view(maybe_layer_value_).substr(0, 6), u"layer(") &&
         maybe_layer_value_.back() == ')';
}

void CssPreloadScanner::EmitRule() {
  if (base::EqualsCaseInsensitiveASCII(rule_, u"import")) {
    if (CanPreloadImportRule()) {
      if (std::optional<PreloadRequest> request = CreatePreloadRequestIfNeeded(
              ParseCssStringOrUrl(rule_value_), *predicted_base_element_url_,
              ResourceType::kCSSStyleSheet, referrer_policy_,
              PreloadRequest::RequestType::kPreload)) {
        // Blocking unless the style element's media does not match, or it is
        // in the body.
        request->render_blocking = media_matches_ && !in_body_;
        requests_->push_back(std::move(*request));
      }
    }
    state_ = State::kInitial;
  } else if (base::EqualsCaseInsensitiveASCII(rule_, u"charset") ||
             base::EqualsCaseInsensitiveASCII(rule_, u"layer")) {
    state_ = State::kInitial;
  } else {
    state_ = State::kDoneParsingImportRules;
  }
  rule_.clear();
  rule_value_.clear();
  maybe_layer_value_.clear();
  has_trailing_contents_ = false;
}

// TokenPreloadScanner::StartTagScanner: the request that one start tag
// makes.
class TokenPreloadScanner::StartTagScanner {
 public:
  StartTagScanner(std::u16string_view tag,
                  const MediaValues& media_values,
                  const MediaQueryFeatures& features,
                  float device_pixel_ratio,
                  ScannerType scanner_type,
                  std::optional<ForeignContentType> foreign_content)
      : tag_(tag),
        media_values_(media_values),
        features_(features),
        device_pixel_ratio_(device_pixel_ratio),
        scanner_type_(scanner_type) {
    // SVG's image and script load by href, or else xlink:href; MathML's
    // script is no script.
    if (foreign_content == ForeignContentType::kSvg && tag_ == u"image") {
      is_svg_image_ = true;
      tag_ = u"img";
    }
    if (foreign_content == ForeignContentType::kSvg && tag_ == u"script") {
      is_svg_script_ = true;
    } else if (foreign_content == ForeignContentType::kMath &&
               tag_ == u"script") {
      tag_ = u"";
    }
    if (tag_ == u"img" || tag_ == u"source" || tag_ == u"link") {
      source_size_ =
          SizesAttributeParser(*media_values_, *features_, std::nullopt).Size();
      return;
    }
    if (tag_ != u"input" && tag_ != u"script" && tag_ != u"video" &&
        tag_ != u"style") {
      tag_ = u"";
    }
  }

  bool GetMatched() const { return matched_; }

  void ProcessAttributes(const std::vector<HtmlAttribute>& attributes) {
    if (tag_.empty()) {
      return;
    }
    for (const HtmlAttribute& attribute : attributes) {
      ProcessAttribute(attribute.name, attribute.value);
    }
    PostProcessAfterAttributes();
  }

  void HandlePictureSourceUrl(PictureData& picture_data) {
    if (tag_ == u"source" && matched_ && picture_data.source_url.empty()) {
      picture_data.source_url = srcset_image_candidate_.Url().value_or(u"");
      picture_data.source_size_set = source_size_set_;
      picture_data.source_size = source_size_;
      picture_data.picked = true;
    } else if (tag_ == u"img" && !picture_data.source_url.empty()) {
      SetUrlToLoad(picture_data.source_url, /*allow_replacement=*/true);
    }
  }

  std::optional<PreloadRequest> CreatePreloadRequest(
      const GURL& predicted_base_url,
      const PictureData& picture_data,
      FetchReferrerPolicy document_referrer_policy,
      bool treat_links_as_in_body) {
    PreloadRequest::RequestType request_type =
        PreloadRequest::RequestType::kPreload;
    std::optional<ResourceType> type;
    if (ShouldPreconnect()) {
      request_type = PreloadRequest::RequestType::kPreconnect;
    } else {
      if (IsLinkRelPreload()) {
        request_type = PreloadRequest::RequestType::kLinkRelPreload;
        type = ResourceTypeFromAsAttribute(as_attribute_value_);
        if (!type) {
          return std::nullopt;
        }
      } else if (IsLinkRelModulePreload()) {
        // A module script's preload is the module loader's to take, which
        // does not take it yet: a Known Gap.
        return std::nullopt;
      }
      if (!ShouldPreload(type)) {
        return std::nullopt;
      }
    }
    float source_size = source_size_;
    bool source_size_set = source_size_set_;
    if (picture_data.picked) {
      source_size_set = picture_data.source_size_set;
      source_size = picture_data.source_size;
    }
    if (source_size_set) {
      // sizes overrides an explicit width.
      resource_width_ = source_size;
    }
    if (!type) {
      type = GetResourceType();
    }
    // The element's referrerpolicy over the document's.
    const FetchReferrerPolicy referrer_policy =
        referrer_policy_ != FetchReferrerPolicy::kDefault
            ? referrer_policy_
            : document_referrer_policy;
    std::optional<PreloadRequest> request = CreatePreloadRequestIfNeeded(
        url_to_load_, predicted_base_url, *type, referrer_policy, request_type);
    if (!request) {
      return std::nullopt;
    }
    const bool is_script = tag_ == u"script";
    // A module script is the module loader's to take, which does not take a
    // preload yet: a Known Gap.
    if (is_script &&
        ScriptType() == ScriptLoader::ScriptTypeAtPrepare::kModule) {
      return std::nullopt;
    }
    request->cross_origin = cross_origin_;
    request->priority_hint = priority_hint_;
    request->charset = Charset();
    request->lazy = lazy_;
    request->resource_width = resource_width_;
    request->resource_height = resource_height_;
    // RenderBlockingBehavior: a link preload blocks nothing, a lazy script
    // only with blocking=render, and a parser-blocking script or a style
    // sheet unless it is in the body.
    if (request_type == PreloadRequest::RequestType::kLinkRelPreload) {
      request->render_blocking = false;
    } else if (is_script && lazy_) {
      request->render_blocking = HasRenderToken(blocking_attribute_value_);
    } else if (is_script || type == ResourceType::kCSSStyleSheet) {
      request->render_blocking = !treat_links_as_in_body;
    }
    // A lazy image waits for its rendering, never for the scanner.
    if (type == ResourceType::kImage && tag_ == u"img" && loading_lazy_) {
      return std::nullopt;
    }
    if (type == ResourceType::kImage && tag_ == u"video" && loading_lazy_) {
      return std::nullopt;
    }
    // Integrity only for the destinations that support it.
    if (type == ResourceType::kScript || type == ResourceType::kCSSStyleSheet ||
        type == ResourceType::kFont) {
      request->integrity = integrity_;
    }
    request->from_insertion_scanner = scanner_type_ == ScannerType::kInsertion;
    return request;
  }

 private:
  static bool HasRenderToken(std::u16string_view blocking) {
    size_t start = 0;
    while (start < blocking.size()) {
      while (start < blocking.size() && IsHtmlSpace(blocking[start])) {
        ++start;
      }
      size_t end = start;
      while (end < blocking.size() && !IsHtmlSpace(blocking[end])) {
        ++end;
      }
      if (blocking.substr(start, end - start) == u"render") {
        return true;
      }
      start = end;
    }
    return false;
  }

  bool MediaMatches(std::u16string_view media) const {
    return MediaQueryEvaluator(*media_values_)
        .Eval(*ParseMediaQuerySet(media, *features_));
  }

  void SetUrlToLoad(std::u16string_view value, bool allow_replacement) {
    // Only the first src or href counts.
    if (!allow_replacement && !url_to_load_.empty()) {
      return;
    }
    const std::u16string_view url = StripHTMLSpaces(value);
    if (url.empty()) {
      return;
    }
    url_to_load_ = std::u16string(url);
  }

  void SetReferrerPolicy(std::u16string_view value, bool legacy_keywords) {
    referrer_policy_set_ = true;
    referrer_policy_ =
        legacy_keywords ? FetchReferrerPolicyFromStringWithLegacyKeywords(
                              std::u16string(value))
                        : FetchReferrerPolicyFromString(std::u16string(value));
  }

  void SetCrossOrigin(const std::u16string& value) {
    cross_origin_ = CrossOriginAttributeFromValue(value);
  }

  void SetPriorityHint(const std::u16string& value) {
    priority_hint_set_ = true;
    priority_hint_ = FetchPriorityHintFromAttribute(value);
  }

  void ParseSourceSize(const std::u16string& value) {
    const SizesAttributeParser parser(*media_values_, *features_, value);
    source_size_ = parser.Size();
    source_size_set_ = true;
  }

  void ProcessScriptAttribute(const std::u16string& name,
                              const std::u16string& value) {
    if (name == u"src") {
      SetUrlToLoad(value, /*allow_replacement=*/false);
    } else if (name == u"crossorigin") {
      SetCrossOrigin(value);
    } else if (name == u"async" || name == u"defer") {
      lazy_ = true;
    } else if (!integrity_set_ && name == u"integrity") {
      integrity_set_ = true;
      integrity_ = ParseIntegrityAttribute(value, nullptr);
    } else if (name == u"type") {
      type_attribute_value_ = value;
      type_attribute_set_ = true;
    } else if (name == u"language") {
      language_attribute_value_ = value;
      language_attribute_set_ = true;
    } else if (name == u"nomodule") {
      nomodule_ = true;
    } else if (!referrer_policy_set_ && name == u"referrerpolicy") {
      SetReferrerPolicy(value, /*legacy_keywords=*/false);
    } else if (!priority_hint_set_ && name == u"fetchpriority") {
      SetPriorityHint(value);
    } else if (name == u"blocking") {
      blocking_attribute_value_ = value;
    }
  }

  void ProcessImgAttribute(const std::u16string& name,
                           const std::u16string& value) {
    if (name == u"src" && !img_src_url_) {
      img_src_url_ = value;
    } else if (name == u"crossorigin") {
      SetCrossOrigin(value);
    } else if (name == u"srcset" && !srcset_attribute_value_) {
      srcset_attribute_value_ = value;
    } else if (name == u"sizes" && !source_size_set_) {
      ParseSourceSize(value);
    } else if (!referrer_policy_set_ && name == u"referrerpolicy") {
      SetReferrerPolicy(value, /*legacy_keywords=*/true);
    } else if (!priority_hint_set_ && name == u"fetchpriority") {
      SetPriorityHint(value);
    } else if (name == u"width") {
      const std::optional<HtmlDimension> dimension = ParseDimensionValue(value);
      if (dimension && dimension->absolute) {
        resource_width_ = static_cast<float>(dimension->value);
      }
    } else if (name == u"height") {
      const std::optional<HtmlDimension> dimension = ParseDimensionValue(value);
      if (dimension && dimension->absolute) {
        resource_height_ = static_cast<float>(dimension->value);
      }
    } else if (!loading_set_ && name == u"loading") {
      loading_set_ = true;
      loading_lazy_ = base::EqualsCaseInsensitiveASCII(value, u"lazy");
    }
  }

  void SetUrlFromImageAttributes() {
    const ImageCandidate candidate =
        BestFitSourceForImageAttributes(device_pixel_ratio_, source_size_,
                                        img_src_url_, srcset_attribute_value_);
    if (srcset_attribute_value_) {
      srcset_image_candidate_ = BestFitSourceForSrcsetAttribute(
          device_pixel_ratio_, source_size_, *srcset_attribute_value_);
    }
    SetUrlToLoad(candidate.Url().value_or(u""), /*allow_replacement=*/true);
  }

  void ProcessLinkAttribute(const std::u16string& name,
                            const std::u16string& value) {
    if (name == u"href") {
      SetUrlToLoad(value, /*allow_replacement=*/false);
      // For an image preload's srcset.
      img_src_url_ = value;
    } else if (name == u"rel") {
      const LinkRelAttribute rel(value);
      link_is_style_sheet_ = rel.is_style_sheet && !rel.is_alternate &&
                             !rel.is_icon && !rel.is_dns_prefetch;
      link_is_preconnect_ = rel.is_preconnect;
      link_is_preload_ = rel.is_link_preload;
      link_is_modulepreload_ = rel.is_module_preload;
    } else if (name == u"media") {
      matched_ &= MediaMatches(value);
    } else if (name == u"crossorigin") {
      SetCrossOrigin(value);
    } else if (name == u"as") {
      as_attribute_value_ = base::ToLowerASCII(value);
    } else if (name == u"type") {
      type_attribute_value_ = value;
    } else if (!referrer_policy_set_ && name == u"referrerpolicy") {
      SetReferrerPolicy(value, /*legacy_keywords=*/false);
    } else if (!integrity_set_ && name == u"integrity") {
      integrity_set_ = true;
      integrity_ = ParseIntegrityAttribute(value, nullptr);
    } else if (name == u"imagesrcset" && !srcset_attribute_value_) {
      srcset_attribute_value_ = value;
    } else if (name == u"imagesizes" && !source_size_set_) {
      ParseSourceSize(value);
    } else if (!priority_hint_set_ && name == u"fetchpriority") {
      SetPriorityHint(value);
    } else if (name == u"blocking") {
      blocking_attribute_value_ = value;
    } else if (name == u"disabled") {
      disabled_ = true;
    }
  }

  void ProcessInputAttribute(const std::u16string& name,
                             const std::u16string& value) {
    if (name == u"src") {
      SetUrlToLoad(value, /*allow_replacement=*/false);
    } else if (name == u"type") {
      input_is_image_ = base::EqualsCaseInsensitiveASCII(value, u"image");
    }
  }

  void ProcessSourceAttribute(const std::u16string& name,
                              const std::u16string& value) {
    if (name == u"srcset" && srcset_image_candidate_.IsEmpty()) {
      srcset_attribute_value_ = value;
      srcset_image_candidate_ = BestFitSourceForSrcsetAttribute(
          device_pixel_ratio_, source_size_, value);
    } else if (name == u"sizes" && !source_size_set_) {
      ParseSourceSize(value);
      if (!srcset_image_candidate_.IsEmpty()) {
        srcset_image_candidate_ = BestFitSourceForSrcsetAttribute(
            device_pixel_ratio_, source_size_,
            srcset_attribute_value_.value_or(u""));
      }
    } else if (name == u"media") {
      matched_ &= MediaMatches(value);
    } else if (name == u"type") {
      matched_ &= SupportedImageType(&value);
    }
  }

  void ProcessVideoAttribute(const std::u16string& name,
                             const std::u16string& value) {
    if (name == u"poster") {
      SetUrlToLoad(value, /*allow_replacement=*/false);
    } else if (name == u"crossorigin") {
      SetCrossOrigin(value);
    } else if (!loading_set_ && name == u"loading") {
      loading_set_ = true;
      loading_lazy_ = base::EqualsCaseInsensitiveASCII(value, u"lazy");
    }
  }

  void ProcessAttribute(const std::u16string& name,
                        const std::u16string& value) {
    if (name == u"charset") {
      charset_ = value;
    }
    if (is_svg_image_ || is_svg_script_) {
      if (name == u"href") {
        svg_href_value_ = value;
      } else if (name == u"xlink:href") {
        svg_xlink_href_value_ = value;
      }
    } else if (tag_ == u"script") {
      ProcessScriptAttribute(name, value);
    } else if (tag_ == u"img") {
      ProcessImgAttribute(name, value);
    } else if (tag_ == u"link") {
      ProcessLinkAttribute(name, value);
    } else if (tag_ == u"input") {
      ProcessInputAttribute(name, value);
    } else if (tag_ == u"source") {
      ProcessSourceAttribute(name, value);
    } else if (tag_ == u"video") {
      ProcessVideoAttribute(name, value);
    } else if (tag_ == u"style" && name == u"media") {
      matched_ &= MediaMatches(value);
    }
  }

  void PostProcessAfterAttributes() {
    if (is_svg_image_ || is_svg_script_) {
      // href over the deprecated xlink:href.
      SetUrlToLoad(svg_href_value_ ? *svg_href_value_
                                   : svg_xlink_href_value_.value_or(u""),
                   /*allow_replacement=*/false);
      return;
    }
    if (tag_ == u"img" ||
        (link_is_preload_ && as_attribute_value_ == u"image")) {
      SetUrlFromImageAttributes();
    }
  }

  ScriptLoader::ScriptTypeAtPrepare ScriptType() const {
    return ScriptLoader::GetScriptTypeAtPrepare(
        type_attribute_set_
            ? std::optional<std::u16string>(type_attribute_value_)
            : std::nullopt,
        language_attribute_set_
            ? std::optional<std::u16string>(language_attribute_value_)
            : std::nullopt);
  }

  std::u16string Charset() const {
    if (tag_ == u"img" || tag_ == u"video") {
      return std::u16string();
    }
    return charset_;
  }

  ResourceType GetResourceType() const {
    if (tag_ == u"script") {
      return ResourceType::kScript;
    }
    if (tag_ == u"img" || tag_ == u"video" ||
        (tag_ == u"input" && input_is_image_)) {
      return ResourceType::kImage;
    }
    if (tag_ == u"link" && link_is_style_sheet_) {
      return ResourceType::kCSSStyleSheet;
    }
    return ResourceType::kRaw;
  }

  bool ShouldPreconnect() const {
    return tag_ == u"link" && link_is_preconnect_ && !url_to_load_.empty();
  }
  bool IsLinkRelPreload() const {
    return tag_ == u"link" && link_is_preload_ && !url_to_load_.empty();
  }
  bool IsLinkRelModulePreload() const {
    return tag_ == u"link" && link_is_modulepreload_ && !url_to_load_.empty();
  }

  bool ShouldPreloadLink(const std::optional<ResourceType>& type) const {
    if (link_is_style_sheet_) {
      // The tree builder loads no disabled sheet either.
      return !disabled_ &&
             (type_attribute_value_.empty() ||
              ContentTypeType(type_attribute_value_) == "text/css");
    }
    if (link_is_preload_) {
      if (type == ResourceType::kImage) {
        return SupportedImageType(&type_attribute_value_);
      }
      if (type_attribute_value_.empty()) {
        return true;
      }
      const std::string mime_type = ContentTypeType(type_attribute_value_);
      if ((type == ResourceType::kFont &&
           !IsSupportedFontMimeType(mime_type)) ||
          (type == ResourceType::kCSSStyleSheet &&
           !base::EqualsCaseInsensitiveASCII(mime_type, "text/css"))) {
        return false;
      }
      return true;
    }
    return false;
  }

  bool ShouldPreload(const std::optional<ResourceType>& type) const {
    if (url_to_load_.empty() || !matched_) {
      return false;
    }
    if (tag_ == u"link") {
      return ShouldPreloadLink(type);
    }
    if (tag_ == u"input" && !input_is_image_) {
      return false;
    }
    if (tag_ == u"script") {
      switch (ScriptType()) {
        case ScriptLoader::ScriptTypeAtPrepare::kClassic:
          return !nomodule_;
        case ScriptLoader::ScriptTypeAtPrepare::kModule:
          return true;
        default:
          return false;
      }
    }
    return true;
  }

  std::u16string tag_;
  const raw_ref<const MediaValues> media_values_;
  const raw_ref<const MediaQueryFeatures> features_;
  const float device_pixel_ratio_;
  const ScannerType scanner_type_;
  std::u16string url_to_load_;
  ImageCandidate srcset_image_candidate_;
  std::u16string charset_;
  bool link_is_style_sheet_ = false;
  bool link_is_preconnect_ = false;
  bool link_is_preload_ = false;
  bool link_is_modulepreload_ = false;
  bool matched_ = true;
  bool input_is_image_ = false;
  std::optional<std::u16string> img_src_url_;
  std::optional<std::u16string> srcset_attribute_value_;
  std::u16string as_attribute_value_;
  std::u16string type_attribute_value_;
  bool type_attribute_set_ = false;
  std::u16string language_attribute_value_;
  bool language_attribute_set_ = false;
  std::u16string blocking_attribute_value_;
  bool nomodule_ = false;
  float source_size_ = 0;
  bool source_size_set_ = false;
  bool lazy_ = false;
  CrossOriginAttribute cross_origin_ = CrossOriginAttribute::kNotSet;
  FetchPriorityHint priority_hint_ = FetchPriorityHint::kAuto;
  bool priority_hint_set_ = false;
  bool referrer_policy_set_ = false;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
  bool integrity_set_ = false;
  bool disabled_ = false;
  IntegrityMetadataSet integrity_;
  bool loading_set_ = false;
  bool loading_lazy_ = false;
  std::optional<float> resource_width_;
  std::optional<float> resource_height_;
  bool is_svg_image_ = false;
  bool is_svg_script_ = false;
  std::optional<std::u16string> svg_href_value_;
  std::optional<std::u16string> svg_xlink_href_value_;
};

TokenPreloadScanner::TokenPreloadScanner(Document& document,
                                         ScannerType scanner_type)
    : document_url_(GURL(document.UrlForBinding())),
      scanner_type_(scanner_type),
      referrer_policy_(document.GetReferrerPolicy()),
      features_(MediaQueryFeaturesFor(
          bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
              ->features())),
      device_pixel_ratio_(1) {
  if (Window* const window = document.DefaultView()) {
    media_values_ = MediaValuesFor(window->Start());
    device_pixel_ratio_ = static_cast<float>(window->DevicePixelRatio());
  }
  media_values_.strict_mode = !document.InQuirksMode();
  css_scanner_.SetReferrerPolicy(referrer_policy_);
}

TokenPreloadScanner::~TokenPreloadScanner() = default;

void TokenPreloadScanner::Scan(const HtmlToken& token,
                               std::vector<PreloadRequest>& requests,
                               int& csp_meta_tag_count) {
  switch (token.type) {
    case HtmlToken::Type::kCharacter:
      if (in_style_) {
        css_scanner_.Scan(token.data, predicted_base_element_url_, requests);
      }
      return;
    case HtmlToken::Type::kEndTag: {
      const std::u16string& tag = token.name;
      if (tag == u"template") {
        if (template_count_) {
          --template_count_;
        } else if (dsd_count_) {
          --dsd_count_;
        }
        return;
      }
      if (template_count_) {
        return;
      }
      if (tag == u"svg" || tag == u"math") {
        if (!foreign_content_stack_.empty()) {
          foreign_content_stack_.pop_back();
        }
        return;
      }
      if (tag == u"style") {
        if (in_style_) {
          css_scanner_.Reset();
        }
        in_style_ = false;
        return;
      }
      if (tag == u"picture") {
        in_picture_ = false;
        picture_data_.picked = false;
      }
      return;
    }
    case HtmlToken::Type::kStartTag: {
      std::u16string tag = token.name;
      if (tag == u"template") {
        const std::u16string* mode = TokenAttribute(token, u"shadowrootmode");
        const bool is_declarative_shadow_root =
            mode && (base::EqualsCaseInsensitiveASCII(*mode, u"open") ||
                     base::EqualsCaseInsensitiveASCII(*mode, u"closed"));
        if (is_declarative_shadow_root && !template_count_) {
          ++dsd_count_;
        } else {
          ++template_count_;
        }
      }
      if (template_count_) {
        return;
      }
      if (!foreign_content_stack_.empty() &&
          IsForeignContentBreakoutStartTag(token)) {
        foreign_content_stack_.clear();
      }
      // The tree builder makes an img of an image start tag, but not in
      // foreign content.
      if (tag == u"svg") {
        if (!token.self_closing) {
          foreign_content_stack_.push_back(ForeignContentType::kSvg);
        }
      } else if (tag == u"math") {
        if (!token.self_closing) {
          foreign_content_stack_.push_back(ForeignContentType::kMath);
        }
      } else if (foreign_content_stack_.empty() && tag == u"image") {
        tag = u"img";
      }
      if (tag == u"style") {
        in_style_ = true;
        css_scanner_.SetInBody(seen_img_ || seen_body_);
      }
      // The first base element wins, outside shadow roots.
      if (tag == u"base" && !dsd_count_) {
        if (predicted_base_element_url_.is_empty()) {
          UpdatePredictedBaseUrl(token);
        }
        return;
      }
      if (tag == u"meta" && !dsd_count_) {
        if (const std::u16string* equiv =
                TokenAttribute(token, u"http-equiv")) {
          if (base::EqualsCaseInsensitiveASCII(*equiv,
                                               u"content-security-policy")) {
            ++csp_meta_tag_count;
          }
          return;
        }
        const std::u16string* name = TokenAttribute(token, u"name");
        const std::u16string* content = TokenAttribute(token, u"content");
        // HandleMetaReferrer(): a valid policy, legacy keywords included,
        // becomes the document's for what follows.
        if (name && content && !content->empty() &&
            base::EqualsCaseInsensitiveASCII(*name, u"referrer")) {
          const FetchReferrerPolicy policy =
              FetchReferrerPolicyFromStringWithLegacyKeywords(*content);
          if (policy != FetchReferrerPolicy::kDefault) {
            referrer_policy_ = policy;
          }
          css_scanner_.SetReferrerPolicy(referrer_policy_);
        }
      }
      if (tag == u"body") {
        seen_body_ = true;
      } else if (tag == u"img") {
        seen_img_ = true;
        // A data: URL is never preloaded.
        if (const std::u16string* src = TokenAttribute(token, u"src");
            src && base::StartsWith(*src, u"data:",
                                    base::CompareCase::INSENSITIVE_ASCII)) {
          return;
        }
      } else if (tag == u"picture") {
        in_picture_ = true;
        picture_data_ = PictureData();
        return;
      } else if (!IsVoidElement(tag)) {
        // An img after a non-void element is no longer the picture's child.
        in_picture_ = false;
        picture_data_.picked = false;
      }
      std::optional<ForeignContentType> foreign_content;
      if (!foreign_content_stack_.empty()) {
        foreign_content = foreign_content_stack_.back();
      }
      StartTagScanner scanner(tag, media_values_, features_,
                              device_pixel_ratio_, scanner_type_,
                              foreign_content);
      scanner.ProcessAttributes(token.attributes);
      if (in_picture_ && media_values_.viewport_width) {
        scanner.HandlePictureSourceUrl(picture_data_);
      }
      if (in_style_) {
        css_scanner_.SetMediaMatches(scanner.GetMatched());
      }
      if (std::optional<PreloadRequest> request = scanner.CreatePreloadRequest(
              predicted_base_element_url_, picture_data_, referrer_policy_,
              seen_img_ || seen_body_)) {
        requests.push_back(std::move(*request));
      }
      return;
    }
    default:
      return;
  }
}

void TokenPreloadScanner::UpdatePredictedBaseUrl(const HtmlToken& token) {
  const std::u16string* href = TokenAttribute(token, u"href");
  if (!href) {
    return;
  }
  const GURL url = document_url_.Resolve(StripHTMLSpaces(*href));
  // A Page enforces no Content Security Policy's base-uri yet.
  predicted_base_element_url_ =
      url.is_valid() && !url.SchemeIs("data") && !url.SchemeIs("javascript")
          ? url
          : GURL();
}

HtmlPreloadScanner::Result::Result() = default;
HtmlPreloadScanner::Result::Result(Result&&) = default;
HtmlPreloadScanner::Result& HtmlPreloadScanner::Result::operator=(Result&&) =
    default;
HtmlPreloadScanner::Result::~Result() = default;

HtmlPreloadScanner::HtmlPreloadScanner(
    Document& document,
    TokenPreloadScanner::ScannerType scanner_type)
    : tokenizer_(std::make_unique<HtmlTokenizer>(HtmlTokenizer::Options())),
      scanner_(document, scanner_type) {}

HtmlPreloadScanner::~HtmlPreloadScanner() = default;

void HtmlPreloadScanner::AppendToEnd(std::u16string_view source) {
  tokenizer_->Append(source);
}

HtmlPreloadScanner::Result HtmlPreloadScanner::Scan(
    const GURL& starting_base_url) {
  Result result;
  // The document's base element is the best prediction of the base.
  if (!starting_base_url.is_empty()) {
    scanner_.SetPredictedBaseElementUrl(starting_base_url);
  }
  HtmlToken token;
  while (tokenizer_->NextToken(token)) {
    if (token.type == HtmlToken::Type::kStartTag) {
      if (const std::optional<HtmlTokenizer::State> state =
              StateForTag(token.name)) {
        tokenizer_->SetState(*state);
      }
    }
    int csp_meta_tag_count = 0;
    scanner_.Scan(token, result.requests, csp_meta_tag_count);
    result.csp_meta_tag_count += csp_meta_tag_count;
    // AllowPreloadingWithCSPMetaTag is off: a CSP meta element stops the
    // scan, and what is left goes unscanned.
    if (csp_meta_tag_count) {
      tokenizer_ = std::make_unique<HtmlTokenizer>(HtmlTokenizer::Options());
      return result;
    }
  }
  return result;
}

}  // namespace mitto
