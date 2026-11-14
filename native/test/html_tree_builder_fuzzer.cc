// Feeds any markup to the tree builder, as a page's server and its scripts'
// innerHTML give it, on V8's C++ heap as the Page program has it.
//
// The first byte says how. Its low five bits pick the context element of a
// fragment, from a list that reaches every insertion mode, or a document for
// the values past the list. Its high three bits say how big the pieces are
// that a document's markup comes in. The rest is the markup, in UTF-8. A
// document must get the same tree whole as in pieces.

#include "mitto/dom/html_tree_builder.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_template_element.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-cppgc.h"
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

struct Context {
  const char16_t* namespace_uri;
  std::u16string_view local_name;
  // An attribute that the context element gets, if any.
  std::u16string_view attribute_name;
  std::u16string_view attribute_value;
};

constexpr char16_t kHtml[] = u"http://www.w3.org/1999/xhtml";

// Context elements that start a fragment in each insertion mode, and in
// each state of the tokenizer that one sets.
constexpr auto kContexts = std::to_array<Context>({
    {kHtml, u"html"},
    {kHtml, u"head"},
    {kHtml, u"body"},
    {kHtml, u"div"},
    {kHtml, u"table"},
    {kHtml, u"tbody"},
    {kHtml, u"tr"},
    {kHtml, u"td"},
    {kHtml, u"caption"},
    {kHtml, u"colgroup"},
    {kHtml, u"select"},
    {kHtml, u"template"},
    {kHtml, u"frameset"},
    {kHtml, u"title"},
    {kHtml, u"textarea"},
    {kHtml, u"script"},
    {kHtml, u"plaintext"},
    {kHtml, u"form"},
    {mitto::html_names::kSvgNamespace, u"svg"},
    {mitto::html_names::kSvgNamespace, u"foreignObject"},
    {mitto::html_names::kMathMlNamespace, u"math"},
    {mitto::html_names::kMathMlNamespace, u"mi"},
    {mitto::html_names::kMathMlNamespace, u"annotation-xml", u"encoding",
     u"text/html"},
    {mitto::html_names::kMathMlNamespace, u"annotation-xml"},
});

constexpr auto kPieceSizes =
    std::to_array<size_t>({1, 2, 3, 7, 16, 64, 512, 0});

// The tree under `root`, in html5lib's form, walked without recursion: a
// tree that the adoption agency algorithm deepens can be deep.
std::string Dump(const Node& root) {
  std::string out;
  const Node* node = root.FirstChild();
  int depth = 0;
  while (node) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
    switch (node->GetNodeType()) {
      case Node::kDocumentTypeNode:
        out += "<!DOCTYPE " +
               base::UTF16ToUTF8(
                   static_cast<const mitto::DocumentType*>(node)->Name()) +
               ">";
        break;
      case Node::kElementNode: {
        const auto* element = static_cast<const Element*>(node);
        out += "<" + base::UTF16ToUTF8(element->NamespaceURI().value_or(u"")) +
               " " + base::UTF16ToUTF8(element->LocalName());
        for (const Element::Attribute& attribute : element->attributes()) {
          out += " " +
                 base::UTF16ToUTF8(attribute.name.namespace_uri.value_or(u"")) +
                 " " + base::UTF16ToUTF8(attribute.name.ToString()) + "=\"" +
                 base::UTF16ToUTF8(attribute.value) + "\"";
        }
        out += ">";
        // A template's contents are a fragment of their own, which this walk
        // does not reach: they get a walk of their own, one level deeper for
        // each template that nests.
        if (element->HasHTMLTagName(u"template")) {
          out += "\n{" +
                 Dump(*static_cast<const mitto::HTMLTemplateElement*>(element)
                           ->Content()) +
                 "}";
        }
        break;
      }
      case Node::kTextNode:
      case Node::kCommentNode:
        out += "\"" +
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
    CHECK_EQ(node->NextSibling()->ParentNode(), node->ParentNode());
    node = node->NextSibling();
  }
  return out;
}

std::string ParseDocument(std::u16string_view markup, size_t piece_size) {
  const cppgc::Persistent<Document> document(Document::CreateHTML());
  mitto::HtmlDocumentParser parser(*document);
  while (!markup.empty()) {
    const size_t size = std::min(piece_size, markup.size());
    parser.Append(markup.substr(0, size));
    markup.remove_prefix(size);
    while (parser.Pump()) {
    }
  }
  parser.Finish();
  while (parser.Pump()) {
  }
  return Dump(*document);
}

// Sets V8 up once, as the test program does, for every input.
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
    v8::Isolate::New(params)->Enter();
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

  const size_t context_index = how & 0x1f;
  if (context_index < kContexts.size()) {
    const Context& context = kContexts[context_index];
    const cppgc::Persistent<Document> document(Document::CreateHTML());
    const cppgc::Persistent<Element> element(document->CreateElement(
        std::u16string(context.namespace_uri), std::nullopt,
        std::u16string(context.local_name)));
    if (!context.attribute_name.empty()) {
      element->SetAttributeWithoutValidation(
          context.attribute_name, std::u16string(context.attribute_value));
    }
    const cppgc::Persistent<Element> holder(
        document->CreateElement(std::u16string(kHtml), std::nullopt, u"div"));
    for (const cppgc::Persistent<Node>& node :
         mitto::ParseHtmlFragment(*element, markup)) {
      CHECK(!node->ParentNode());
      holder->ParserAppendChild(node.Get());
    }
    Dump(*holder);
  } else {
    size_t piece_size = kPieceSizes[how >> 5];
    if (piece_size == 0) {
      piece_size = markup.size() + 1;
    }
    CHECK_EQ(ParseDocument(markup, markup.size() + 1),
             ParseDocument(markup, piece_size));
  }

  // Nothing else collects the documents of earlier inputs, since no script
  // runs to allocate on the heap.
  static int runs = 0;
  if (++runs % 256 == 0) {
    v8::Isolate::GetCurrent()->LowMemoryNotification();
  }
  return 0;
}
