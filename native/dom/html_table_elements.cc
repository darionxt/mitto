#include "mitto/dom/html_table_elements.h"

#include <optional>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_table_cell_element.h"
#include "mitto/bindings/v8_html_table_col_element.h"
#include "mitto/bindings/v8_html_table_element.h"
#include "mitto/bindings/v8_html_table_row_element.h"
#include "mitto/bindings/v8_html_table_section_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/html_elements.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_parser_idioms.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// Blink's limits of a cell's spans and a column's span.
constexpr uint32_t kDefaultSpan = 1;
constexpr uint32_t kMinColSpan = 1;
constexpr uint32_t kMaxColSpan = 1000;
constexpr uint32_t kMinRowSpan = 0;
constexpr uint32_t kMaxRowSpan = 65534;

HTMLElement* MakeHTMLElement(Document& document, std::u16string_view name) {
  return static_cast<HTMLElement*>(document.CreateElement(
      html_names::kXhtmlNamespace, std::nullopt, std::u16string(name)));
}

// The children of `parent` that are HTML elements named `name`, in order.
HTMLElement* FirstHTMLChild(const Node& parent, std::u16string_view name) {
  for (Node* child = parent.FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(name)) {
      return static_cast<HTMLElement*>(child);
    }
  }
  return nullptr;
}

HTMLElement* LastHTMLChild(const Node& parent, std::u16string_view name) {
  for (Node* child = parent.LastChild(); child;
       child = child->PreviousSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(name)) {
      return static_cast<HTMLElement*>(child);
    }
  }
  return nullptr;
}

// Blink's SetUnsignedIntegralAttribute(): a value above 2147483647 sets the
// default.
void SetSpanAttribute(Element& element,
                      std::u16string_view name,
                      uint32_t value) {
  if (value > 0x7fffffffu) {
    value = kDefaultSpan;
  }
  element.SetAttributeWithoutValidation(name, base::NumberToString16(value));
}

uint32_t SpanAttribute(const Element& element,
                       std::u16string_view name,
                       uint32_t min,
                       uint32_t max) {
  const std::u16string* value = element.FindAttribute(name);
  if (!value) {
    return kDefaultSpan;
  }
  return ParseHTMLClampedNonNegativeInteger(*value, min, max)
      .value_or(kDefaultSpan);
}

// The index of `row` among `rows`, as Blink's FindIndexInRowCollection().
int32_t IndexIn(const HTMLCollection& rows, const Element& row) {
  for (uint32_t i = 0; Element* element = rows.Item(i); ++i) {
    if (element == &row) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

std::string IndexText(int32_t index) {
  return base::NumberToString(index);
}

}  // namespace

// The table.

// static
HTMLTableElement* HTMLTableElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTableElement>(document);
}

HTMLTableElement::HTMLTableElement(Document& document)
    : HTMLElement(document, u"table") {}

const bindings::WrapperTypeInfo* HTMLTableElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLTableElement::kWrapperTypeInfo;
}

void HTMLTableElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(rows_);
  visitor->Trace(t_bodies_);
}

HTMLTableCaptionElement* HTMLTableElement::Caption() const {
  return static_cast<HTMLTableCaptionElement*>(
      FirstHTMLChild(*this, u"caption"));
}

void HTMLTableElement::SetCaption(HTMLTableCaptionElement* caption,
                                  bindings::ExceptionState& exception_state) {
  DeleteCaption();
  if (caption) {
    InsertBefore(caption, FirstChild(), exception_state);
  }
}

HTMLTableCaptionElement* HTMLTableElement::CreateCaption() {
  if (HTMLTableCaptionElement* existing = Caption()) {
    return existing;
  }
  auto* caption = static_cast<HTMLTableCaptionElement*>(
      MakeHTMLElement(GetDocument(), u"caption"));
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  SetCaption(caption, exception_state);
  return caption;
}

void HTMLTableElement::DeleteCaption() {
  if (HTMLTableCaptionElement* caption = Caption()) {
    bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
    RemoveChild(caption, exception_state);
  }
}

HTMLTableSectionElement* HTMLTableElement::FirstSection(
    std::u16string_view name) const {
  return static_cast<HTMLTableSectionElement*>(FirstHTMLChild(*this, name));
}

HTMLTableSectionElement* HTMLTableElement::THead() const {
  return FirstSection(u"thead");
}

