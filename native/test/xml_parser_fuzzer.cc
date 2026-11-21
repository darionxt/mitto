// Feeds any XML to the code that drives libxml2 and the Rust xml crate, as a
// page's server, DOMParser and innerHTML in an XML document give it, on V8's
// C++ heap as the Page program has it.
//
// The first byte says how. Its lowest bit picks libxml2 or the Rust xml
// crate. The next two pick a document, or a fragment of libxml2's with no
// context, an element of the HTML namespace or one that declares a prefix and
// a default namespace. The high five bits say how big the pieces are that a
// document comes in. The rest is the markup, in UTF-8. A document must get the
// same tree whole as in pieces, save what pieces change in Chrome too: where
// libxml2 places an error, and some documents of the Rust crate.

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/libxml_tree_builder.h"
#include "mitto/dom/processing_instruction.h"
#include "mitto/dom/xml_document.h"
#include "mitto/dom/xml_tree_builder.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-snapshot.h"

// The Page program's data, which the fuzzer links as the program does.
extern "C" {
extern const char mitto_icu_data[];
extern const char mitto_v8_snapshot[];
extern const uint64_t mitto_v8_snapshot_size;
}

namespace {

using mitto::Document;
using mitto::Element;
using mitto::Node;

constexpr auto kPieceSizes = std::to_array<size_t>(
    {1,   2,   3,   4,    5,    7,    8, 11, 13, 16, 17, 31, 32, 64, 100, 128,
     255, 256, 512, 1000, 1024, 4096, 0, 0,  0,  0,  0,  0,  0,  0,  0,   0});

// The tree under `root`, one node to a line, walked without recursion.
std::string Dump(const Node& root) {
  std::string out;
  const Node* node = root.FirstChild();
  int depth = 0;
  while (node) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
    switch (node->GetNodeType()) {
      case Node::kDocumentTypeNode: {
        const auto* doctype = static_cast<const mitto::DocumentType*>(node);
        out += "<!DOCTYPE " + base::UTF16ToUTF8(doctype->Name()) + " " +
               base::UTF16ToUTF8(doctype->PublicId()) + " " +
               base::UTF16ToUTF8(doctype->SystemId()) + ">";
        break;
      }
      case Node::kElementNode: {
        const auto* element = static_cast<const Element*>(node);
        out += "<" + base::UTF16ToUTF8(element->NamespaceURI().value_or(u"")) +
               " " + base::UTF16ToUTF8(element->TagQName().ToString());
        for (const Element::Attribute& attribute : element->attributes()) {
          out += " " +
                 base::UTF16ToUTF8(attribute.name.namespace_uri.value_or(u"")) +
                 " " + base::UTF16ToUTF8(attribute.name.ToString()) + "=\"" +
                 base::UTF16ToUTF8(attribute.value) + "\"";
        }
        out += ">";
        if (element->HasHTMLTagName(u"template")) {
          out += "\n{" +
                 Dump(*static_cast<const mitto::HTMLTemplateElement*>(element)
                           ->Content()) +
                 "}";
        }
        break;
      }
      case Node::kProcessingInstructionNode:
        out += "<?" +
               base::UTF16ToUTF8(
                   static_cast<const mitto::ProcessingInstruction*>(node)
                       ->Target()) +
               " " +
               base::UTF16ToUTF8(
                   static_cast<const mitto::CharacterData*>(node)->Data()) +
               "?>";
        break;
      case Node::kTextNode:
      case Node::kCdataSectionNode:
      case Node::kCommentNode:
        out += std::to_string(node->GetNodeType()) + "\"" +
               base::UTF16ToUTF8(
                   static_cast<const mitto::CharacterData*>(node)->Data()) +
               "\"";
        break;
      default:
        break;
    }
    out += "\n";
    if (node->FirstChild()) {
      CHECK_EQ(node->FirstChild()->ParentNode(), node);
      node = node->FirstChild();
      ++depth;
      continue;
    }
    while (node && node != &root && !node->NextSibling()) {
      node = node->ParentNode();
      --depth;
    }
    if (!node || node == &root) {
      break;
    }
    node = node->NextSibling();
  }
  return out;
}

