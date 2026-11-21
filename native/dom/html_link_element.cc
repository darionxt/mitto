#include "mitto/dom/html_link_element.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_link_element.h"
#include "mitto/css/media_query_evaluator.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"
#include "mitto/css/sizes_attribute_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/html_srcset_parser.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/subresource_integrity.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/mime_types.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The window whose active document `document` is.
Window* ActiveWindowOf(const Document& document) {
  Window* const window = document.DefaultView();
  if (!window || !window->HasContext() || &window->GetDocument() != &document) {
    return nullptr;
  }
  return window;
}

// PreloadHelper::GetResourceTypeFromAsAttribute(), of what a Page fetches:
// track, which Blink preloads too, has no Request Kind yet, a Known Gap.
std::optional<ResourceType> ResourceTypeFromAs(std::u16string_view as) {
  if (as == u"image") {
    return ResourceType::kImage;
  }
  if (as == u"script") {
    return ResourceType::kScript;
  }
  if (as == u"style") {
    return ResourceType::kCSSStyleSheet;
  }
  if (as == u"font") {
    return ResourceType::kFont;
  }
  if (as == u"fetch") {
    return ResourceType::kRaw;
  }
  return std::nullopt;
}

// IsValidButUnsupportedAsAttribute(), and track, which Blink supports.
bool IsValidButUnsupportedAs(std::u16string_view as) {
  return as == u"audio" || as == u"audioworklet" || as == u"document" ||
         as == u"embed" || as == u"manifest" || as == u"object" ||
         as == u"paintworklet" || as == u"report" || as == u"sharedworker" ||
         as == u"video" || as == u"worker" || as == u"xslt";
}

// PreloadHelper's IsSupportedType().
bool IsSupportedType(ResourceType type, std::u16string_view mime_type) {
  if (mime_type.empty()) {
    return true;
  }
  const std::string ascii = base::UTF16ToUTF8(mime_type);
  switch (type) {
    case ResourceType::kImage:
      return IsSupportedImagePrefixedMimeType(ascii);
    case ResourceType::kScript:
      return IsSupportedJavaScriptMimeType(ascii);
    case ResourceType::kCSSStyleSheet:
      return base::EqualsCaseInsensitiveASCII(ascii, "text/css");
    case ResourceType::kFont:
      return IsSupportedFontMimeType(ascii);
    case ResourceType::kRaw:
      return true;
  }
}

// An attribute's value, or "" for none.
std::u16string_view AttributeOrEmpty(const Element& element,
                                     std::u16string_view name) {
  const std::u16string* value = element.FindAttribute(name);
  return value ? std::u16string_view(*value) : std::u16string_view();
}

void FireSimpleEvent(EventTarget& target, const std::u16string& type) {
  bindings::EventInit init;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  target.DispatchEvent(event);
}

}  // namespace

// Blink's PendingLinkPreload, as a module preload's client: tells the link of
// the module script that it waits for, until the link lets it go.
class HTMLLinkElement::ModulePreloadClient final : public SingleModuleClient {
 public:
  explicit ModulePreloadClient(HTMLLinkElement& link) : link_(&link) {}

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(link_);
    SingleModuleClient::Trace(visitor);
  }

  // PendingLinkPreload::Dispose(): the link no longer waits.
  void Dispose() { link_ = nullptr; }

  // SingleModuleClient:
  void NotifyModuleLoadFinished(ModuleScript* module_script) override {
    if (HTMLLinkElement* const link = link_.Get()) {
      link_ = nullptr;
      link->ModulePreloadFinished(*this, module_script != nullptr);
    }
  }

 private:
  cppgc::Member<HTMLLinkElement> link_;
};

LinkRelAttribute::LinkRelAttribute(std::u16string_view rel) {
  for (const std::u16string_view token :
       base::SplitStringPiece(rel, u" \t\n\f\r", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const std::u16string link_type = base::ToLowerASCII(token);
    if (link_type == u"stylesheet") {
      is_style_sheet = true;
    } else if (link_type == u"alternate") {
      is_alternate = true;
    } else if (link_type == u"icon") {
      // "shortcut icon" too, as "shortcut" is ignored.
      is_icon = true;
    } else if (link_type == u"prefetch") {
      is_link_prefetch = true;
    } else if (link_type == u"dns-prefetch") {
      is_dns_prefetch = true;
    } else if (link_type == u"preconnect") {
      is_preconnect = true;
    } else if (link_type == u"preload") {
      is_link_preload = true;
    } else if (link_type == u"modulepreload") {
      is_module_preload = true;
    } else if (link_type == u"expect") {
      is_expect = true;
    }
  }
}

// static
HTMLLinkElement* HTMLLinkElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLinkElement>(document);
}