void HTMLTableElement::SetTHead(HTMLTableSectionElement* head,
                                bindings::ExceptionState& exception_state) {
  if (head && !head->HasHTMLTagName(u"thead")) {
    exception_state.ThrowDOMException("HierarchyRequestError",
                                      "Not a thead element.");
    return;
  }
  DeleteTHead();
  if (!head) {
    return;
  }
  // Before the first child that is not a caption or a colgroup.
  Node* child = FirstChild();
  for (; child; child = child->NextSibling()) {
    if (!child->IsElementNode() ||
        !static_cast<Element*>(child)->IsHTMLElement()) {
      continue;
    }
    const auto& element = static_cast<const Element&>(*child);
    if (!element.HasHTMLTagName(u"caption") &&
        !element.HasHTMLTagName(u"colgroup")) {
      break;
    }
  }
  InsertBefore(head, child, exception_state);
}

HTMLTableSectionElement* HTMLTableElement::CreateTHead() {
  if (HTMLTableSectionElement* existing = THead()) {
    return existing;
  }
  auto* head = static_cast<HTMLTableSectionElement*>(
      MakeHTMLElement(GetDocument(), u"thead"));
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  SetTHead(head, exception_state);
  return head;
}

void HTMLTableElement::DeleteTHead() {
  if (HTMLTableSectionElement* head = THead()) {
    bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
    RemoveChild(head, exception_state);
  }
}

HTMLTableSectionElement* HTMLTableElement::TFoot() const {
  return FirstSection(u"tfoot");
}

void HTMLTableElement::SetTFoot(HTMLTableSectionElement* foot,
                                bindings::ExceptionState& exception_state) {
  if (foot && !foot->HasHTMLTagName(u"tfoot")) {
    exception_state.ThrowDOMException("HierarchyRequestError",
                                      "Not a tfoot element.");
    return;
  }
  DeleteTFoot();
  if (foot) {
    AppendChild(foot, exception_state);
  }
}

HTMLTableSectionElement* HTMLTableElement::CreateTFoot() {
  if (HTMLTableSectionElement* existing = TFoot()) {
    return existing;
  }
  auto* foot = static_cast<HTMLTableSectionElement*>(
      MakeHTMLElement(GetDocument(), u"tfoot"));
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  SetTFoot(foot, exception_state);
  return foot;
}

void HTMLTableElement::DeleteTFoot() {
  if (HTMLTableSectionElement* foot = TFoot()) {
    bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
    RemoveChild(foot, exception_state);
  }
}

HTMLCollection* HTMLTableElement::TBodies() {
  if (!t_bodies_) {
    t_bodies_ = HTMLCollection::Create(this, HTMLCollection::Type::kTBodies);
  }
  return t_bodies_.Get();
}

HTMLTableSectionElement* HTMLTableElement::LastBody() const {
  return static_cast<HTMLTableSectionElement*>(LastHTMLChild(*this, u"tbody"));
}

HTMLTableSectionElement* HTMLTableElement::CreateTBody() {
  auto* body = static_cast<HTMLTableSectionElement*>(
      MakeHTMLElement(GetDocument(), u"tbody"));
  HTMLTableSectionElement* const last_body = LastBody();
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  InsertBefore(body, last_body ? last_body->NextSibling() : nullptr,
               exception_state);
  return body;
}

HTMLCollection* HTMLTableElement::Rows() {
  if (!rows_) {
    rows_ = HTMLCollection::Create(this, HTMLCollection::Type::kTableRows);
  }
  return rows_.Get();
}

HTMLTableRowElement* HTMLTableElement::InsertRow(
    int32_t index,
    bindings::ExceptionState& exception_state) {
  if (index < -1) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat(
            {"The index provided (", IndexText(index), ") is less than -1."}));
    return nullptr;
  }
  HTMLCollection* const rows = Rows();
  const int32_t count = static_cast<int32_t>(rows->Length());
  if (index > count) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The index provided (", IndexText(index),
                      ") is greater than the number of rows in the table (",
                      IndexText(count), ")."}));
    return nullptr;
  }
  // Blink's insertRow(): before the row at the index, in its parent, or after
  // the last row; or in a new tbody when the table has no rows or bodies.
  Element* const row = index == -1 ? nullptr : rows->Item(index);
  Element* last_row = row;
  if (!row) {
    const int32_t last = index == -1 ? count - 1 : index - 1;
    last_row = last >= 0 ? rows->Item(last) : nullptr;
  }
  auto* new_row =
      static_cast<HTMLTableRowElement*>(MakeHTMLElement(GetDocument(), u"tr"));
  Node* parent = nullptr;
  if (last_row) {
    parent = last_row->ParentNode();
  } else {
    parent = LastBody();
    if (!parent) {
      HTMLElement* const new_body = MakeHTMLElement(GetDocument(), u"tbody");
      new_body->AppendChild(new_row, exception_state);
      AppendChild(new_body, exception_state);
      return new_row;
    }
  }
  parent->InsertBefore(new_row, row, exception_state);
  return new_row;
}

