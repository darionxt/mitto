#include "mitto/dom/html_image_element.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_image_element.h"
#include "mitto/css/media_values.h"
#include "mitto/css/sizes_attribute_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/html_elements.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/html_srcset_parser.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/mime_types.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

void FireSimpleEvent(EventTarget& target, const std::u16string& type) {
  bindings::EventInit init;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  target.DispatchEvent(event);
}

// The node as an element of an HTML tag, or null.
template <typename T>
T* AsHtmlElement(Node* node, std::u16string_view tag) {
  if (!node || !node->IsElementNode() ||
      !static_cast<Element*>(node)->HasHTMLTagName(tag)) {
    return nullptr;
  }
  return static_cast<T*>(node);
}

// The window whose active document `document` is.
Window* ActiveWindowOf(const Document& document) {
  Window* const window = document.DefaultView();
  if (!window || !window->HasContext() || &window->GetDocument() != &document) {
    return nullptr;
  }
  return window;
}

// HTMLImageElement::SupportedImageType(): an empty type, or one that a
// decoder takes, as ContentType::GetType() reads it.
bool SupportedImageType(const std::u16string* type) {
  if (!type) {
    return true;
  }
  std::u16string_view stripped = base::TrimWhitespace(*type, base::TRIM_ALL);
  if (const size_t semi = stripped.find(';');
      semi != std::u16string_view::npos) {
    stripped = base::TrimWhitespace(stripped.substr(0, semi), base::TRIM_ALL);
  }
  return stripped.empty() ||
         IsSupportedImagePrefixedMimeType(base::UTF16ToUTF8(stripped));
}

}  // namespace

// static
HTMLImageElement* HTMLImageElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLImageElement>(document);
}

// static
HTMLImageElement* HTMLImageElement::CreateForJSConstructor(
    std::optional<uint32_t> width,
    std::optional<uint32_t> height) {
  HTMLImageElement* const image = Create(Window::Current().GetDocument());
  // Blink's overloads take a width before a height, so a width left
  // undefined before a height converts to 0.
  if (height && !width) {
    width = 0;
  }
  if (width) {
    image->SetWidth(*width);
  }
  if (height) {
    image->SetHeight(*height);
  }
  return image;
}

HTMLImageElement::HTMLImageElement(Document& document)
    : HTMLElement(document, u"img"),
      image_loader_(bindings::MakeGarbageCollected<ImageLoader>(*this)) {}

HTMLImageElement::~HTMLImageElement() = default;

const bindings::WrapperTypeInfo* HTMLImageElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLImageElement::kWrapperTypeInfo;
}

void HTMLImageElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(image_loader_);
  visitor->Trace(source_);
  HTMLElement::Trace(visitor);
}

Element::NamedItemType HTMLImageElement::GetNamedItemType() const {
  return NamedItemType::kNameOrIdWithName;
}

bool HTMLImageElement::Draggable() const {
  const std::u16string* value = FindAttribute(u"draggable");
  return !value || !base::EqualsCaseInsensitiveASCII(*value, u"false");
}

uint32_t HTMLImageElement::Width() const {
  if (const std::u16string* value = FindAttribute(u"width")) {
    if (const std::optional<uint32_t> width =
            ParseHTMLNonNegativeInteger(*value)) {
      return *width;
    }
  }
  uint32_t width;
  uint32_t height;
  image_loader_->DensityCorrectedNaturalSize(1, width, height);
  return width;
}

void HTMLImageElement::SetWidth(uint32_t value) {
  SetReflectedUnsignedIntegerAttribute(u"width", value);
}

uint32_t HTMLImageElement::Height() const {
  if (const std::u16string* value = FindAttribute(u"height")) {
    if (const std::optional<uint32_t> height =
            ParseHTMLNonNegativeInteger(*value)) {
      return *height;
    }
  }
  uint32_t width;
  uint32_t height;
  image_loader_->DensityCorrectedNaturalSize(1, width, height);
  return height;
}

void HTMLImageElement::SetHeight(uint32_t value) {
  SetReflectedUnsignedIntegerAttribute(u"height", value);
}

uint32_t HTMLImageElement::NaturalWidth() const {
  uint32_t width;
  uint32_t height;
  image_loader_->DensityCorrectedNaturalSize(image_device_pixel_ratio_, width,
                                             height);
  return width;
}

uint32_t HTMLImageElement::NaturalHeight() const {
  uint32_t width;
  uint32_t height;
  image_loader_->DensityCorrectedNaturalSize(image_device_pixel_ratio_, width,
                                             height);
  return height;
}

bool HTMLImageElement::Complete() const {
  return image_loader_->ImageComplete();
}

std::u16string HTMLImageElement::CurrentSrc() const {
  // The URL that was chosen, for a load that failed an access check.
  if (image_loader_->HadError()) {
    return best_fit_image_url_.value_or(std::u16string());
  }
  // The pending request becomes the current one once it is available or
  // broken.
  const Resource* const content = image_loader_->GetContent();
  if (!content || (!content->ErrorOccurred() && !content->HasImage())) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(content->Url().spec());
}

