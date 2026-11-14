// The elements of a table that have members of their own: the table, its
// sections, rows and cells, and its columns. Each finds its parts among its
// children, as Blink's do, and keeps one live collection of each kind.
//
// Upstream: third_party/blink/renderer/core/html/html_table_element.h
// Upstream: third_party/blink/renderer/core/html/html_table_element.cc
// Upstream: third_party/blink/renderer/core/html/html_table_section_element.cc
// Upstream: third_party/blink/renderer/core/html/html_table_row_element.cc
// Upstream: third_party/blink/renderer/core/html/html_table_cell_element.cc
// Upstream: third_party/blink/renderer/core/html/html_table_col_element.cc
// Upstream: third_party/blink/renderer/core/html/html_table_rows_collection.cc

#ifndef MITTO_DOM_HTML_TABLE_ELEMENTS_H_
#define MITTO_DOM_HTML_TABLE_ELEMENTS_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/html_element.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class HTMLCollection;
class HTMLTableCaptionElement;
class HTMLTableRowElement;
class HTMLTableSectionElement;

class HTMLTableElement final : public HTMLElement {
 public:
  static HTMLTableElement* Create(Document& document);

  explicit HTMLTableElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The caption is the first caption child,
  // the head and foot the first thead and tfoot children.
  HTMLTableCaptionElement* Caption() const;
  void SetCaption(HTMLTableCaptionElement* caption,
                  bindings::ExceptionState& exception_state);
  HTMLTableCaptionElement* CreateCaption();
  void DeleteCaption();
  HTMLTableSectionElement* THead() const;
  void SetTHead(HTMLTableSectionElement* head,
                bindings::ExceptionState& exception_state);
  HTMLTableSectionElement* CreateTHead();
  void DeleteTHead();
  HTMLTableSectionElement* TFoot() const;
  void SetTFoot(HTMLTableSectionElement* foot,
                bindings::ExceptionState& exception_state);
  HTMLTableSectionElement* CreateTFoot();
  void DeleteTFoot();
  HTMLCollection* TBodies();
  HTMLTableSectionElement* CreateTBody();
  // The rows of the thead children first, then those of the table and of
  // its tbody children, then those of its tfoot children, as Blink's
  // HTMLTableRowsCollection orders them.
  HTMLCollection* Rows();
  HTMLTableRowElement* InsertRow(int32_t index,
                                 bindings::ExceptionState& exception_state);
  void DeleteRow(int32_t index, bindings::ExceptionState& exception_state);

 private:
  HTMLTableSectionElement* FirstSection(std::u16string_view name) const;
  HTMLTableSectionElement* LastBody() const;

  cppgc::Member<HTMLCollection> rows_;
  cppgc::Member<HTMLCollection> t_bodies_;
};

// The element of each of tbody, tfoot, thead.
class HTMLTableSectionElement final : public HTMLElement {
 public:
  static HTMLTableSectionElement* Create(Document& document,
                                         const std::u16string& local_name);

  HTMLTableSectionElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the rows are its tr children.
  HTMLCollection* Rows();
  HTMLElement* InsertRow(int32_t index,
                         bindings::ExceptionState& exception_state);
  void DeleteRow(int32_t index, bindings::ExceptionState& exception_state);

 private:
  cppgc::Member<HTMLCollection> rows_;
};

class HTMLTableRowElement final : public HTMLElement {
 public:
  static HTMLTableRowElement* Create(Document& document);

  explicit HTMLTableRowElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The indices are -1 for a row outside a
  // table or section; the cells are its td and th children.
  int32_t RowIndex() const;
  int32_t SectionRowIndex() const;
  HTMLCollection* Cells();
  HTMLElement* InsertCell(int32_t index,
                          bindings::ExceptionState& exception_state);
  void DeleteCell(int32_t index, bindings::ExceptionState& exception_state);

 private:
  cppgc::Member<HTMLCollection> cells_;
};

// The element of each of td, th.
class HTMLTableCellElement final : public HTMLElement {
 public:
  static HTMLTableCellElement* Create(Document& document,
                                      const std::u16string& local_name);

  HTMLTableCellElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: the spans are their attributes clamped to
  // Blink's limits, 1 to 1000 columns and 0 to 65534 rows, or else 1; the
  // index is among the cells of its row, or -1 outside a row.
  uint32_t ColSpan() const;
  void SetColSpan(uint32_t value);
  uint32_t RowSpan() const;
  void SetRowSpan(uint32_t value);
  int32_t CellIndex() const;
};

// The element of each of col, colgroup.
class HTMLTableColElement final : public HTMLElement {
 public:
  static HTMLTableColElement* Create(Document& document,
                                     const std::u16string& local_name);

  HTMLTableColElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: the span attribute clamped to 1 to 1000, or
  // else 1.
  uint32_t Span() const;
  void SetSpan(uint32_t value);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TABLE_ELEMENTS_H_