void HTMLTableElement::DeleteRow(int32_t index,
                                 bindings::ExceptionState& exception_state) {
  if (index < -1) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat(
            {"The index provided (", IndexText(index), ") is less than -1."}));
    return;
  }
  HTMLCollection* const rows = Rows();
  const int32_t count = static_cast<int32_t>(rows->Length());
  Element* row = nullptr;
  if (index == -1) {
    if (count == 0) {
      return;
    }
    row = rows->Item(count - 1);
  } else {
    row = rows->Item(index);
  }
  if (!row) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The index provided (", IndexText(index),
                      ") is greater than the number of rows in the table (",
                      IndexText(count), ")."}));
    return;
  }
  row->Remove(exception_state);
}

// The sections.

// static
HTMLTableSectionElement* HTMLTableSectionElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableSectionElement>(document,
                                                                 local_name);
}

HTMLTableSectionElement::HTMLTableSectionElement(
    Document& document,
    const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableSectionElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableSectionElement::kWrapperTypeInfo;
}

void HTMLTableSectionElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(rows_);
}

HTMLCollection* HTMLTableSectionElement::Rows() {
  if (!rows_) {
    rows_ = HTMLCollection::Create(this, HTMLCollection::Type::kSectionRows);
  }
  return rows_.Get();
}

HTMLElement* HTMLTableSectionElement::InsertRow(
    int32_t index,
    bindings::ExceptionState& exception_state) {
  HTMLCollection* const rows = Rows();
  const int32_t count = static_cast<int32_t>(rows->Length());
  if (index < -1 || index > count) {
    // Blink's message lacks the closing parenthesis after the index.
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The provided index (", IndexText(index),
                      " is outside the range [-1, ", IndexText(count), "]."}));
    return nullptr;
  }
  HTMLElement* const row = MakeHTMLElement(GetDocument(), u"tr");
  if (index == -1 || index == count) {
    AppendChild(row, exception_state);
  } else {
    InsertBefore(row, rows->Item(index), exception_state);
  }
  return row;
}

void HTMLTableSectionElement::DeleteRow(
    int32_t index,
    bindings::ExceptionState& exception_state) {
  HTMLCollection* const rows = Rows();
  const int32_t count = static_cast<int32_t>(rows->Length());
  if (index == -1) {
    if (count == 0) {
      return;
    }
    index = count - 1;
  }
  if (index < 0 || index >= count) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The provided index (", IndexText(index),
                      " is outside the range [-1, ", IndexText(count), "]."}));
    return;
  }
  RemoveChild(rows->Item(index), exception_state);
}

// The rows.

// static
HTMLTableRowElement* HTMLTableRowElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTableRowElement>(document);
}

HTMLTableRowElement::HTMLTableRowElement(Document& document)
    : HTMLElement(document, u"tr") {}

const bindings::WrapperTypeInfo* HTMLTableRowElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableRowElement::kWrapperTypeInfo;
}

void HTMLTableRowElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(cells_);
}

int32_t HTMLTableRowElement::RowIndex() const {
  Node* maybe_table = ParentNode();
  if (maybe_table && maybe_table->IsElementNode()) {
    const auto& parent = static_cast<const Element&>(*maybe_table);
    if (parent.HasHTMLTagName(u"thead") || parent.HasHTMLTagName(u"tbody") ||
        parent.HasHTMLTagName(u"tfoot")) {
      maybe_table = maybe_table->ParentNode();
    }
  }
  if (!maybe_table || !maybe_table->IsElementNode() ||
      !static_cast<Element*>(maybe_table)->HasHTMLTagName(u"table")) {
    return -1;
  }
  return IndexIn(*static_cast<HTMLTableElement*>(maybe_table)->Rows(), *this);
}

