#include "mitto/dom/parsed_specifier.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "mitto/encoding/url_with_encoding.h"

namespace mitto {

// https://html.spec.whatwg.org/#resolve-a-module-specifier and
// https://html.spec.whatwg.org/#parse-a-url-like-import-specifier. This gives
// kBare where those return failure or null, which callers handle as fits
// their context.
ParsedSpecifier ParsedSpecifier::Create(std::u16string_view specifier,
                                        const GURL& base_url) {
  // The specifier parsed with no base URL, Blink's KURL(NullURL(),
  // specifier): an absolute URL is the result. Whether its scheme is a fetch
  // scheme is for the callers to check.
  GURL url = ResolveUrlWithEncoding(GURL(), specifier, std::string_view());
  if (url.is_valid()) {
    return ParsedSpecifier(Type::kURL, std::move(url), std::u16string());
  }

  // A specifier that does not start with "/", "./" or "../" is bare, but the
  // empty string, which is no valid bare specifier either: an import map's
  // normalization of a specifier key turns it down.
  if (!specifier.starts_with(u'/') && !specifier.starts_with(u"./") &&
      !specifier.starts_with(u"../")) {
    if (specifier.empty()) {
      return ParsedSpecifier(Type::kInvalid, GURL(), std::u16string());
    }
    return ParsedSpecifier(Type::kBare, GURL(), std::u16string(specifier));
  }

  // The specifier parsed against the base URL, Blink's KURL(base_url,
  // specifier), with the query in UTF-8. Blink expects a valid base here; an
  // invalid one resolves nothing, as a KURL with an invalid base does.
  GURL absolute_url =
      ResolveUrlWithEncoding(base_url, specifier, std::string_view());
  if (absolute_url.is_valid()) {
    return ParsedSpecifier(Type::kURL, std::move(absolute_url),
                           std::u16string());
  }
  return ParsedSpecifier(Type::kInvalid, GURL(), std::u16string());
}

ParsedSpecifier::ParsedSpecifier(Type type,
                                 GURL url,
                                 std::u16string bare_specifier)
    : type_(type),
      url_(std::move(url)),
      bare_specifier_(std::move(bare_specifier)) {}

std::u16string ParsedSpecifier::GetImportMapKeyString() const {
  switch (type_) {
    case Type::kInvalid:
      return std::u16string();
    case Type::kBare:
      return bare_specifier_;
    case Type::kURL:
      return base::UTF8ToUTF16(url_.spec());
  }
}

}  // namespace mitto