HTMLLinkElement::HTMLLinkElement(Document& document)
    : HTMLElement(document, u"link") {}

HTMLLinkElement::~HTMLLinkElement() = default;

const bindings::WrapperTypeInfo* HTMLLinkElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLinkElement::kWrapperTypeInfo;
}

void HTMLLinkElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(pending_resource_);
  visitor->Trace(module_preload_);
  HTMLElement::Trace(visitor);
}

void HTMLLinkElement::AttributeChanged(const QualifiedName& name,
                                       const std::u16string* old_value,
                                       const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  // The attributes that HTMLLinkElement::ParseAttribute() processes the link
  // anew for, unless the value stays the same, as
  // HTMLLinkElementAttributeValueChanges, stable, has it.
  if (name.local_name != u"rel" && name.local_name != u"href" &&
      name.local_name != u"type" && name.local_name != u"as" &&
      name.local_name != u"media" && name.local_name != u"sizes") {
    return;
  }
  const bool same = old_value && new_value && *old_value == *new_value;
  if (same && name.local_name != u"sizes") {
    return;
  }
  Process();
}

Node::InsertionNotificationRequest HTMLLinkElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  if (insertion_point.IsConnected()) {
    Process();
  }
  return InsertionNotificationRequest::kInsertionDone;
}

void HTMLLinkElement::RemovedFrom(Node& insertion_point) {
  HTMLElement::RemovedFrom(insertion_point);
  if (insertion_point.IsConnected() &&
      !GetDocument().StatePreservingAtomicMoveInProgress()) {
    Abort();
  }
}

bool HTMLLinkElement::ShouldLoadLink() const {
  // A link in a document that will be rendered, but for a style sheet's in
  // any document's tree.
  const LinkRelAttribute rel(AttributeOrEmpty(*this, u"rel"));
  if (!IsConnected() ||
      (!ActiveWindowOf(GetDocument()) && !rel.is_style_sheet)) {
    return false;
  }
  return !rel.is_expect;
}

void HTMLLinkElement::Abort() {
  ++load_generation_;
  if (Resource* const resource = pending_resource_.Get()) {
    pending_resource_ = nullptr;
    resource->RemoveClient(this);
  }
  if (ModulePreloadClient* const client = module_preload_.Get()) {
    module_preload_ = nullptr;
    client->Dispose();
  }
}

void HTMLLinkElement::Process() {
  // LinkLoader::LoadLink(): what it loaded before goes.
  Abort();
  if (!ShouldLoadLink()) {
    return;
  }
  if (Resource* const resource = PreloadIfNeeded()) {
    pending_resource_ = resource;
    adding_client_ = true;
    resource->AddClient(this);
    adding_client_ = false;
  }
  ModulePreloadIfNeeded();
}

