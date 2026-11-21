#include "mitto/dom/libxml_tree_builder.h"

#include <libxml/SAX2.h>
#include <libxml/entities.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlIO.h>
#include <libxml/xmlerror.h>
#include <stdarg.h>
#include <stdio.h>

#include <climits>
#include <cstring>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/text.h"
#include "mitto/dom/xml_errors.h"
#include "mitto/html/html_entity_table.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

// One call of libxml2's, as Blink's PendingCallback keeps it.
struct LibxmlTreeBuilder::Event {
  enum class Type {
    kStartDocument,
    kStartElement,
    kEndElement,
    kCharacters,
    kProcessingInstruction,
    kCData,
    kComment,
    kInternalSubset,
    kEndDocument,
    kError,
  };
  struct Attribute {
    std::optional<std::u16string> prefix;
    std::u16string local_name;
    std::optional<std::u16string> uri;
    std::u16string value;
  };

  Type type;
  MarkupPosition position;
  // Names and text, as the type has them.
  std::optional<std::u16string> first;
  std::optional<std::u16string> second;
  std::optional<std::u16string> third;
  int number = 0;
  std::vector<XmlDomBuilder::NamespaceDeclaration> namespaces;
  std::vector<Attribute> attributes;
  std::string message;
};

namespace {

std::optional<std::u16string> ToUtf16(const xmlChar* string) {
  if (!string) {
    return std::nullopt;
  }
  return base::UTF8ToUTF16(reinterpret_cast<const char*>(string));
}

std::u16string ToUtf16(const xmlChar* string, int length) {
  return base::UTF8ToUTF16(
      std::string_view(reinterpret_cast<const char*>(string),
                       base::checked_cast<size_t>(length)));
}

LibxmlTreeBuilder* GetBuilder(void* closure) {
  return static_cast<LibxmlTreeBuilder*>(
      static_cast<xmlParserCtxt*>(closure)->_private);
}

std::unique_ptr<LibxmlTreeBuilder::Event> NewEvent(
    void* closure,
    LibxmlTreeBuilder::Event::Type type) {
  auto event = std::make_unique<LibxmlTreeBuilder::Event>();
  event->type = type;
  event->position = GetBuilder(closure)->ContextPosition();
  return event;
}

bool IsFeatureEnabled(bindings::RuntimeFeature feature) {
  v8::Isolate* const isolate = v8::Isolate::TryGetCurrent();
  const bindings::RuntimeFeatureSet* const features =
      isolate ? bindings::PerIsolateData::FeaturesOf(isolate) : nullptr;
  return features && features->IsEnabled(feature);
}

// libxml2 asks these for each external resource, which a Page never loads:
// every one is empty, as Blink's descriptor of a refused load is.
int g_global_descriptor = 0;

int MatchFunc(const char*) {
  return 1;
}

void* OpenFunc(const char*) {
  return &g_global_descriptor;
}

int ReadFunc(void*, char*, int) {
  return 0;
}

int WriteFunc(void*, const char*, int) {
  return 0;
}

int CloseFunc(void*) {
  return 0;
}

void EnsureLibxmlInitialized() {
  static bool did_init = false;
  if (did_init) {
    return;
  }
  xmlInitParser();
  xmlRegisterInputCallbacks(MatchFunc, OpenFunc, ReadFunc, CloseFunc);
  xmlRegisterOutputCallbacks(MatchFunc, OpenFunc, WriteFunc, CloseFunc);
  did_init = true;
}

// Each piece goes in as UTF-16, whose encoding libxml2 is told before it
// reads, as Blink tells it for a 16-bit string.
void SwitchEncoding(xmlParserCtxt* context) {
  if (context->errNo != XML_ERR_OK) {
    return;
  }
  const char16_t kBom = 0xFEFF;
  const unsigned char bom_high_byte =
      *reinterpret_cast<const unsigned char*>(&kBom);
  xmlSwitchEncoding(context, bom_high_byte == 0xFF ? XML_CHAR_ENCODING_UTF16LE
                                                   : XML_CHAR_ENCODING_UTF16BE);
}

void StartElementNsHandler(void* closure,
                           const xmlChar* local_name,
                           const xmlChar* prefix,
                           const xmlChar* uri,
                           int nb_namespaces,
                           const xmlChar** namespaces,
                           int nb_attributes,
                           int nb_defaulted,
                           const xmlChar** attributes) {
  auto event = NewEvent(closure, LibxmlTreeBuilder::Event::Type::kStartElement);
  event->first = ToUtf16(local_name);
  event->second = ToUtf16(prefix);
  event->third = ToUtf16(uri);
  // Two strings for each namespace, the prefix and the URI.
  for (int i = 0; i < nb_namespaces; ++i) {
    const xmlChar* const declared_prefix = UNSAFE_TODO(namespaces[2 * i]);
    const xmlChar* const declared_uri = UNSAFE_TODO(namespaces[2 * i + 1]);
    event->namespaces.push_back(
        {ToUtf16(declared_prefix), ToUtf16(declared_uri).value_or(u"")});
  }
  // Five for each attribute: the local name, the prefix, the URI, and where
  // the value starts and ends.
  for (int i = 0; i < nb_attributes; ++i) {
    const xmlChar* const* const attribute = UNSAFE_TODO(attributes + 5 * i);
    const xmlChar* const value = UNSAFE_TODO(attribute[3]);
    const xmlChar* const end = UNSAFE_TODO(attribute[4]);
    event->attributes.push_back(
        {ToUtf16(UNSAFE_TODO(attribute[1])),
         ToUtf16(UNSAFE_TODO(attribute[0])).value_or(u""),
         ToUtf16(UNSAFE_TODO(attribute[2])),
         ToUtf16(value, static_cast<int>(end - value))});
  }
  GetBuilder(closure)->Enqueue(std::move(event));
}

void EndElementNsHandler(void* closure,
                         const xmlChar*,
                         const xmlChar*,
                         const xmlChar*) {
  GetBuilder(closure)->Enqueue(
      NewEvent(closure, LibxmlTreeBuilder::Event::Type::kEndElement));
}

void CharactersHandler(void* closure, const xmlChar* chars, int length) {
  auto event = NewEvent(closure, LibxmlTreeBuilder::Event::Type::kCharacters);
  event->first = ToUtf16(chars, length);
  GetBuilder(closure)->Enqueue(std::move(event));
}

void ProcessingInstructionHandler(void* closure,
                                  const xmlChar* target,
                                  const xmlChar* data) {
  auto event =
      NewEvent(closure, LibxmlTreeBuilder::Event::Type::kProcessingInstruction);
  event->first = ToUtf16(target).value_or(u"");
  event->second = ToUtf16(data).value_or(u"");
  GetBuilder(closure)->Enqueue(std::move(event));
}

void CdataBlockHandler(void* closure, const xmlChar* text, int length) {
  auto event = NewEvent(closure, LibxmlTreeBuilder::Event::Type::kCData);
  event->first = ToUtf16(text, length);
  GetBuilder(closure)->Enqueue(std::move(event));
}

void CommentHandler(void* closure, const xmlChar* text) {
  auto event = NewEvent(closure, LibxmlTreeBuilder::Event::Type::kComment);
  event->first = ToUtf16(text).value_or(u"");
  GetBuilder(closure)->Enqueue(std::move(event));
}

// The message as libxml2 writes it, in up to 1023 bytes, as Blink's
// GetError() formats it.
void EnqueueError(void* closure,
                  XmlErrors::Type type,
                  const char* message,
                  va_list args) {
  char formatted[1024] = {};
  UNSAFE_TODO(vsnprintf(formatted, sizeof(formatted) - 1, message, args));
  auto event = NewEvent(closure, LibxmlTreeBuilder::Event::Type::kError);
  event->number = static_cast<int>(type);
  event->message = formatted;
  GetBuilder(closure)->Enqueue(std::move(event));
}

PRINTF_FORMAT(2, 3)
void WarningHandler(void* closure, const char* message, ...) {
  va_list args;
  va_start(args, message);
  EnqueueError(closure, XmlErrors::Type::kWarning, message, args);
  va_end(args);
}

// Blink takes both libxml2's errors and its fatal errors as errors that are
// not fatal: libxml2 stops reading after a fatal one by itself.
PRINTF_FORMAT(2, 3)
void NormalErrorHandler(void* closure, const char* message, ...) {
  va_list args;
  va_start(args, message);
  EnqueueError(closure, XmlErrors::Type::kNonFatal, message, args);
  va_end(args);
}

// An XHTML DTD's entity, which libxml2 parses as markup, in a buffer that
// Blink shares between them.
xmlChar g_shared_xhtml_entity_result[9] = {};

xmlEntityPtr SharedXhtmlEntity() {
  static xmlEntity entity;
  if (!entity.type) {
    entity.type = XML_ENTITY_DECL;
    entity.orig = g_shared_xhtml_entity_result;
    entity.content = g_shared_xhtml_entity_result;
  }
  return &entity;
}

// GetXHTMLEntity(): HTML's named character reference of `name` and a
// semicolon, as UTF-8, with "&" and "<" as character references, since
// libxml2 parses an entity's content.
xmlEntityPtr GetXhtmlEntity(const xmlChar* name) {
  const std::string wanted =
      std::string(reinterpret_cast<const char*>(name)) + ";";
  const HtmlEntity* found = nullptr;
  for (const HtmlEntity& entity : HtmlEntityTable()) {
    if (entity.name == wanted) {
      found = &entity;
      break;
    }
  }
  if (!found) {
    return nullptr;
  }
  std::string utf8;
  if (found->first == '&' && !found->second) {
    utf8 = "&#38;";
  } else if (found->first == '<' && !found->second) {
    utf8 = "&#60;";
  } else if (found->first == '<' && found->second == 0x20D2) {
    utf8 = "&#60;\xE2\x83\x92";
  } else {
    std::u32string code_points(1, found->first);
    if (found->second) {
      code_points.push_back(found->second);
    }
    std::u16string utf16;
    for (const char32_t c : code_points) {
      if (c >= 0x10000) {
        utf16.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
        utf16.push_back(
            static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
      } else {
        utf16.push_back(static_cast<char16_t>(c));
      }
    }
    utf8 = base::UTF16ToUTF8(utf16);
  }
  CHECK_LT(utf8.size(), sizeof(g_shared_xhtml_entity_result));
  UNSAFE_TODO(memcpy(g_shared_xhtml_entity_result, utf8.data(), utf8.size()));
  UNSAFE_TODO(g_shared_xhtml_entity_result[utf8.size()]) = 0;
  xmlEntityPtr const entity = SharedXhtmlEntity();
  entity->length = static_cast<int>(utf8.size());
  entity->name = name;
  entity->etype = XML_INTERNAL_GENERAL_ENTITY;
  return entity;
}

xmlEntityPtr GetEntityHandler(void* closure, const xmlChar* name) {
  auto* const context = static_cast<xmlParserCtxt*>(closure);
  if (xmlEntityPtr const predefined = xmlGetPredefinedEntity(name)) {
    return predefined;
  }
  xmlEntityPtr entity = xmlGetDocEntity(context->myDoc, name);
  if (!entity && GetBuilder(closure)->is_xhtml_document()) {
    entity = GetXhtmlEntity(name);
  }
  return entity;
}

void StartDocumentHandler(void* closure) {
  auto* const context = static_cast<xmlParserCtxt*>(closure);
  // The encoding that the XML declaration names would make libxml2 read the
  // rest wrongly; it stays UTF-16, and the name goes to xmlEncoding.
  SwitchEncoding(context);
  auto event =
      NewEvent(closure, LibxmlTreeBuilder::Event::Type::kStartDocument);
  event->first = ToUtf16(context->version);
  event->second = ToUtf16(context->encoding);
  event->number = context->standalone;
  GetBuilder(closure)->Enqueue(std::move(event));
  xmlSAX2StartDocument(closure);
}

void EndDocumentHandler(void* closure) {
  GetBuilder(closure)->Enqueue(
      NewEvent(closure, LibxmlTreeBuilder::Event::Type::kEndDocument));
  xmlSAX2EndDocument(closure);
}

void InternalSubsetHandler(void* closure,
                           const xmlChar* name,
                           const xmlChar* external_id,
                           const xmlChar* system_id) {
  auto event =
      NewEvent(closure, LibxmlTreeBuilder::Event::Type::kInternalSubset);
  event->first = ToUtf16(name).value_or(u"");
  event->second = ToUtf16(external_id).value_or(u"");
  event->third = ToUtf16(system_id).value_or(u"");
  GetBuilder(closure)->Enqueue(std::move(event));
  xmlSAX2InternalSubset(closure, name, external_id, system_id);
}

void ExternalSubsetHandler(void* closure,
                           const xmlChar*,
                           const xmlChar* external_id,
                           const xmlChar*) {
  // The HTML Standard's named character references for XHTML's DTDs.
  if (MatchesXhtmlSubsetDtd(ToUtf16(external_id).value_or(u""))) {
    GetBuilder(closure)->set_is_xhtml_document();
  }
}

void IgnorableWhitespaceHandler(void*, const xmlChar*, int) {}

}  // namespace

LibxmlTreeBuilder::LibxmlTreeBuilder(HtmlTreeBuilderReferences& references,
                                     bool scripts_already_started)
    : builder_(references,
               /*fragment=*/nullptr,
               /*context_element=*/nullptr,
               scripts_already_started) {}

LibxmlTreeBuilder::LibxmlTreeBuilder(HtmlTreeBuilderReferences& references,
                                     DocumentFragment& fragment,
                                     Element* context_element)
    : builder_(references,
               &fragment,
               context_element,
               /*scripts_already_started=*/true) {}

LibxmlTreeBuilder::~LibxmlTreeBuilder() {
  if (context_) {
    if (context_->myDoc) {
      xmlFreeDoc(context_->myDoc);
    }
    xmlFreeParserCtxt(context_.ExtractAsDangling());
  }
}

void LibxmlTreeBuilder::InitializeParserContext(
    const std::string* fragment_source) {
  EnsureLibxmlInitialized();
  xmlSAXHandler sax = {};
  sax.error = NormalErrorHandler;
  sax.fatalError = NormalErrorHandler;
  sax.characters = CharactersHandler;
  sax.processingInstruction = ProcessingInstructionHandler;
  sax.cdataBlock = CdataBlockHandler;
  sax.comment = CommentHandler;
  sax.warning = WarningHandler;
  sax.startElementNs = StartElementNsHandler;
  sax.endElementNs = EndElementNsHandler;
  sax.getEntity = GetEntityHandler;
  sax.startDocument = StartDocumentHandler;
  sax.endDocument = EndDocumentHandler;
  sax.internalSubset = InternalSubsetHandler;
  sax.externalSubset = ExternalSubsetHandler;
  sax.ignorableWhitespace = IgnorableWhitespaceHandler;
  sax.entityDecl = xmlSAX2EntityDecl;
  sax.initialized = XML_SAX2_MAGIC;
  int options = XML_PARSE_HUGE | XML_PARSE_NOENT;
  if (IsFeatureEnabled(bindings::RuntimeFeature::kXMLNoExternalEntities)) {
    options |= XML_PARSE_NO_XXE;
  }
  if (fragment_source) {
    context_ = xmlCreateMemoryParserCtxt(
        fragment_source->c_str(),
        base::checked_cast<int>(fragment_source->size()));
    if (!context_) {
      return;
    }
    UNSAFE_TODO(memcpy(context_->sax, &sax, sizeof(xmlSAXHandler)));
    options |= XML_PARSE_NODICT;
  } else {
    context_ = xmlCreatePushParserCtxt(&sax, nullptr, nullptr, 0, nullptr);
  }
  xmlCtxtUseOptions(context_.get(), options);
  context_->_private = this;
}

void LibxmlTreeBuilder::Append(std::u16string_view text) {
  if (builder_.IsStopped() || finished_) {
    return;
  }
  // DoWrite(): the context comes with the first piece, even an empty one.
  if (!context_) {
    InitializeParserContext(nullptr);
  }
  // libxml2 refuses to switch the encoding of an empty string.
  if (text.empty()) {
    return;
  }
  SwitchEncoding(context_);
  xmlParseChunk(context_.get(), reinterpret_cast<const char*>(text.data()),
                base::checked_cast<int>(text.size() * sizeof(char16_t)), 0);
  // DoWrite(): once the decoder of the body has met a byte that it cannot
  // decode, which stopped it, a fatal error where libxml2 has got to.
  if (builder_.document().SawDecodingError()) {
    auto event = std::make_unique<Event>();
    event->type = Event::Type::kError;
    event->position = ContextPosition();
    event->number = static_cast<int>(XmlErrors::Type::kFatal);
    event->message = "Encoding error";
    Enqueue(std::move(event));
  }
}

void LibxmlTreeBuilder::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  // DoEnd(): libxml2 reads the end, which may report more.
  if (context_ && !builder_.IsStopped()) {
    xmlParseChunk(context_.get(), nullptr, 0, 1);
  }
}

