// A module specifier parsed once, as Blink keeps it for module resolution and
// import maps: a URL, a bare specifier kept as it was written, or neither.
// Import maps handle specifiers mostly as strings and only now and then as
// URLs, so the one parse serves both the map's keys and the URL to fetch.
//
// Without an import map, a bare specifier is a resolution error, which callers
// report; a ParsedSpecifier of a URL is then the HTML Standard's result of
// resolving a module specifier.
//
// Upstream: third_party/blink/renderer/core/script/parsed_specifier.h
// Upstream: third_party/blink/renderer/core/script/parsed_specifier.cc

#ifndef MITTO_DOM_PARSED_SPECIFIER_H_
#define MITTO_DOM_PARSED_SPECIFIER_H_

#include <string>
#include <string_view>

#include "url/gurl.h"

namespace mitto {

class ParsedSpecifier final {
 public:
  enum class Type { kInvalid, kBare, kURL };

  // Parses `specifier`, bare or not, against `base_url`, as the HTML
  // Standard's resolve a module specifier and parse a URL-like import
  // specifier do, except that a bare specifier is kept rather than rejected.
  // An absolute URL parses on its own; one that starts with "/", "./" or
  // "../" parses against `base_url`; anything else is bare, but the empty
  // string, which is invalid.
  static ParsedSpecifier Create(std::u16string_view specifier,
                                const GURL& base_url);

  Type GetType() const { return type_; }
  bool IsValid() const { return type_ != Type::kInvalid; }

  // The string that keys an import map's entries: the bare specifier itself,
  // the URL serialized, or the empty string for an invalid specifier.
  std::u16string GetImportMapKeyString() const;

  // The URL for kURL, an empty GURL otherwise.
  const GURL& GetUrl() const { return url_; }

 private:
  ParsedSpecifier(Type type, GURL url, std::u16string bare_specifier);

  Type type_;
  GURL url_;
  std::u16string bare_specifier_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PARSED_SPECIFIER_H_
