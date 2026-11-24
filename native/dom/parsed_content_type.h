// A Content-Type or Content-Disposition value parsed as Blink parses it: a
// type, or a type and a subtype, then parameters, each a token and a token or
// quoted string after ";". A value whose parameters do not parse has its type
// but no parameters, and is not valid.
//
// Upstream: third_party/blink/renderer/platform/network/header_field_tokenizer.cc
// Upstream: third_party/blink/renderer/platform/network/parsed_content_header_field_parameters.cc
// Upstream: third_party/blink/renderer/platform/network/parsed_content_type.cc
// Upstream: third_party/blink/renderer/platform/network/parsed_content_disposition.cc

#ifndef MITTO_DOM_PARSED_CONTENT_TYPE_H_
#define MITTO_DOM_PARSED_CONTENT_TYPE_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mitto {

// What a parameter's value may hold: Blink's HeaderFieldTokenizer::Mode, in
// which kRelaxed takes the separators "(),/:<=>?@[\]" as token characters.
enum class HeaderFieldMode {
  kNormal,
  kRelaxed,
};

// The parameters after a type, as Blink's ParsedContentHeaderFieldParameters.
class ParsedContentParameters {
 public:
  explicit ParsedContentParameters(
      std::vector<std::pair<std::u16string, std::u16string>> parameters);
  ParsedContentParameters(const ParsedContentParameters&);
  ParsedContentParameters& operator=(const ParsedContentParameters&);
  ~ParsedContentParameters();

  // The value of the last parameter named `name`, in any case, if any.
  std::optional<std::u16string> ValueForName(std::u16string_view name) const;

 private:
  std::vector<std::pair<std::u16string, std::u16string>> parameters_;
};

class ParsedContentType {
 public:
  explicit ParsedContentType(std::u16string_view content_type,
                             HeaderFieldMode mode = HeaderFieldMode::kNormal);
  ~ParsedContentType();

  // "type/subtype" as written, or empty if it does not parse.
  const std::u16string& MimeType() const { return mime_type_; }
  // Null for an invalid value.
  std::optional<std::u16string> ParameterValueForName(
      std::u16string_view name) const;
  bool IsValid() const { return parameters_.has_value(); }

 private:
  std::u16string mime_type_;
  std::optional<ParsedContentParameters> parameters_;
};

class ParsedContentDisposition {
 public:
  explicit ParsedContentDisposition(
      std::u16string_view content_disposition,
      HeaderFieldMode mode = HeaderFieldMode::kNormal);
  ~ParsedContentDisposition();

  // The type as written, or null if it does not parse.
  const std::optional<std::u16string>& Type() const { return type_; }
  std::optional<std::u16string> Filename() const {
    return ParameterValueForName(u"filename");
  }
  std::optional<std::u16string> ParameterValueForName(
      std::u16string_view name) const;
  bool IsValid() const { return parameters_.has_value(); }

 private:
  std::optional<std::u16string> type_;
  std::optional<ParsedContentParameters> parameters_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PARSED_CONTENT_TYPE_H_