bool LibxmlTreeBuilder::HasSeenEndOfFile() const {
  return finished_;
}

void LibxmlTreeBuilder::Enqueue(std::unique_ptr<Event> event) {
  events_.push_back(std::move(event));
}

MarkupPosition LibxmlTreeBuilder::ContextPosition() const {
  if (!context_ || !context_->input) {
    return {0, 0};
  }
  return {context_->input->line - 1, context_->input->col - 1};
}

MarkupPosition LibxmlTreeBuilder::TextPosition() const {
  return events_.empty() ? ContextPosition() : position_;
}

Element* LibxmlTreeBuilder::Pump() {
  ended_script_ = nullptr;
  while (!events_.empty()) {
    const std::unique_ptr<Event> event = std::move(events_.front());
    events_.pop_front();
    position_ = event->position;
    if (!builder_.IsStopped()) {
      Play(*event);
    }
    if (builder_.IsStopped()) {
      // StopParsing() stops libxml2 too.
      events_.clear();
      if (context_) {
        xmlStopParser(context_.get());
      }
    }
    if (ended_script_) {
      return std::exchange(ended_script_, nullptr).get();
    }
  }
  return nullptr;
}

void LibxmlTreeBuilder::Play(Event& event) {
  using Type = Event::Type;
  switch (event.type) {
    case Type::kStartDocument:
      builder_.StartDocument(
          event.first, event.second,
          static_cast<XmlDomBuilder::Standalone>(event.number));
      return;
    case Type::kStartElement: {
      std::vector<XmlDomBuilder::ReadAttribute> attributes;
      for (Event::Attribute& attribute : event.attributes) {
        std::optional<std::u16string> uri = attribute.uri;
        std::u16string qualified_name = attribute.local_name;
        if (attribute.prefix && !attribute.prefix->empty()) {
          qualified_name = *attribute.prefix + u":" + attribute.local_name;
          // libxml2 resolves a prefix as it reads; in a fragment, one it did
          // not may be the context's.
          if (!uri) {
            uri = builder_.FragmentNamespaceOfPrefix(*attribute.prefix);
            if (!uri) {
              builder_.FailOnUndeclaredAttributePrefix(*attribute.prefix,
                                                       attribute.local_name);
              return;
            }
          }
        }
        if (uri && uri->empty()) {
          uri.reset();
        }
        attributes.push_back(
            {std::move(uri), std::move(qualified_name), attribute.value});
      }
      builder_.StartElement(
          event.first.value_or(u""),
          event.second && !event.second->empty() ? event.second : std::nullopt,
          event.third, event.namespaces, attributes, event.position);
      return;
    }
    case Type::kEndElement:
      ended_script_ = builder_.EndElement();
      return;
    case Type::kCharacters:
      builder_.Characters(*event.first);
      return;
    case Type::kProcessingInstruction:
      builder_.ProcessingInstruction(*event.first, *event.second);
      return;
    case Type::kCData:
      builder_.CData(*event.first);
      return;
    case Type::kComment:
      builder_.Comment(*event.first);
      return;
    case Type::kInternalSubset:
      builder_.DocType(*event.first, *event.second, *event.third);
      return;
    case Type::kEndDocument:
      builder_.EndDocument();
      return;
    case Type::kError:
      builder_.HandleError(static_cast<XmlErrors::Type>(event.number),
                           event.message, event.position);
      return;
  }
}

