#include "mitto/dom/import_map.h"

#include <stddef.h>

#include <cmath>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/parsed_specifier.h"
#include "mitto/dom/pending_script.h"
#include "mitto/encoding/url_with_encoding.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// A JSON value as far as an import map looks into it: its type, the text of
// a string, and the members of an object. Blink's JSONValue keeps numbers,
// booleans and the elements of arrays too, which an import map never reads.
class JsonValue {
 public:
  enum class Type { kNull, kBoolean, kNumber, kString, kObject, kArray };

  using Members =
      std::vector<std::pair<std::u16string, std::unique_ptr<JsonValue>>>;

  explicit JsonValue(Type type) : type_(type) {}
  explicit JsonValue(std::u16string string)
      : type_(Type::kString), string_(std::move(string)) {}
  JsonValue(const JsonValue&) = delete;
  JsonValue& operator=(const JsonValue&) = delete;

  Type type() const { return type_; }

  // The text of a string.
  const std::u16string& string() const { return string_; }

  // The members of an object, in the order their keys first came in.
  const Members& members() const { return members_; }

  // The value of the member `key` of an object, or null, as Blink's
  // JSONObject::Get().
  const JsonValue* Get(std::u16string_view key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? nullptr : members_[it->second].second.get();
  }

  // Blink's JSONObject::SetValue(): a key that comes again keeps the place it
  // first came in and takes the new value.
  void Set(std::u16string key, std::unique_ptr<JsonValue> value) {
    const auto [it, inserted] = index_.try_emplace(key, members_.size());
    if (inserted) {
      members_.emplace_back(std::move(key), std::move(value));
    } else {
      members_[it->second].second = std::move(value);
    }
  }

 private:
  const Type type_;
  const std::u16string string_;
  Members members_;
  // Where in `members_` each key is.
  std::map<std::u16string, size_t, std::less<>> index_;
};

// Whether `text` has a surrogate that is not half of a pair, which Blink's
// strict conversion to UTF-8 turns down.
bool HasUnpairedSurrogate(std::u16string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = text[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
          text[i + 1] <= 0xDFFF) {
        ++i;
        continue;
      }
      return true;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
      return true;
    }
  }
  return false;
}

