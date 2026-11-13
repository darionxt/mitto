// Feeds any input to the selector parser, which reads what a page's scripts
// give querySelector(), querySelectorAll(), matches() and closest().
//
// The first byte says how. Its five low bits turn on, one each, the
// runtime-enabled features that change what parses beyond Chrome's stable
// set: the media element pseudo-classes, :lang()'s extended ranges, the flag
// s of attribute selectors, ::highlight(*) and :hover after ::before. The
// next bit puts the document in quirks mode, the one after makes it a
// document that is not HTML, and the high bit says whether the rest is
// UTF-16, little-endian and with any code units, rather than UTF-8.
//
// A list that parses must have the shape the matcher relies on: a complex
// selector with a compound or more, each compound with a simple selector or
// more, a relation only between compounds, a relative relation only right
// after the anchor of :has(), and a selector list in each pseudo-class that
// takes one, empty only for the forgiving lists of :is() and :where(). The
// same input must parse the same way again.

#include "mitto/css/css_selector.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_selector_parser.h"

namespace {

using mitto::CssComplexSelector;
using mitto::CssCompoundSelector;
using mitto::CssPseudoType;
using mitto::CssRelation;
using mitto::CssSelectorList;
using mitto::CssSimpleSelector;

void CheckList(const CssSelectorList& list,
               bool may_be_empty,
               bool relative = false);

bool IsRelative(CssRelation relation) {
  return relation == CssRelation::kRelativeDescendant ||
         relation == CssRelation::kRelativeChild ||
         relation == CssRelation::kRelativeDirectAdjacent ||
         relation == CssRelation::kRelativeIndirectAdjacent;
}

void CheckSimple(const CssSimpleSelector& simple) {
  if (simple.match != CssSimpleSelector::Match::kPseudoClass &&
      simple.match != CssSimpleSelector::Match::kPseudoElement) {
    CHECK(!simple.selector_list);
    return;
  }
  CHECK(simple.pseudo_type != CssPseudoType::kUnknown);
  switch (simple.pseudo_type) {
    case CssPseudoType::kIs:
    case CssPseudoType::kWhere:
      CHECK(simple.selector_list);
      CheckList(*simple.selector_list, /*may_be_empty=*/true);
      break;
    case CssPseudoType::kHas:
      CHECK(simple.selector_list);
      CheckList(*simple.selector_list, /*may_be_empty=*/false,
                /*relative=*/true);
      break;
    case CssPseudoType::kNot:
    case CssPseudoType::kAny:
    case CssPseudoType::kHostContext:
    case CssPseudoType::kCue:
    case CssPseudoType::kSlotted:
      CHECK(simple.selector_list);
      CheckList(*simple.selector_list, /*may_be_empty=*/false);
      break;
    default:
      if (simple.selector_list) {
        CheckList(*simple.selector_list, /*may_be_empty=*/false);
      }
      break;
  }
}

// A relative selector, of :has(), starts with the anchor and a relative
// relation to it, and no other relation is relative.
void CheckComplex(const CssComplexSelector& complex, bool relative) {
  CHECK(!complex.compounds.empty());
  if (relative) {
    CHECK_GE(complex.compounds.size(), 2u);
    const CssCompoundSelector& anchor = complex.compounds.front();
    CHECK_EQ(anchor.simples.size(), 1u);
    CHECK(anchor.simples.front().pseudo_type == CssPseudoType::kRelativeAnchor);
  }
  for (size_t i = 0; i < complex.compounds.size(); ++i) {
    const CssCompoundSelector& compound = complex.compounds[i];
    CHECK(!compound.simples.empty());
    if (i == 0) {
      CHECK(compound.relation == CssRelation::kNone);
    } else if (i == 1 && relative) {
      CHECK(IsRelative(compound.relation));
    } else {
      CHECK(compound.relation != CssRelation::kNone);
      CHECK(!IsRelative(compound.relation));
    }
    for (const CssSimpleSelector& simple : compound.simples) {
      CheckSimple(simple);
    }
  }
}

void CheckList(const CssSelectorList& list, bool may_be_empty, bool relative) {
  CHECK(may_be_empty || !list.complexes.empty());
  for (const CssComplexSelector& complex : list.complexes) {
    CheckComplex(complex, relative);
  }
}

// A description of a list's shape, to compare two parses.
void Describe(const CssSelectorList& list, std::string& out) {
  out += '[';
  for (const CssComplexSelector& complex : list.complexes) {
    out += '(';
    for (const CssCompoundSelector& compound : complex.compounds) {
      out += static_cast<char>('0' + static_cast<int>(compound.relation));
      for (const CssSimpleSelector& simple : compound.simples) {
        out += static_cast<char>('a' + static_cast<int>(simple.match));
        out += std::to_string(static_cast<int>(simple.pseudo_type));
        out += base::UTF16ToUTF8(simple.name);
        out += '|';
        out += base::UTF16ToUTF8(simple.value);
        out += '|';
        out +=
            std::to_string(simple.nth_a) + ',' + std::to_string(simple.nth_b);
        for (const std::u16string& argument : simple.arguments) {
          out += base::UTF16ToUTF8(argument) + ';';
        }
        if (simple.selector_list) {
          Describe(*simple.selector_list, out);
        }
      }
    }
    out += ')';
  }
  out += ']';
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(data, size));
  if (bytes.empty()) {
    return 0;
  }
  const uint8_t how = bytes.take_first_elem();
  mitto::CssSelectorParserContext context;
  context.features.media_element_pseudos = (how & 0x01) != 0;
  context.features.lang_extended_ranges = (how & 0x02) != 0;
  context.features.case_sensitive_selector = (how & 0x04) != 0;
  context.features.highlight_universal = (how & 0x08) != 0;
  context.features.pseudo_elements_hoverable = (how & 0x10) != 0;
  context.quirks_mode = (how & 0x20) != 0;
  context.html_document = (how & 0x40) == 0;

  std::u16string input;
  if (how & 0x80) {
    while (bytes.size() >= 2) {
      const auto unit = bytes.take_first<2u>();
      input.push_back(static_cast<char16_t>(unit[0] | (unit[1] << 8)));
    }
  } else {
    input = base::UTF8ToUTF16(base::as_string_view(bytes));
  }

  const std::unique_ptr<CssSelectorList> list =
      mitto::ParseCssSelectorList(input, context);
  const std::unique_ptr<CssSelectorList> again =
      mitto::ParseCssSelectorList(input, context);
  CHECK_EQ(!list, !again);
  if (!list) {
    return 0;
  }
  CheckList(*list, /*may_be_empty=*/false);
  std::string first;
  std::string second;
  Describe(*list, first);
  Describe(*again, second);
  CHECK_EQ(first, second);
  return 0;
}