struct ParsedDocument {
  std::string dump;
  // Whether the document has no error block and nothing after its root
  // element. Only such a document of the Rust crate must not change with the
  // pieces, since the crate, in Chrome as here, places an error a column or
  // two later when the token it stopped at came in two pieces (`<ro]]>` gives
  // column 4 whole, 5 or 6 split inside `]]>`), and ends the document at a
  // piece that ends just after the root element, dropping what follows.
  bool pieces_leave_alone;
};

// `dump` with the line and column of each error in its block left out. libxml2
// places an error where its input ends when it stopped there, so where a piece
// ended, in Chrome as here: a processing instruction that never ends in the
// internal subset, which libxml2 reads once the subset's `]>` has come, is an
// error at column 51 of its document whole, and at column 40, just after `]>`,
// when `]>` ended a piece.
std::string WithoutErrorPlaces(std::string_view dump) {
  std::string out;
  while (!dump.empty()) {
    size_t mark = std::string_view::npos;
    size_t mark_size = 0;
    for (const std::string_view candidate : {" on line ", " at column "}) {
      const size_t at = dump.find(candidate);
      if (at < mark) {
        mark = at;
        mark_size = candidate.size();
      }
    }
    if (mark == std::string_view::npos) {
      out += dump;
      break;
    }
    out += dump.substr(0, mark + mark_size);
    dump.remove_prefix(mark + mark_size);
    size_t digits = 0;
    while (digits < dump.size() && base::IsAsciiDigit(dump[digits])) {
      ++digits;
    }
    out += '#';
    dump.remove_prefix(digits);
  }
  return out;
}

// A document of no window, whose scripts never run, so no reader gives the
// parser a script to wait for.
ParsedDocument ParseDocument(std::u16string_view markup,
                             size_t piece_size,
                             bool libxml) {
  const cppgc::Persistent<Document> document(
      mitto::XMLDocument::Create(u"application/xml"));
  const cppgc::Persistent<mitto::HtmlTreeBuilderReferences> references(
      mitto::bindings::MakeGarbageCollected<mitto::HtmlTreeBuilderReferences>(
          *document));
  std::unique_ptr<mitto::XmlReader> reader;
  if (libxml) {
    reader = std::make_unique<mitto::LibxmlTreeBuilder>(
        *references, /*scripts_already_started=*/true);
  } else {
    reader = std::make_unique<mitto::XmlTreeBuilder>(
        *references, /*scripts_already_started=*/true);
  }
  reader->Append(u"");
  while (!markup.empty()) {
    const size_t size = std::min(piece_size, markup.size());
    reader->Append(markup.substr(0, size));
    markup.remove_prefix(size);
    CHECK(!reader->Pump());
  }
  reader->Finish();
  CHECK(!reader->Pump());
  reader->End();
  std::string dump = Dump(*document);
  const bool pieces_leave_alone =
      document->LastChild() == document->DocumentElement() &&
      dump.find(" parsererror") == std::string::npos;
  return {std::move(dump), pieces_leave_alone};
}