void LibxmlTreeBuilder::End() {
  builder_.End();
}

// static
bool LibxmlTreeBuilder::ParseFragment(std::u16string_view source,
                                      DocumentFragment& fragment,
                                      Element* context_element,
                                      HtmlTreeBuilderReferences& references) {
  if (source.empty()) {
    return true;
  }
  // Blink's stand-in for the HTML Standard's XML fragment parsing algorithm:
  // the markup of a script or a style element is its text.
  if (context_element && (context_element->LocalName() == u"script" ||
                          context_element->LocalName() == u"style")) {
    fragment.ParserAppendChild(
        fragment.GetDocument().CreateTextNode(std::u16string(source)));
    return true;
  }
  const std::string utf8 = base::UTF16ToUTF8(source);
  if (utf8.size() > INT_MAX) {
    return false;
  }
  LibxmlTreeBuilder builder(references, fragment, context_element);
  builder.InitializeParserContext(&utf8);
  if (!builder.context_) {
    return false;
  }
  xmlParseContent(builder.context_.get());
  // A fragment's parser never waits for a script.
  while (builder.Pump()) {
  }
  // The text that waits goes in.
  builder.builder_.EndDocument();
  // Well formed, or not but with no error, as AppendFragmentSource() judges.
  return builder.context_->wellFormed ||
         !xmlCtxtGetLastError(builder.context_.get());
}