int32_t HTMLTableRowElement::SectionRowIndex() const {
  Node* const parent = ParentNode();
  if (!parent || !parent->IsElementNode()) {
    return -1;
  }
  auto* const element = static_cast<Element*>(parent);
  if (element->HasHTMLTagName(u"thead") || element->HasHTMLTagName(u"tbody") ||
      element->HasHTMLTagName(u"tfoot")) {
    return IndexIn(*static_cast<HTMLTableSectionElement*>(element)->Rows(),
                   *this);
  }
  if (element->HasHTMLTagName(u"table")) {
    return IndexIn(*static_cast<HTMLTableElement*>(element)->Rows(), *this);
  }
  return -1;
}

HTMLCollection* HTMLTableRowElement::Cells() {
  if (!cells_) {
    cells_ = HTMLCollection::Create(this, HTMLCollection::Type::kRowCells);
  }
  return cells_.Get();
}

HTMLElement* HTMLTableRowElement::InsertCell(
    int32_t index,
    bindings::ExceptionState& exception_state) {
  HTMLCollection* const cells = Cells();
  const int32_t count = static_cast<int32_t>(cells->Length());
  if (index < -1 || index > count) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The value provided (", IndexText(index),
                      ") is outside the range [-1, ", IndexText(count), "]."}));
    return nullptr;
  }
  HTMLElement* const cell = MakeHTMLElement(GetDocument(), u"td");
  if (index == -1 || index == count) {
    AppendChild(cell, exception_state);
  } else {
    InsertBefore(cell, cells->Item(index), exception_state);
  }
  return cell;
}

void HTMLTableRowElement::DeleteCell(
    int32_t index,
    bindings::ExceptionState& exception_state) {
  HTMLCollection* const cells = Cells();
  const int32_t count = static_cast<int32_t>(cells->Length());
  if (index < -1 || index >= count) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The value provided (", IndexText(index),
                      ") is outside the range [0, ", IndexText(count), ")."}));
    return;
  }
  if (index == -1) {
    if (count == 0) {
      return;
    }
    index = count - 1;
  }
  RemoveChild(cells->Item(index), exception_state);
}

// The cells.

// static
HTMLTableCellElement* HTMLTableCellElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableCellElement>(document,
                                                              local_name);
}

HTMLTableCellElement::HTMLTableCellElement(Document& document,
                                           const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableCellElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableCellElement::kWrapperTypeInfo;
}

uint32_t HTMLTableCellElement::ColSpan() const {
  return SpanAttribute(*this, u"colspan", kMinColSpan, kMaxColSpan);
}

void HTMLTableCellElement::SetColSpan(uint32_t value) {
  SetSpanAttribute(*this, u"colspan", value);
}

uint32_t HTMLTableCellElement::RowSpan() const {
  return SpanAttribute(*this, u"rowspan", kMinRowSpan, kMaxRowSpan);
}

void HTMLTableCellElement::SetRowSpan(uint32_t value) {
  SetSpanAttribute(*this, u"rowspan", value);
}

int32_t HTMLTableCellElement::CellIndex() const {
  const Element* const parent = ParentElement();
  if (!parent || !parent->HasHTMLTagName(u"tr")) {
    return -1;
  }
  int32_t index = 0;
  for (Node* sibling = PreviousSibling(); sibling;
       sibling = sibling->PreviousSibling()) {
    if (sibling->IsElementNode() &&
        (static_cast<Element*>(sibling)->HasHTMLTagName(u"td") ||
         static_cast<Element*>(sibling)->HasHTMLTagName(u"th"))) {
      ++index;
    }
  }
  return index;
}

// The columns.

// static
HTMLTableColElement* HTMLTableColElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableColElement>(document,
                                                             local_name);
}

HTMLTableColElement::HTMLTableColElement(Document& document,
                                         const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableColElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableColElement::kWrapperTypeInfo;
}

uint32_t HTMLTableColElement::Span() const {
  return SpanAttribute(*this, u"span", kMinColSpan, kMaxColSpan);
}

void HTMLTableColElement::SetSpan(uint32_t value) {
  SetSpanAttribute(*this, u"span", value);
}

}  // namespace mitto