// The value of a hexadecimal digit, or -1.
int HexDigitValue(char16_t c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Blink's JSON parser as ParseJSON() runs it, with no comments and nesting
// up to 1000 deep. It differs from RFC 8259 where Blink's does: "\v" is an
// escape, and a string with a lone surrogate, escaped or not, is no JSON.
// Only whether the text is JSON matters to an import map, not where it is
// not, so this keeps no line or column.
class JsonParser {
 public:
  explicit JsonParser(std::u16string_view text) : text_(text) {}
  JsonParser(const JsonParser&) = delete;
  JsonParser& operator=(const JsonParser&) = delete;

  // The value of the whole text, or null if it is not JSON.
  std::unique_ptr<JsonValue> Parse() {
    if (text_.empty()) {
      return nullptr;
    }
    std::unique_ptr<JsonValue> value = BuildValue(kMaxStackLimit);
    if (!value || pos_ != text_.size()) {
      return nullptr;
    }
    return value;
  }

 private:
  // Blink's kMaxStackLimit.
  static constexpr int kMaxStackLimit = 1000;

  bool AtEnd() const { return pos_ == text_.size(); }

  // JSON's whitespace, Blink's SkipWhitespaceAndComments() with comments
  // disallowed.
  void SkipWhitespace() {
    while (!AtEnd()) {
      const char16_t c = text_[pos_];
      if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        return;
      }
      ++pos_;
    }
  }

  // Blink's ParseConstToken().
  bool ConsumeLiteral(std::u16string_view literal) {
    if (!text_.substr(pos_).starts_with(literal)) {
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  // Blink's ReadInt().
  bool ReadInt(bool can_have_leading_zeros) {
    const size_t start = pos_;
    while (!AtEnd() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    const size_t length = pos_ - start;
    if (!length) {
      return false;
    }
    return can_have_leading_zeros || length == 1 || text_[start] != '0';
  }

  // Blink's ParseNumberToken(), [minus] int [frac] [exp], and its check that
  // the number is finite, which turns down one like 1e400.
  bool ParseNumber() {
    const size_t start = pos_;
    if (text_[pos_] == '-') {
      ++pos_;
    }
    if (!ReadInt(/*can_have_leading_zeros=*/false)) {
      return false;
    }
    if (!AtEnd() && text_[pos_] == '.') {
      ++pos_;
      if (!ReadInt(/*can_have_leading_zeros=*/true)) {
        return false;
      }
    }
    if (!AtEnd() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (!AtEnd() && (text_[pos_] == '-' || text_[pos_] == '+')) {
        ++pos_;
      }
      if (!ReadInt(/*can_have_leading_zeros=*/true)) {
        return false;
      }
    }
    double value = 0;
    return base::StringToDouble(text_.substr(start, pos_ - start), &value) &&
           std::isfinite(value);
  }

  // Blink's ParseStringToken() and DecodeString(), at the opening quote: the
  // text of the string, or nullopt. A control character must be escaped, and
  // "\x", which Blink's tokenizer lets through, its decoder turns down.
  std::optional<std::u16string> ParseString() {
    ++pos_;
    std::u16string result;
    while (!AtEnd()) {
      char16_t c = text_[pos_++];
      if (c == '"') {
        if (HasUnpairedSurrogate(result)) {
          return std::nullopt;
        }
        return result;
      }
      if (c < 0x20) {
        return std::nullopt;
      }
      if (c != '\\') {
        result += c;
        continue;
      }
      if (AtEnd()) {
        return std::nullopt;
      }
      c = text_[pos_++];
      switch (c) {
        case '"':
        case '/':
        case '\\':
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'v':
          c = '\v';
          break;
        case 'u': {
          if (text_.size() - pos_ < 4) {
            return std::nullopt;
          }
          int code_unit = 0;
          for (int i = 0; i < 4; ++i) {
            const int digit = HexDigitValue(text_[pos_++]);
            if (digit < 0) {
              return std::nullopt;
            }
            code_unit = code_unit * 16 + digit;
          }
          c = static_cast<char16_t>(code_unit);
          break;
        }
        default:
          return std::nullopt;
      }
      result += c;
    }
    return std::nullopt;
  }

  // The elements of an array, after its '['. Blink keeps them; an import map
  // only needs them to be JSON.
  bool ParseArrayElements(int max_depth) {
    SkipWhitespace();
    if (!AtEnd() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      if (!BuildValue(max_depth - 1) || AtEnd()) {
        return false;
      }
      const char16_t c = text_[pos_++];
      if (c == ']') {
        return true;
      }
      // A ']' right after a ',' is not a value, which the next element turns
      // down.
      if (c != ',') {
        return false;
      }
    }
  }

  // The members of an object, after its '{'.
  bool ParseObjectMembers(int max_depth, JsonValue& object) {
    SkipWhitespace();
    if (!AtEnd() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      // A '}' right after a ',' is not a key either.
      SkipWhitespace();
      if (AtEnd() || text_[pos_] != '"') {
        return false;
      }
      std::optional<std::u16string> key = ParseString();
      if (!key) {
        return false;
      }
      SkipWhitespace();
      if (AtEnd() || text_[pos_] != ':') {
        return false;
      }
      ++pos_;
      std::unique_ptr<JsonValue> value = BuildValue(max_depth - 1);
      if (!value) {
        return false;
      }
      object.Set(std::move(*key), std::move(value));
      if (AtEnd()) {
        return false;
      }
      const char16_t c = text_[pos_++];
      if (c == '}') {
        return true;
      }
      if (c != ',') {
        return false;
      }
    }
  }

  // Blink's BuildValue(): the value at the cursor, and the whitespace after
  // it, or null.
  std::unique_ptr<JsonValue> BuildValue(int max_depth) {
    if (max_depth == 0) {
      return nullptr;
    }
    SkipWhitespace();
    if (AtEnd()) {
      return nullptr;
    }
    std::unique_ptr<JsonValue> result;
    switch (text_[pos_]) {
      case 'n':
        if (!ConsumeLiteral(u"null")) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(JsonValue::Type::kNull);
        break;
      case 't':
        if (!ConsumeLiteral(u"true")) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(JsonValue::Type::kBoolean);
        break;
      case 'f':
        if (!ConsumeLiteral(u"false")) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(JsonValue::Type::kBoolean);
        break;
      case '[':
        ++pos_;
        if (!ParseArrayElements(max_depth)) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(JsonValue::Type::kArray);
        break;
      case '{':
        ++pos_;
        result = std::make_unique<JsonValue>(JsonValue::Type::kObject);
        if (!ParseObjectMembers(max_depth, *result)) {
          return nullptr;
        }
        break;
      case '"': {
        std::optional<std::u16string> string = ParseString();
        if (!string) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(std::move(*string));
        break;
      }
      case '-':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        if (!ParseNumber()) {
          return nullptr;
        }
        result = std::make_unique<JsonValue>(JsonValue::Type::kNumber);
        break;
      default:
        return nullptr;
    }
    SkipWhitespace();
    return result;
  }

  const std::u16string_view text_;
  size_t pos_ = 0;
};

using MatchResult = ImportMap::SpecifierMap::const_iterator;

// Blink's KURL(base, relative): `relative` parsed against `base`, with the
// query in UTF-8. An invalid base counts as none.
GURL ParseUrl(const GURL& base, std::u16string_view relative) {
  return ResolveUrlWithEncoding(base, relative, std::string_view());
}

// The URL Standard's special schemes, Blink's SchemeRegistry::
// IsSpecialScheme().
bool IsSpecialScheme(const GURL& url) {
  return url.SchemeIs("ftp") || url.SchemeIs("file") || url.SchemeIs("http") ||
         url.SchemeIs("https") || url.SchemeIs("ws") || url.SchemeIs("wss");
}

// Blink's String::EncodeForDebugging(): `text` quoted, with tab, line feed,
// carriage return, quote and backslash escaped, and each other character
// that is not printable ASCII as "\u" and four uppercase hex digits.
void AppendEncodedForDebugging(std::string& builder, std::u16string_view text) {
  builder += '"';
  for (const char16_t c : text) {
    switch (c) {
      case '\t':
        builder += "\\t";
        break;
      case '\n':
        builder += "\\n";
        break;
      case '\r':
        builder += "\\r";
        break;
      case '"':
        builder += "\\\"";
        break;
      case '\\':
        builder += "\\\\";
        break;
      default:
        if (c >= 0x20 && c <= 0x7E) {
          builder += static_cast<char>(c);
        } else {
          base::StringAppendF(&builder, "\\u%04X", static_cast<unsigned>(c));
        }
        break;
    }
  }
  builder += '"';
}

void AppendSpecifierMapForTesting(
    std::string& builder,
    const ImportMap::SpecifierMap& specifier_map) {
  builder += '{';
  bool is_first_key = true;
  for (const auto& [key, url] : specifier_map) {
    if (!is_first_key) {
      builder += ',';
    }
    is_first_key = false;
    AppendEncodedForDebugging(builder, key);
    builder += ':';
    if (url.is_valid()) {
      AppendEncodedForDebugging(builder, base::UTF8ToUTF16(url.spec()));
    } else {
      builder += "null";
    }
  }
  builder += '}';
}

void AddIgnoredKeyMessage(ImportMap::Warn warn,
                          std::u16string_view key,
                          std::string_view reason) {
  warn(base::StrCat({"Ignored an import map key \"", base::UTF16ToUTF8(key),
                     "\": ", reason}));
}

void AddIgnoredValueMessage(ImportMap::Warn warn,
                            std::u16string_view key,
                            std::string_view reason) {
  warn(base::StrCat({"Ignored an import map value of \"",
                     base::UTF16ToUTF8(key), "\": ", reason}));
}

// https://html.spec.whatwg.org/C#normalizing-a-specifier-key, with the empty
// string for null.
std::u16string NormalizeSpecifierKey(const std::u16string& key_string,
                                     const GURL& base_url,
                                     ImportMap::Warn warn) {
  // Step 1.
  if (key_string.empty()) {
    AddIgnoredKeyMessage(warn, key_string,
                         "specifier keys cannot be the empty string.");
    return std::u16string();
  }

  // Step 2: parse a URL-like import specifier.
  const ParsedSpecifier key = ParsedSpecifier::Create(key_string, base_url);
  switch (key.GetType()) {
    case ParsedSpecifier::Type::kInvalid:
    case ParsedSpecifier::Type::kBare:
      // Step 4.
      return key_string;
    case ParsedSpecifier::Type::kURL:
      // Step 3: the serialization of the URL.
      return key.GetImportMapKeyString();
  }
}

// Steps 2.4 to 2.7 of
// https://html.spec.whatwg.org/C#sorting-and-normalizing-a-module-specifier-map:
// the address of `key`, or an invalid GURL for null.
GURL NormalizeValue(const std::u16string& key,
                    const std::u16string& value_string,
                    const GURL& base_url,
                    ImportMap::Warn warn) {
  // Step 2.4.
  const ParsedSpecifier value = ParsedSpecifier::Create(value_string, base_url);
  switch (value.GetType()) {
    case ParsedSpecifier::Type::kInvalid:
      // Step 2.5.
      AddIgnoredValueMessage(
          warn, key,
          base::StrCat({"Invalid URL: ", base::UTF16ToUTF8(value_string)}));
      return GURL();
    case ParsedSpecifier::Type::kBare:
      AddIgnoredValueMessage(
          warn, key,
          base::StrCat({"Bare specifier: ", base::UTF16ToUTF8(value_string)}));
      return GURL();
    case ParsedSpecifier::Type::kURL:
      // Step 2.6: a key that ends in a slash needs an address that does too.
      if (key.ends_with(u'/') && !value.GetUrl().spec().ends_with('/')) {
        AddIgnoredValueMessage(
            warn, key,
            base::StrCat(
                {"Since specifierKey ended in a slash, so must the address: ",
                 base::UTF16ToUTF8(value_string)}));
        return GURL();
      }
      DCHECK(value.GetUrl().is_valid());
      return value.GetUrl();
  }
}

// https://html.spec.whatwg.org/C#sorting-and-normalizing-a-module-specifier-map
ImportMap::SpecifierMap SortAndNormalizeSpecifierMap(const JsonValue& imports,
                                                     const GURL& base_url,
                                                     ImportMap::Warn warn) {
  // Step 1.
  ImportMap::SpecifierMap normalized;

  // Step 2.
  for (const auto& [specifier_key, value] : imports.members()) {
    // Step 2.1.
    std::u16string normalized_specifier_key =
        NormalizeSpecifierKey(specifier_key, base_url, warn);

    // Step 2.2.
    if (normalized_specifier_key.empty()) {
      continue;
    }

    if (value->type() == JsonValue::Type::kString) {
      // Steps 2.4 to 2.6. A later key that normalizes the same replaces an
      // earlier one.
      normalized.insert_or_assign(
          std::move(normalized_specifier_key),
          NormalizeValue(specifier_key, value->string(), base_url, warn));
      continue;
    }

    // Step 2.3: an address that is not a string is null.
    AddIgnoredValueMessage(warn, specifier_key, "Invalid value type.");
    normalized.insert_or_assign(std::move(normalized_specifier_key), GURL());
  }

  return normalized;
}

// https://html.spec.whatwg.org/C#merge-module-specifier-maps, into `old_map`
// in place, whose rules win.
void MergeModuleSpecifierMaps(ImportMap::SpecifierMap& old_map,
                              const ImportMap::SpecifierMap& new_map,
                              ImportMap::Warn warn) {
  // Step 2.
  for (const auto& [specifier, url] : new_map) {
    // Step 2.2 before step 2.1, as Blink does: try_emplace() leaves a
    // specifier that is already there alone.
    if (!old_map.try_emplace(specifier, url).second) {
      // Step 2.1.1.
      warn(base::StrCat(
          {"An import map rule for specifier '", base::UTF16ToUTF8(specifier),
           "' was removed, as it conflicted with an existing rule."}));
    }
  }
}

// Blink's MatchPrefix(), step 1.2 of
// https://html.spec.whatwg.org/C#resolving-an-imports-match: the key of
// `specifier_map` that ends in a slash and starts `key`, the longest if more
// do. Two such keys cannot have the same length, so no code unit comparison
// is needed.
std::optional<MatchResult> MatchPrefix(
    std::u16string_view key,
    const ImportMap::SpecifierMap& specifier_map) {
  std::optional<MatchResult> best_match;
  for (auto it = specifier_map.begin(); it != specifier_map.end(); ++it) {
    if (!it->first.ends_with(u'/') || !key.starts_with(it->first)) {
      continue;
    }
    if (best_match && it->first.size() < (*best_match)->first.size()) {
      continue;
    }
    best_match = it;
  }
  return best_match;
}

// https://html.spec.whatwg.org/C#resolving-an-imports-match, once `matched`
// matches `key`: the URL, or an invalid GURL where the spec throws.
GURL ResolveImportsMatchInternal(std::u16string_view key,
                                 MatchResult matched,
                                 std::string* debug_message) {
  const std::string key_utf8 = base::UTF16ToUTF8(key);
  const std::string matched_key_utf8 = base::UTF16ToUTF8(matched->first);
  const GURL& resolution_result = matched->second;

  // Step 1.2.3: the part of the specifier after the key.
  const std::u16string_view after_prefix = key.substr(matched->first.size());

  // Steps 1.1.1 and 1.2.1: a null resolution result blocks the specifier.
  if (!resolution_result.is_valid()) {
    *debug_message =
        base::StrCat({"Import Map: \"", key_utf8, "\" matches with \"",
                      matched_key_utf8, "\" but is blocked by a null value"});
    return GURL();
  }

  // Step 1.2.5.
  const GURL url = after_prefix.empty()
                       ? resolution_result
                       : ParseUrl(resolution_result, after_prefix);

  // Step 1.2.6.
  if (!url.is_valid()) {
    *debug_message = base::StrCat(
        {"Import Map: \"", key_utf8, "\" matches with \"", matched_key_utf8,
         "\" but is blocked due to relative URL parse failure"});
    return GURL();
  }

  // Step 1.2.8: a URL that backtracks above the prefix is blocked.
  if (!url.spec().starts_with(resolution_result.spec())) {
    *debug_message = base::StrCat({"Import Map: \"", key_utf8,
                                   "\" matches with \"", matched_key_utf8,
                                   "\" but is blocked due to backtracking"});
    return GURL();
  }

  // Step 1.2.9.
  *debug_message =
      base::StrCat({"Import Map: \"", key_utf8, "\" matches with \"",
                    matched_key_utf8, "\" and is mapped to ", ElidedUrl(url)});
  return url;
}

// https://html.spec.whatwg.org/C#resolving-an-imports-match: nullopt for
// null, an invalid GURL where the spec throws, the URL otherwise.
std::optional<GURL> ResolveImportsMatch(
    const ParsedSpecifier& parsed_specifier,
    const ImportMap::SpecifierMap& specifier_map,
    std::string* debug_message) {
  DCHECK(debug_message);
  const std::u16string key = parsed_specifier.GetImportMapKeyString();

  // Step 1.1: a key that is the specifier.
  const MatchResult exact = specifier_map.find(key);
  if (exact != specifier_map.end()) {
    return ResolveImportsMatchInternal(key, exact, debug_message);
  }

  // Step 1.2 matches a prefix only of a bare specifier or a special URL.
  if (parsed_specifier.GetType() == ParsedSpecifier::Type::kURL &&
      !IsSpecialScheme(parsed_specifier.GetUrl())) {
    *debug_message =
        base::StrCat({"Import Map: \"", base::UTF16ToUTF8(key),
                      "\" skips prefix match because of non-special URL "
                      "scheme"});
    return std::nullopt;
  }

  // Step 1.2.
  if (const std::optional<MatchResult> prefix_match =
          MatchPrefix(key, specifier_map)) {
    return ResolveImportsMatchInternal(key, *prefix_match, debug_message);
  }

  // Step 2.
  *debug_message =
      base::StrCat({"Import Map: \"", base::UTF16ToUTF8(key),
                    "\" matches with no entries and thus is not mapped."});
  return std::nullopt;
}

}  // namespace

v8::Local<v8::Value> ImportMapError::ToV8(v8::Isolate* isolate) const {
  const v8::Local<v8::String> text =
      bindings::ToV8StringFromUtf8(isolate, message);
  switch (type) {
    case Type::kTypeError:
      return v8::Exception::TypeError(text);
    case Type::kSyntaxError:
      return v8::Exception::SyntaxError(text);
  }
}

// https://html.spec.whatwg.org/C#parse-an-import-map-string. Keys and values
// that do not fit are ignored, each with its warning; only what makes the
// whole map unusable throws.
std::unique_ptr<ImportMap> ImportMap::Parse(
    std::u16string_view text,
    const GURL& base_url,
    Warn warn,
    std::optional<ImportMapError>* error_to_rethrow) {
  DCHECK(error_to_rethrow);

  // Step 1: parse JSON into Infra values, as Blink's ParseJSON() does.
  const std::unique_ptr<JsonValue> parsed = JsonParser(text).Parse();
  if (!parsed) {
    *error_to_rethrow =
        ImportMapError{ImportMapError::Type::kSyntaxError,
                       "Failed to parse import map: invalid JSON"};
    return std::make_unique<ImportMap>();
  }

  // Step 2.
  if (parsed->type() != JsonValue::Type::kObject) {
    *error_to_rethrow =
        ImportMapError{ImportMapError::Type::kTypeError,
                       "Failed to parse import map: not an object"};
    return std::make_unique<ImportMap>();
  }

  // Step 3.
  SpecifierMap sorted_and_normalized_imports;

  // Step 4. A member that is null exists too.
  if (const JsonValue* imports = parsed->Get(u"imports")) {
    // Step 4.1.
    if (imports->type() != JsonValue::Type::kObject) {
      *error_to_rethrow =
          ImportMapError{ImportMapError::Type::kTypeError,
                         "Failed to parse import map: \"imports\" top-level "
                         "key must be a JSON object."};
      return std::make_unique<ImportMap>();
    }

    // Step 4.2.
    sorted_and_normalized_imports =
        SortAndNormalizeSpecifierMap(*imports, base_url, warn);
  }

  // Step 5.
  ScopesMap normalized_scopes;

  // Step 6.
  if (const JsonValue* scopes = parsed->Get(u"scopes")) {
    // Step 6.1.
    if (scopes->type() != JsonValue::Type::kObject) {
      *error_to_rethrow =
          ImportMapError{ImportMapError::Type::kTypeError,
                         "Failed to parse import map: \"scopes\" top-level "
                         "key must be a JSON object."};
      return std::make_unique<ImportMap>();
    }

    // Step 6.2, https://html.spec.whatwg.org/C#sorting-and-normalizing-scopes.
    // Its step 2: for each scope prefix and potential specifier map.
    for (const auto& [scope_prefix, potential_specifier_map] :
         scopes->members()) {
      // Step 2.1, before the prefix is parsed.
      if (potential_specifier_map->type() != JsonValue::Type::kObject) {
        *error_to_rethrow = ImportMapError{
            ImportMapError::Type::kTypeError,
            base::StrCat({"Failed to parse import map: the value of the scope "
                          "with prefix \"",
                          base::UTF16ToUTF8(scope_prefix),
                          "\" must be a JSON object."})};
        return std::make_unique<ImportMap>();
      }

      // Step 2.2.
      const GURL prefix_url = ParseUrl(base_url, scope_prefix);

      // Step 2.3.
      if (!prefix_url.is_valid()) {
        warn(base::StrCat({"Ignored scope \"", base::UTF16ToUTF8(scope_prefix),
                           "\": not parsable as a URL."}));
        continue;
      }

      // Steps 2.4 and 2.5. A later prefix that serializes the same replaces
      // an earlier one. Step 3's sort is the order of ScopesMap.
      normalized_scopes.insert_or_assign(
          base::UTF8ToUTF16(prefix_url.spec()),
          SortAndNormalizeSpecifierMap(*potential_specifier_map, base_url,
                                       warn));
    }
  }

  // Step 7.
  IntegrityMap normalized_integrity;

  // Step 8.
  if (const JsonValue* integrity = parsed->Get(u"integrity")) {
    // Step 8.1.
    if (integrity->type() != JsonValue::Type::kObject) {
      *error_to_rethrow =
          ImportMapError{ImportMapError::Type::kTypeError,
                         "Failed to parse import map: \"integrity\" top-level "
                         "key must be a JSON object."};
      return std::make_unique<ImportMap>();
    }

    // Step 8.2, normalizing a module integrity map:
    // https://html.spec.whatwg.org/C#normalizing-a-module-integrity-map
    // Its step 2: for each key and value.
    for (const auto& [key, value] : integrity->members()) {
      // Step 2.1: resolve a URL-like module specifier.
      const ParsedSpecifier parsed_specifier =
          ParsedSpecifier::Create(key, base_url);

      // Step 2.2.
      if (parsed_specifier.GetType() != ParsedSpecifier::Type::kURL) {
        AddIgnoredValueMessage(
            warn, key,
            "Integrity key is not a valid absolute URL or relative URL "
            "starting with '/', './', or '../'");
        continue;
      }

      // Step 2.3.
      if (value->type() != JsonValue::Type::kString) {
        AddIgnoredValueMessage(warn, key, "Integrity value is not a string.");
        continue;
      }

      // Step 2.4. A later key that resolves the same replaces an earlier one.
      normalized_integrity.insert_or_assign(parsed_specifier.GetUrl(),
                                            value->string());
    }
  }

  // Step 9, the warning for other top-level keys, Blink leaves out.

  // Step 10.
  return std::make_unique<ImportMap>(std::move(sorted_and_normalized_imports),
                                     std::move(normalized_scopes),
                                     std::move(normalized_integrity));
}

ImportMap::ImportMap() = default;

ImportMap::ImportMap(SpecifierMap imports,
                     ScopesMap scopes,
                     IntegrityMap integrity)
    : imports_(std::move(imports)),
      scopes_(std::move(scopes)),
      integrity_(std::move(integrity)) {}

ImportMap::~ImportMap() = default;

// https://html.spec.whatwg.org/C#resolve-a-module-specifier
std::optional<GURL> ImportMap::Resolve(const ParsedSpecifier& parsed_specifier,
                                       const GURL& base_url,
                                       std::string* debug_message) const {
  DCHECK(debug_message);
  const std::u16string base_url_string =
      base::UTF8ToUTF16(base_url.possibly_invalid_spec());

  // Step 8: the scopes, most specific first.
  for (const auto& [scope, specifier_map] : scopes_) {
    // Step 8.1: a scope that is the base URL, or a prefix of it that ends in
    // a slash.
    if (scope == base_url_string ||
        (scope.ends_with(u'/') && base_url_string.starts_with(scope))) {
      // Steps 8.1.1 and 8.1.2: a match in the scope, null falling through to
      // the next.
      std::optional<GURL> scope_match =
          ResolveImportsMatch(parsed_specifier, specifier_map, debug_message);
      if (scope_match) {
        return scope_match;
      }
    }
  }

  // Steps 9 and 10.
  return ResolveImportsMatch(parsed_specifier, imports_, debug_message);
}

std::optional<std::u16string> ImportMap::ResolveIntegrity(
    const GURL& module_url) const {
  const auto it = integrity_.find(module_url);
  if (it == integrity_.end()) {
    return std::nullopt;
  }
  return it->second;
}

// https://html.spec.whatwg.org/C/#merge-existing-and-new-import-maps
void ImportMap::MergeExistingAndNewImportMaps(
    std::unique_ptr<ImportMap> new_import_map,
    const std::map<std::u16string, std::set<std::u16string>>&
        scoped_resolved_module_map,
    const std::set<std::u16string>& toplevel_resolved_module_set,
    Warn warn) {
  DCHECK(new_import_map);

  // Steps 1 and 2 copy the new map's scopes and imports. This owns the new
  // map now, so it changes them in place instead.
  ScopesMap& new_import_map_scopes = new_import_map->scopes_;
  SpecifierMap& new_import_map_imports = new_import_map->imports_;
  const IntegrityMap& new_import_map_integrity = new_import_map->integrity_;

  // Step 3.
  for (auto& [scope, scope_imports] : new_import_map_scopes) {
    // Step 3.1 walks the global's resolved module set. Blink keeps, for each
    // scope prefix, the specifiers and specifier prefixes already resolved in
    // it instead, and removes the scope's rules for those.
    const auto current_set_it = scoped_resolved_module_map.find(scope);
    if (current_set_it != scoped_resolved_module_map.end()) {
      const std::set<std::u16string>& current_resolved_set =
          current_set_it->second;
      std::vector<std::u16string> specifiers_to_remove;
      for (const auto& entry : scope_imports) {
        if (current_resolved_set.contains(entry.first)) {
          specifiers_to_remove.push_back(entry.first);
        }
      }
      for (const std::u16string& specifier : specifiers_to_remove) {
        // Step 3.1.2.1.1.
        warn(base::StrCat({"An import map scope rule for specifier '",
                           base::UTF16ToUTF8(specifier),
                           "' was removed, as it conflicted with already "
                           "resolved module specifiers."}));
        // Step 3.1.2.1.2.
        scope_imports.erase(specifier);
      }
    }

    // Steps 3.2 and 3.3.
    const auto old_scope_it = scopes_.find(scope);
    if (old_scope_it != scopes_.end()) {
      MergeModuleSpecifierMaps(old_scope_it->second, scope_imports, warn);
    } else {
      scopes_.emplace(scope, std::move(scope_imports));
    }
  }

  // Step 4.
  for (const auto& [url, integrity] : new_import_map_integrity) {
    // Step 4.2 before step 4.1, as Blink does: try_emplace() leaves a URL
    // that is already there alone.
    if (!integrity_.try_emplace(url, integrity).second) {
      // Step 4.1.1.
      warn(base::StrCat({"An import map integrity rule for url '",
                         url.possibly_invalid_spec(),
                         "' was removed, as it conflicted with already "
                         "defined integrity rules."}));
    }
  }

  // Step 5 walks the global's resolved module set. Blink keeps every
  // specifier resolved at the top level and each of its prefixes instead,
  // and removes the new map's rules for those.
  for (const std::u16string& specifier : toplevel_resolved_module_set) {
    if (!new_import_map_imports.contains(specifier)) {
      continue;
    }
    // Step 5.1.1.1.
    warn(base::StrCat(
        {"An import map rule for specifier '", base::UTF16ToUTF8(specifier),
         "' was removed, as it conflicted with already resolved module "
         "specifiers."}));
    // Step 5.1.1.2.
    new_import_map_imports.erase(specifier);
  }

  // Step 6.
  MergeModuleSpecifierMaps(imports_, new_import_map_imports, warn);
}

std::string ImportMap::ToStringForTesting() const {
  std::string builder = "{\"imports\":";
  AppendSpecifierMapForTesting(builder, imports_);

  builder += ",\"scopes\":{";
  bool is_first = true;
  for (const auto& [scope, specifier_map] : scopes_) {
    if (!is_first) {
      builder += ',';
    }
    is_first = false;
    AppendEncodedForDebugging(builder, scope);
    builder += ':';
    AppendSpecifierMapForTesting(builder, specifier_map);
  }

  builder += "},\"integrity\": {";
  is_first = true;
  for (const auto& [url, integrity] : integrity_) {
    if (!is_first) {
      builder += ',';
    }
    is_first = false;
    base::StrAppend(&builder, {"\"", url.possibly_invalid_spec(), "\":\"",
                               base::UTF16ToUTF8(integrity), "\""});
  }

  builder += "}}";
  return builder;
}

}  // namespace mitto
