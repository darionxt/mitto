#include "mitto/dom/xml_tree_builder.h"

#include <utility>

#include "base/containers/span.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_tree_builder.h"

namespace mitto {

namespace {

std::u16string ToUtf16(const rust::String& string) {
  return base::UTF8ToUTF16(std::string_view(string.data(), string.size()));
}

}  // namespace

XmlTreeBuilder::XmlTreeBuilder(HtmlTreeBuilderReferences& references,
                               bool scripts_already_started)
    : builder_(references,
               /*fragment=*/nullptr,
               /*context_element=*/nullptr,
               scripts_already_started),
      read_state_(xml_ffi::create_read_state(*this)) {}

XmlTreeBuilder::~XmlTreeBuilder() = default;

void XmlTreeBuilder::Append(std::u16string_view text) {
  if (builder_.IsStopped() || saw_end_document_) {
    return;
  }
  // What came may end what the reader took for an unfinished end.
  if (!text.empty()) {
    carried_error_.reset();
  }
  // Lone surrogates become U+FFFD, so that the reader makes the best of the
  // rest (XMLParserReplaceLoneSurrogates).
  const std::string utf8 = base::UTF16ToUTF8(text);
  const base::span<const uint8_t> bytes = base::as_byte_span(utf8);
  xml_ffi::append_to_source(
      *read_state_, rust::Slice<const uint8_t>(bytes.data(), bytes.size()));
}

void XmlTreeBuilder::Finish() {
  finish_called_ = true;
}

bool XmlTreeBuilder::HasSeenEndOfFile() const {
  return finish_called_;
}

Element* XmlTreeBuilder::Pump() {
  ended_script_ = nullptr;
  while (!builder_.IsStopped() && !saw_end_document_ &&
         !xml_ffi::saw_error(*read_state_)) {
    xml_ffi::process_next_event(*read_state_);
    if (ended_script_) {
      return std::exchange(ended_script_, nullptr).get();
    }
  }
  if (!xml_ffi::saw_error(*read_state_)) {
    return nullptr;
  }
  rust::String message;
  uint64_t row = 0;
  uint64_t column = 0;
  MarkupPosition position{-2, -2};
  if (xml_ffi::try_get_error_details(*read_state_, message, row, column)) {
    position = {static_cast<int>(row), static_cast<int>(column)};
  }
  std::string text(message.data(), message.size());
  if (xml_ffi::is_error_resumable(*read_state_) && !finish_called_) {
    // The end of what has come so far, inside the root element: an error
    // only if no more comes.
    carried_error_ = CarriedError{std::move(text), position};
    xml_ffi::reset_error(*read_state_);
  } else {
    builder_.HandleError(XmlErrors::Type::kFatal, text, position);
  }
  return nullptr;
}

void XmlTreeBuilder::End() {
  if (carried_error_ && !builder_.IsStopped()) {
    const CarriedError error = *std::exchange(carried_error_, std::nullopt);
    builder_.HandleError(XmlErrors::Type::kFatal, error.message,
                         error.position);
  }
  builder_.End();
}

MarkupPosition XmlTreeBuilder::TextPosition() const {
  uint64_t row = 0;
  uint64_t column = 0;
  if (!xml_ffi::try_get_last_event_position(*read_state_, row, column)) {
    return {-1, -1};
  }
  return {static_cast<int>(row), static_cast<int>(column)};
}

void XmlTreeBuilder::StartDocument(rust::Str version,
                                   rust::Str encoding,
                                   xml_ffi::StandaloneInfo standalone_info) {
  builder_.StartDocument(
      xml_ffi::ToUtf16(version),
      encoding.size()
          ? std::optional<std::u16string>(xml_ffi::ToUtf16(encoding))
          : std::nullopt,
      static_cast<XmlDomBuilder::Standalone>(standalone_info));
}

void XmlTreeBuilder::ProcessingInstruction(rust::Str target, rust::Str data) {
  builder_.ProcessingInstruction(xml_ffi::ToUtf16(target),
                                 xml_ffi::ToUtf16(data));
}

void XmlTreeBuilder::StartElementNs(rust::Str local_name,
                                    bool has_prefix,
                                    rust::Str prefix,
                                    bool has_ns,
                                    rust::Str ns,
                                    xml_ffi::AttributesIterator& attributes,
                                    xml_ffi::NamespacesIterator& namespaces) {
  if (builder_.IsStopped()) {
    return;
  }
  std::vector<XmlDomBuilder::NamespaceDeclaration> declarations;
  rust::String declared_prefix;
  rust::String declared_uri;
  while (xml_ffi::namespaces_next(namespaces, declared_prefix, declared_uri)) {
    declarations.push_back(
        {declared_prefix.size()
             ? std::optional<std::u16string>(ToUtf16(declared_prefix))
             : std::nullopt,
         ToUtf16(declared_uri)});
  }
  std::vector<XmlDomBuilder::ReadAttribute> read_attributes;
  xml_ffi::AttributeView attribute;
  while (xml_ffi::attributes_next(attributes, attribute)) {
    read_attributes.push_back(
        {attribute.attr_ns.empty()
             ? std::nullopt
             : std::optional<std::u16string>(attribute.attr_ns),
         attribute.q_name, attribute.value});
  }
  const std::u16string element_prefix = xml_ffi::ToUtf16(prefix);
  builder_.StartElement(
      xml_ffi::ToUtf16(local_name),
      has_prefix && !element_prefix.empty()
          ? std::optional<std::u16string>(element_prefix)
          : std::nullopt,
      has_ns && ns.size() ? std::optional<std::u16string>(xml_ffi::ToUtf16(ns))
                          : std::nullopt,
      declarations, read_attributes, TextPosition());
}

void XmlTreeBuilder::EndElementNs(rust::Str local_name,
                                  rust::Str prefix,
                                  rust::Str ns) {
  ended_script_ = builder_.EndElement();
}

void XmlTreeBuilder::Characters(rust::Str characters) {
  builder_.Characters(xml_ffi::ToUtf16(characters));
}

void XmlTreeBuilder::CData(rust::Str data) {
  builder_.CData(xml_ffi::ToUtf16(data));
}

void XmlTreeBuilder::Comment(rust::Str comment) {
  builder_.Comment(xml_ffi::ToUtf16(comment));
}

void XmlTreeBuilder::DocType(rust::Str name,
                             rust::Str public_id,
                             rust::Str system_id) {
  if (builder_.IsStopped()) {
    return;
  }
  const std::u16string public_identifier = xml_ffi::ToUtf16(public_id);
  // The HTML Standard's named character references for XHTML's DTDs.
  if (MatchesXhtmlSubsetDtd(public_identifier)) {
    xml_ffi::add_xhtml_entities(*read_state_);
  }
  builder_.DocType(xml_ffi::ToUtf16(name), public_identifier,
                   xml_ffi::ToUtf16(system_id));
}

void XmlTreeBuilder::EndDocument() {
  builder_.EndDocument();
  carried_error_.reset();
  // Neither a document of no window nor an SVG document shows Blink's XML
  // tree viewer, which the XML of a navigation can.
  saw_end_document_ = true;
}

}  // namespace mitto