v8::Local<v8::Promise> HTMLImageElement::Decode(
    bindings::ExceptionState& exception_state) {
  return image_loader_->Decode(exception_state);
}

std::optional<std::u16string> HTMLImageElement::ImageSourceURL() const {
  if (best_fit_image_url_) {
    return best_fit_image_url_;
  }
  if (const std::u16string* src = FindAttribute(u"src")) {
    return *src;
  }
  return std::nullopt;
}

CrossOriginAttribute HTMLImageElement::GetCrossOriginAttribute() const {
  const std::u16string* value = FindAttribute(u"crossorigin");
  return CrossOriginAttributeFromValue(
      value ? std::optional<std::u16string>(*value) : std::nullopt);
}

FetchReferrerPolicy HTMLImageElement::ReferrerPolicyForFetch() const {
  const std::u16string* value = FindAttribute(u"referrerpolicy");
  return value ? FetchReferrerPolicyFromStringWithLegacyKeywords(*value)
               : FetchReferrerPolicy::kDefault;
}

FetchPriorityHint HTMLImageElement::GetFetchPriorityHint() const {
  const std::u16string* value = FindAttribute(u"fetchpriority");
  return value ? FetchPriorityHintFromAttribute(*value)
               : FetchPriorityHint::kAuto;
}

void HTMLImageElement::DispatchLoadEvent() {
  FireSimpleEvent(*this, u"load");
}

void HTMLImageElement::DispatchErrorEvent() {
  FireSimpleEvent(*this, u"error");
}

bool HTMLImageElement::HasLazyLoadingAttribute() const {
  const std::u16string* value = FindAttribute(u"loading");
  return value && base::EqualsCaseInsensitiveASCII(*value, u"lazy");
}

bool HTMLImageElement::AllowAutoSizes() const {
  const std::u16string* sizes = FindAttribute(u"sizes");
  return HasLazyLoadingAttribute() && sizes &&
         (base::EqualsCaseInsensitiveASCII(*sizes, u"auto") ||
          base::StartsWith(*sizes, u"auto,",
                           base::CompareCase::INSENSITIVE_ASCII));
}

void HTMLImageElement::AttributeChanged(const QualifiedName& name,
                                        const std::u16string* old_value,
                                        const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"src" || name.local_name == u"srcset" ||
      name.local_name == u"sizes") {
    SelectSourceURL(ImageLoader::kUpdateIgnorePreviousError);
  } else if (name.local_name == u"referrerpolicy") {
    // Only a change of the policy, not of how it is written, is a relevant
    // mutation.
    const FetchReferrerPolicy old_policy =
        old_value ? FetchReferrerPolicyFromStringWithLegacyKeywords(*old_value)
                  : FetchReferrerPolicy::kDefault;
    const FetchReferrerPolicy new_policy =
        new_value ? FetchReferrerPolicyFromStringWithLegacyKeywords(*new_value)
                  : FetchReferrerPolicy::kDefault;
    if (old_policy != new_policy) {
      image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
    }
  } else if (name.local_name == u"loading") {
    // An image that is no longer lazy loads now.
    if (!HasLazyLoadingAttribute()) {
      image_loader_->LoadDeferredImage();
    }
  } else if (name.local_name == u"crossorigin") {
    auto state = [](const std::u16string* value) {
      return CrossOriginAttributeFromValue(
          value ? std::optional<std::u16string>(*value) : std::nullopt);
    };
    if (state(old_value) != state(new_value)) {
      image_loader_->UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
    }
  }
}

Node::InsertionNotificationRequest HTMLImageElement::InsertedInto(
    Node& insertion_point) {
  // An img that goes into a picture picks its source anew.
  Node* const parent = ParentNode();
  if (parent && parent == &insertion_point &&
      AsHtmlElement<HTMLPictureElement>(parent, u"picture")) {
    SelectSourceURL(ImageLoader::kUpdateIgnorePreviousError);
  }
  const InsertionNotificationRequest request =
      HTMLElement::InsertedInto(insertion_point);
  image_loader_->ElementInserted();
  return request;
}

void HTMLImageElement::RemovedFrom(Node& insertion_point) {
  // One that leaves its picture picks its source anew.
  if (!ParentNode() &&
      AsHtmlElement<HTMLPictureElement>(&insertion_point, u"picture")) {
    SelectSourceURL(ImageLoader::kUpdateIgnorePreviousError);
  }
  HTMLElement::RemovedFrom(insertion_point);
}

void HTMLImageElement::DidMoveToNewDocument(Document& old_document) {
  image_loader_->ElementDidMoveToNewDocument();
  HTMLElement::DidMoveToNewDocument(old_document);
  SelectSourceURL(ImageLoader::kUpdateIgnorePreviousError);
}