NodeVector ParseXmlFragment(Element& context,
                            std::u16string_view markup,
                            bindings::ExceptionState& exception_state) {
  Document& document = context.GetDocument();
  const cppgc::Persistent<DocumentFragment> fragment =
      DocumentFragment::Create(document);
  const cppgc::Persistent<HtmlTreeBuilderReferences> references =
      bindings::MakeGarbageCollected<HtmlTreeBuilderReferences>(document);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  bool well_formed = false;
  v8::Local<v8::Value> thrown;
  {
    // What an attribute threw, a DOMException with the caller's context,
    // which the caller throws once this TryCatch is gone, as Blink's
    // TryRethrowScope hands it on.
    v8::TryCatch try_catch(isolate);
    {
      // The context goes on once, as the caller throws it.
      const bindings::OmitExceptionContextScope omit_context(isolate);
      well_formed = LibxmlTreeBuilder::ParseFragment(markup, *fragment,
                                                     &context, *references);
    }
    if (try_catch.HasCaught()) {
      thrown = try_catch.Exception();
      try_catch.Reset();
    }
  }
  if (!thrown.IsEmpty()) {
    exception_state.ThrowDOMException(thrown);
    return {};
  }
  if (!well_formed) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        "The provided markup is invalid XML, and therefore cannot be inserted "
        "into an XML document.");
    return {};
  }
  NodeVector nodes;
  for (Node* child = fragment->FirstChild(); child;
       child = child->NextSibling()) {
    nodes.emplace_back(child);
  }
  fragment->RemoveChildren();
  return nodes;
}

}  // namespace mitto
