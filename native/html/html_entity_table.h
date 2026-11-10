// The HTML Standard's table of named character references, such as "amp;":
// https://html.spec.whatwg.org/multipage/named-characters.html
// make_html_entity_table.py writes it into the build from Blink's copy of
// the table, html_entity_names.csv.

#ifndef MITTO_HTML_HTML_ENTITY_TABLE_H_
#define MITTO_HTML_HTML_ENTITY_TABLE_H_

#include <string_view>

#include "base/containers/span.h"

namespace mitto {

struct HtmlEntity {
  // Without its ampersand, and with its semicolon if it has one. Every name
  // is ASCII letters and digits, and a semicolon can only end it.
  std::string_view name;
  // The one or two code points it stands for. `second` is 0 for one.
  char32_t first;
  char32_t second;
};

// Every entry, sorted by name in byte order.
base::span<const HtmlEntity> HtmlEntityTable();

}  // namespace mitto

#endif  // MITTO_HTML_HTML_ENTITY_TABLE_H_