// https://html.spec.whatwg.org/#update-the-source-set
ImageCandidate HTMLImageElement::FindBestFitImageFromPictureParent() {
  source_ = nullptr;
  auto* const picture_parent =
      AsHtmlElement<HTMLPictureElement>(ParentNode(), u"picture");
  if (!picture_parent) {
    return ImageCandidate();
  }
  Window* const window = ActiveWindowOf(GetDocument());
  for (Node* child = picture_parent->FirstChild(); child;
       child = child->NextSibling()) {
    if (child == this) {
      return ImageCandidate();
    }
    auto* const source = AsHtmlElement<HTMLSourceElement>(child, u"source");
    if (!source) {
      continue;
    }
    const std::u16string* srcset = source->FindAttribute(u"srcset");
    if (!srcset || srcset->empty()) {
      continue;
    }
    if (!SupportedImageType(source->FindAttribute(u"type"))) {
      continue;
    }
    if (!source->MediaQueryMatches()) {
      continue;
    }
    const SrcsetWarning warn =
        window ? base::BindRepeating(
                     [](Window* window, std::string message) {
                       window->AddConsoleMessage(ConsoleLevel::kWarning,
                                                 std::move(message));
                     },
                     base::Unretained(window))
               : SrcsetWarning();
    const ImageCandidate candidate = BestFitSourceForSrcsetAttribute(
        window ? static_cast<float>(window->DevicePixelRatio()) : 1,
        SourceSize(*source), *srcset, warn, AvailableImages());
    if (candidate.IsEmpty()) {
      continue;
    }
    source_ = source;
    return candidate;
  }
  return ImageCandidate();
}

SrcsetImageAvailable HTMLImageElement::AvailableImages() {
  // AvoidDownloadIfHigherDensityResourceIsInCache(): a denser candidate that
  // the document has loaded already, or a data: URL.
  return base::BindRepeating(
      [](Document* document, const std::u16string& url) {
        const GURL resolved = document->CompleteUrl(StripHTMLSpaces(url));
        if (resolved.SchemeIs(url::kDataScheme)) {
          return true;
        }
        const Resource* const resource =
            document->HasFetcher()
                ? document->Fetcher().CachedResource(resolved)
                : nullptr;
        return resource && resource->IsLoaded();
      },
      base::Unretained(&GetDocument()));
}

float HTMLImageElement::SourceSize(Element& element) {
  Window* const window = ActiveWindowOf(GetDocument());
  if (!window) {
    return 0;
  }
  std::optional<std::u16string> sizes;
  if (const std::u16string* value = element.FindAttribute(u"sizes")) {
    sizes = *value;
  }
  // A source before an img that allows auto-sizes may leave sizes out,
  // which stands for auto.
  if (!sizes && &element != this && AllowAutoSizes()) {
    sizes = u"auto";
  }
  MediaValues values = MediaValuesFor(window->Start());
  values.strict_mode = !GetDocument().InQuirksMode();
  const MediaQueryFeatures features = MediaQueryFeaturesFor(
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features());
  SizesAutoContext auto_context;
  auto_context.allows_auto_sizes = AllowAutoSizes();
  const SizesAttributeParser parser(values, features, sizes, auto_context);
  return parser.Size();
}

void HTMLImageElement::SetBestFitURLAndDPRFromImageCandidate(
    const ImageCandidate& candidate) {
  best_fit_image_url_ = candidate.Url();
  if (candidate.Density() >= 0) {
    image_device_pixel_ratio_ = 1.0f / candidate.Density();
  }
}

void HTMLImageElement::SelectSourceURL(
    ImageLoader::UpdateFromElementBehavior behavior) {
  Window* const window = ActiveWindowOf(GetDocument());
  if (!window) {
    return;
  }
  ImageCandidate candidate = FindBestFitImageFromPictureParent();
  if (candidate.IsEmpty()) {
    const float source_size = SourceSize(*this);
    const std::u16string* src = FindAttribute(u"src");
    const std::u16string* srcset = FindAttribute(u"srcset");
    const SrcsetWarning warn = base::BindRepeating(
        [](Window* window, std::string message) {
          window->AddConsoleMessage(ConsoleLevel::kWarning, std::move(message));
        },
        base::Unretained(window));
    candidate = BestFitSourceForImageAttributes(
        static_cast<float>(window->DevicePixelRatio()), source_size,
        src ? std::optional<std::u16string>(*src) : std::nullopt,
        srcset ? std::optional<std::u16string>(*srcset) : std::nullopt, warn,
        AvailableImages());
  }
  const std::optional<std::u16string> old_url = best_fit_image_url_;
  SetBestFitURLAndDPRFromImageCandidate(candidate);
  // A change of the viewport loads only a new URL, not a new density.
  if (behavior != ImageLoader::kUpdateSizeChanged ||
      best_fit_image_url_ != old_url) {
    image_loader_->UpdateFromElement(behavior);
  }
}

}  // namespace mitto