Resource* HTMLLinkElement::PreloadIfNeeded() {
  const LinkRelAttribute rel(AttributeOrEmpty(*this, u"rel"));
  Window* const window = ActiveWindowOf(GetDocument());
  if (!rel.is_link_preload || !window) {
    return nullptr;
  }
  Document& document = GetDocument();
  const std::u16string as = base::ToLowerASCII(AttributeOrEmpty(*this, u"as"));
  const std::optional<ResourceType> type = ResourceTypeFromAs(as);
  const std::u16string_view href =
      StripHTMLSpaces(AttributeOrEmpty(*this, u"href"));
  GURL url = href.empty() ? GURL() : document.CompleteUrl(href);
  MediaValues values = MediaValuesFor(window->Start());
  values.strict_mode = !document.InQuirksMode();
  const MediaQueryFeatures features = MediaQueryFeaturesFor(
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features());
  // An image's imagesrcset and imagesizes choose its URL, as an img's do.
  const std::u16string* image_srcset = FindAttribute(u"imagesrcset");
  if (type == ResourceType::kImage && image_srcset && !image_srcset->empty()) {
    const std::u16string* image_sizes = FindAttribute(u"imagesizes");
    const float source_size =
        SizesAttributeParser(
            values, features,
            image_sizes ? std::optional<std::u16string_view>(*image_sizes)
                        : std::nullopt)
            .Size();
    const ImageCandidate candidate = BestFitSourceForImageAttributes(
        static_cast<float>(window->DevicePixelRatio()), source_size,
        url.is_valid()
            ? std::optional<std::u16string>(base::UTF8ToUTF16(url.spec()))
            : std::nullopt,
        *image_srcset);
    url = candidate.Url()
              ? document.CompleteUrl(StripHTMLSpaces(*candidate.Url()))
              : GURL();
  }
  if (!url.is_valid() || url.is_empty()) {
    window->AddConsoleMessage(ConsoleLevel::kWarning,
                              "<link rel=preload> has an invalid `href` value");
    return nullptr;
  }
  // Only if media matches.
  const std::u16string* media = FindAttribute(u"media");
  if (media && !media->empty() &&
      !MediaQueryEvaluator(values).Eval(
          *ParseMediaQuerySet(*media, features))) {
    return nullptr;
  }
  if (!type) {
    if (as != u"track") {
      window->AddConsoleMessage(
          ConsoleLevel::kWarning,
          IsValidButUnsupportedAs(as)
              ? "<link rel=preload> uses an unsupported `as` value"
              : "<link rel=preload> must have a valid `as` value");
    }
    return nullptr;
  }
  if (!IsSupportedType(*type, AttributeOrEmpty(*this, u"type"))) {
    window->AddConsoleMessage(
        ConsoleLevel::kWarning,
        "<link rel=preload> has an unsupported `type` value");
    return nullptr;
  }
  FetchParameters params;
  params.url = url;
  params.type = *type;
  // Without crossorigin, a preload goes in mode no-cors with credentials,
  // even a font's, unlike @font-face's.
  const std::u16string* cross_origin = FindAttribute(u"crossorigin");
  switch (CrossOriginAttributeFromValue(
      cross_origin ? std::optional<std::u16string>(*cross_origin)
                   : std::nullopt)) {
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
  if (const std::u16string* policy = FindAttribute(u"referrerpolicy")) {
    params.referrer_policy = FetchReferrerPolicyFromString(*policy);
  }
  if (const std::u16string* hint = FindAttribute(u"fetchpriority")) {
    params.priority_hint = FetchPriorityHintFromAttribute(*hint);
  }
  const std::u16string* integrity = FindAttribute(u"integrity");
  if (integrity && !integrity->empty()) {
    if (type == ResourceType::kScript || type == ResourceType::kCSSStyleSheet ||
        type == ResourceType::kFont) {
      params.integrity = ParseIntegrityAttribute(*integrity, nullptr);
    } else {
      window->AddConsoleMessage(
          ConsoleLevel::kWarning,
          "The `integrity` attribute is currently ignored for preload "
          "destinations that do not support subresource integrity. See "
          "https://crbug.com/981419 for more information");
    }
  }
  params.charset = document.Encoding();
  params.is_link_preload = true;
  return document.Fetcher().RequestResource(params);
}

void HTMLLinkElement::ModulePreloadIfNeeded() {
  const LinkRelAttribute rel(AttributeOrEmpty(*this, u"rel"));
  Window* const window = ActiveWindowOf(GetDocument());
  if (!rel.is_module_preload || !window) {
    return;
  }
  Document& document = GetDocument();
  // Step 1: no href, no fetch.
  const std::u16string_view href =
      StripHTMLSpaces(AttributeOrEmpty(*this, u"href"));
  if (href.empty()) {
    window->AddConsoleMessage(ConsoleLevel::kWarning,
                              "<link rel=modulepreload> has no `href` value");
    return;
  }
  Modulator* const modulator = window->GetModulator();
  if (!modulator) {
    return;
  }
  // Steps 2 and 3: the destination that as names, script where it names
  // none, and style and json, as ModulePreloadStyleJson, stable, has it.
  // Any other fails from a task.
  const std::u16string as = base::ToLowerASCII(AttributeOrEmpty(*this, u"as"));
  ModuleType module_type = ModuleType::kInvalid;
  if (as.empty() || as == u"script") {
    module_type = ModuleType::kJavaScriptOrWasm;
  } else if (as == u"style") {
    module_type = ModuleType::kCSS;
  } else if (as == u"json") {
    module_type = ModuleType::kJSON;
  } else {
    window->AddConsoleMessage(
        ConsoleLevel::kWarning,
        "<link rel=modulepreload> has an invalid `as` value " +
            base::UTF16ToUTF8(as));
    module_preload_ =
        bindings::MakeGarbageCollected<ModulePreloadClient>(*this);
    modulator->PostTask(base::BindOnce(
        &ModulePreloadClient::NotifyModuleLoadFinished,
        cppgc::Persistent<ModulePreloadClient>(module_preload_.Get()),
        nullptr));
    return;
  }
  // Step 4.
  const GURL url = document.CompleteUrl(href);
  if (!url.is_valid()) {
    window->AddConsoleMessage(
        ConsoleLevel::kWarning,
        "<link rel=modulepreload> has an invalid `href` value " +
            base::UTF16ToUTF8(href));
    return;
  }
  // Only if media matches.
  const std::u16string* media = FindAttribute(u"media");
  if (media && !media->empty()) {
    MediaValues values = MediaValuesFor(window->Start());
    values.strict_mode = !document.InQuirksMode();
    const MediaQueryFeatures features = MediaQueryFeaturesFor(
        bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features());
    if (!MediaQueryEvaluator(values).Eval(
            *ParseMediaQuerySet(*media, features))) {
      return;
    }
  }
  // Step 6: the module script credentials mode for crossorigin.
  ScriptFetchOptions options;
  const std::u16string* cross_origin = FindAttribute(u"crossorigin");
  options.credentials_mode =
      CrossOriginAttributeFromValue(
          cross_origin ? std::optional<std::u16string>(*cross_origin)
                       : std::nullopt) == CrossOriginAttribute::kUseCredentials
          ? FetchCredentialsMode::kInclude
          : FetchCredentialsMode::kSameOrigin;
  // Step 7.
  options.nonce = Nonce();
  // Steps 9 and 10: the integrity attribute's metadata, or, with no such
  // attribute, what the import map's integrity says of url.
  if (const std::u16string* integrity = FindAttribute(u"integrity")) {
    options.integrity_attribute = *integrity;
    if (!integrity->empty()) {
      std::vector<std::string> messages;
      options.integrity_metadata =
          ParseIntegrityAttribute(*integrity, &messages);
      for (std::string& message : messages) {
        window->AddConsoleMessage(ConsoleLevel::kError, std::move(message));
      }
    }
  } else {
    options.integrity_attribute = modulator->GetIntegrityMetadataString(url);
    options.integrity_metadata = modulator->GetIntegrityMetadata(url);
  }
  // Steps 11 to 13.
  options.parser_state = ParserDisposition::kNotParserInserted;
  options.referrer_policy =
      FetchReferrerPolicyFromString(AttributeOrEmpty(*this, u"referrerpolicy"));
  options.fetch_priority_hint =
      FetchPriorityHintFromAttribute(AttributeOrEmpty(*this, u"fetchpriority"));
  options.render_blocking = RenderBlockingBehavior::kNonBlocking;
  // Step 14, with the document's referrer, as ModulePreloadReferrer, stable,
  // has it: the single module script, not its imports.
  modulator->SetAcquiringImportMapsState(
      Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad);
  module_preload_ = bindings::MakeGarbageCollected<ModulePreloadClient>(*this);
  modulator->FetchSingle(
      // A JavaScript module's destination; a JSON or CSS one has its own.
      ModuleScriptFetchRequest(url, module_type, FetchDestination::kScript,
                               options, kClientReferrer),
      ModuleGraphLevel::kDependentModuleFetch, module_preload_.Get());
}

void HTMLLinkElement::ModulePreloadFinished(ModulePreloadClient& client,
                                            bool loaded) {
  if (&client != module_preload_.Get()) {
    return;
  }
  module_preload_ = nullptr;
  // Steps 14 and 15.
  DispatchLinkEvent(loaded);
}

void HTMLLinkElement::NotifyFinished(Resource& resource) {
  if (&resource != pending_resource_.Get()) {
    return;
  }
  // A preload whose body does not match its integrity metadata fails too.
  bool loaded = !resource.ErrorOccurred();
  if (loaded && !resource.Integrity().empty()) {
    std::vector<std::string> messages;
    loaded = CheckSubresourceIntegrity(
        resource.Integrity(), resource.Body(), resource.Url().spec(),
        !resource.Response().opaque, resource.Response().headers, messages);
    if (Window* const window = ActiveWindowOf(GetDocument())) {
      for (std::string& message : messages) {
        window->AddConsoleMessage(ConsoleLevel::kError, std::move(message));
      }
    }
  }
  pending_resource_ = nullptr;
  resource.RemoveClient(this);
  if (!adding_client_) {
    DispatchLinkEvent(loaded);
    return;
  }
  // A resource that has ended tells a finish observer from a task, as
  // Resource::TriggerNotificationForFinishObservers() posts it.
  Window* const window = ActiveWindowOf(GetDocument());
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(
      TaskType::kNetworking,
      base::BindOnce(
          [](const cppgc::Persistent<HTMLLinkElement>& link,
             uint64_t generation, bool loaded) {
            if (link->load_generation_ == generation) {
              link->DispatchLinkEvent(loaded);
            }
          },
          cppgc::Persistent<HTMLLinkElement>(this), load_generation_, loaded));
}

void HTMLLinkElement::DispatchLinkEvent(bool loaded) {
  FireSimpleEvent(*this, loaded ? u"load" : u"error");
}

}  // namespace mitto