void ParseFragment(std::u16string_view markup, int context_kind) {
  const cppgc::Persistent<Document> document(
      mitto::XMLDocument::Create(u"application/xhtml+xml"));
  cppgc::Persistent<Element> context;
  if (context_kind >= 1) {
    context = document->CreateElement(
        std::u16string(mitto::html_names::kXhtmlNamespace), std::nullopt,
        u"div");
  }
  if (context_kind == 2) {
    context->SetAttributeWithoutValidation(
        mitto::QualifiedName{
            u"xmlns", u"p", std::u16string(mitto::html_names::kXmlnsNamespace)},
        u"urn:p");
    context->SetAttributeWithoutValidation(
        mitto::QualifiedName{
            std::nullopt, u"xmlns",
            std::u16string(mitto::html_names::kXmlnsNamespace)},
        u"");
  }
  const cppgc::Persistent<mitto::DocumentFragment> fragment(
      mitto::DocumentFragment::Create(*document));
  const cppgc::Persistent<mitto::HtmlTreeBuilderReferences> references(
      mitto::bindings::MakeGarbageCollected<mitto::HtmlTreeBuilderReferences>(
          *document));
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::TryCatch try_catch(isolate);
  mitto::LibxmlTreeBuilder::ParseFragment(markup, *fragment, context.Get(),
                                          *references);
  Dump(*fragment);
}

// Sets V8 up once, as the test program does, for every input, with a context
// for the DOMExceptions that a fragment's attributes throw.
void InitializeV8() {
  [[maybe_unused]] static const bool initialized = [] {
    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(mitto_icu_data, &status);
    CHECK(U_SUCCESS(status));
    static v8::StartupData snapshot = {
        mitto_v8_snapshot, base::checked_cast<int>(mitto_v8_snapshot_size)};
    v8::V8::SetSnapshotDataBlob(&snapshot);
    // V8 keeps the platform for as long as the process runs.
    v8::Platform* const platform = v8::platform::NewDefaultPlatform().release();
    v8::V8::InitializePlatform(platform);
    CHECK(v8::V8::Initialize());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator_shared =
        std::shared_ptr<v8::ArrayBuffer::Allocator>(
            v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    params.cpp_heap =
        v8::CppHeap::Create(platform, v8::CppHeapCreateParams({})).release();
    v8::Isolate* const isolate = v8::Isolate::New(params);
    isolate->Enter();
    static v8::HandleScope handle_scope(isolate);
    v8::Context::New(isolate)->Enter();
    // The DOMExceptions that a fragment's attributes throw, as plain errors,
    // with the stable set of features, which picks the XML parsers too.
    static const std::unique_ptr<mitto::bindings::PerIsolateData> data =
        mitto::bindings::PerIsolateData::Create(
            isolate, mitto::bindings::RuntimeFeatureSet::Stable());
    data->SetDOMExceptionHooks(
        {.create = [](v8::Isolate* isolate, std::string_view name,
                      std::string_view message) -> v8::Local<v8::Value> {
          return v8::Exception::Error(
              mitto::bindings::ToV8StringFromUtf8(isolate, message));
        }});
    return true;
  }();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  InitializeV8();
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(data, size));
  if (bytes.empty()) {
    return 0;
  }
  const uint8_t how = bytes.take_first_elem();
  const std::u16string markup = base::UTF8ToUTF16(base::as_string_view(bytes));
  const bool libxml = (how & 1) == 0;
  const int mode = (how >> 1) & 3;
  if (mode == 0 || !libxml) {
    size_t piece_size = kPieceSizes[how >> 3];
    if (piece_size == 0) {
      piece_size = markup.size() + 1;
    }
    const ParsedDocument whole =
        ParseDocument(markup, markup.size() + 1, libxml);
    const ParsedDocument in_pieces = ParseDocument(markup, piece_size, libxml);
    if (libxml) {
      CHECK_EQ(WithoutErrorPlaces(whole.dump),
               WithoutErrorPlaces(in_pieces.dump));
    } else if (whole.pieces_leave_alone) {
      CHECK_EQ(whole.dump, in_pieces.dump);
    }
  } else {
    ParseFragment(markup, mode - 1);
  }

  // Nothing else collects the documents of earlier inputs, since no script
  // runs to allocate on the heap.
  static int runs = 0;
  if (++runs % 256 == 0) {
    v8::Isolate::GetCurrent()->LowMemoryNotification();
  }
  return 0;
}
