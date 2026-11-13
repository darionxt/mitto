#include "mitto/dom/performance_server_timing.h"

#include <stddef.h>

#include <initializer_list>
#include <optional>

#include "base/strings/string_util.h"
#include "base/third_party/double_conversion/double-conversion/double-conversion.h"
#include "mitto/bindings/v8_performance_server_timing.h"
#include "mitto/dom/performance_entry.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// HeaderFieldTokenizer's IsTokenCharacter() in its normal mode.
bool IsTokenCharacter(char16_t c) {
  if (c >= 128 || c < 0x20) {
    return false;
  }
  switch (c) {
    case ' ':
    case ';':
    case '"':
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ':':
    case '\\':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
      return false;
    default:
      return true;
  }
}

// Blink's HeaderFieldTokenizer, over a header's value.
class HeaderFieldTokenizer {
 public:
  explicit HeaderFieldTokenizer(std::u16string input)
      : input_(std::move(input)) {
    SkipOptionalWhitespace();
  }

  bool IsConsumed() const { return index_ >= input_.size(); }

  bool Consume(char16_t c) {
    if (IsConsumed() || input_[index_] != c) {
      return false;
    }
    ++index_;
    SkipOptionalWhitespace();
    return true;
  }

  bool ConsumeToken(std::u16string& output) {
    const size_t start = index_;
    while (!IsConsumed() && IsTokenCharacter(input_[index_])) {
      ++index_;
    }
    if (start == index_) {
      return false;
    }
    output = input_.substr(start, index_ - start);
    SkipOptionalWhitespace();
    return true;
  }

  bool ConsumeTokenOrQuotedString(std::u16string& output) {
    if (IsConsumed()) {
      return false;
    }
    if (input_[index_] == '"') {
      return ConsumeQuotedString(output);
    }
    return ConsumeToken(output);
  }

  void ConsumeBeforeAnyCharMatch(std::initializer_list<char16_t> chars) {
    while (!IsConsumed()) {
      for (const char16_t c : chars) {
        if (c == input_[index_]) {
          return;
        }
      }
      ++index_;
    }
  }

 private:
  bool ConsumeQuotedString(std::u16string& output) {
    std::u16string builder;
    ++index_;
    while (!IsConsumed()) {
      if (input_[index_] == '"') {
        output = std::move(builder);
        ++index_;
        SkipOptionalWhitespace();
        return true;
      }
      if (input_[index_] == '\\') {
        ++index_;
        if (IsConsumed()) {
          return false;
        }
      }
      builder.push_back(input_[index_]);
      ++index_;
    }
    return false;
  }

  void SkipOptionalWhitespace() {
    while (!IsConsumed() && (input_[index_] == ' ' || input_[index_] == '\t')) {
      ++index_;
    }
  }

  const std::u16string input_;
  size_t index_ = 0;
};

// WTF's StringToDouble(): a decimal number after ASCII spaces, with nothing
// after it, and no Infinity or NaN.
// Upstream: third_party/blink/renderer/platform/wtf/text/string_to_number.cc
// Upstream: third_party/blink/renderer/platform/wtf/dtoa.cc
std::optional<double> StringToDouble(std::u16string_view text) {
  size_t leading = 0;
  while (leading < text.size() && base::IsAsciiWhitespace(text[leading])) {
    ++leading;
  }
  // Characters past ASCII end the number, as ParseDoubleFromLongString()
  // turns them into NUL.
  std::string ascii;
  for (const char16_t c : text.substr(leading)) {
    ascii.push_back(c < 0x80 ? static_cast<char>(c) : '\0');
  }
  static const double_conversion::StringToDoubleConverter converter(
      double_conversion::StringToDoubleConverter::ALLOW_LEADING_SPACES |
          double_conversion::StringToDoubleConverter::ALLOW_TRAILING_JUNK,
      0.0, 0, nullptr, nullptr);
  int parsed = 0;
  const double number = converter.StringToDouble(
      ascii.data(), static_cast<int>(ascii.size()), &parsed);
  if (parsed == 0 || static_cast<size_t>(parsed) != ascii.size()) {
    return std::nullopt;
  }
  return number;
}

// ServerTimingHeader::SetParameter(): the first dur and desc count.
struct ServerTimingHeader {
  void SetParameter(std::u16string_view name, const std::u16string& value) {
    if (base::EqualsCaseInsensitiveASCII(name, u"dur")) {
      if (!duration_set) {
        info.duration = StringToDouble(value).value_or(0);
        duration_set = true;
      }
    } else if (base::EqualsCaseInsensitiveASCII(name, u"desc")) {
      if (!description_set) {
        info.description = value;
        description_set = true;
      }
    }
  }

  ServerTimingInfo info;
  bool duration_set = false;
  bool description_set = false;
};

}  // namespace

std::vector<ServerTimingInfo> ParseServerTimingHeader(std::string_view value) {
  std::vector<ServerTimingInfo> metrics;
  std::u16string latin1;
  for (const char byte : value) {
    latin1.push_back(static_cast<unsigned char>(byte));
  }
  HeaderFieldTokenizer tokenizer(std::move(latin1));
  while (!tokenizer.IsConsumed()) {
    std::u16string name;
    if (!tokenizer.ConsumeToken(name)) {
      break;
    }
    ServerTimingHeader header;
    header.info.name = name;
    tokenizer.ConsumeBeforeAnyCharMatch({',', ';'});
    while (tokenizer.Consume(';')) {
      std::u16string parameter_name;
      if (!tokenizer.ConsumeToken(parameter_name)) {
        break;
      }
      std::u16string parameter_value;
      if (tokenizer.Consume('=')) {
        tokenizer.ConsumeTokenOrQuotedString(parameter_value);
        tokenizer.ConsumeBeforeAnyCharMatch({',', ';'});
      }
      header.SetParameter(parameter_name, parameter_value);
    }
    metrics.push_back(header.info);
    if (!tokenizer.Consume(',')) {
      break;
    }
  }
  return metrics;
}

PerformanceServerTiming::PerformanceServerTiming(const ServerTimingInfo& info)
    : info_(info) {}

PerformanceServerTiming::~PerformanceServerTiming() = default;

const bindings::WrapperTypeInfo* PerformanceServerTiming::GetWrapperTypeInfo()
    const {
  return &bindings::V8PerformanceServerTiming::kWrapperTypeInfo;
}

v8::Local<v8::Object> PerformanceServerTiming::ToJSONForBinding() const {
  V8ObjectBuilder builder(v8::Isolate::GetCurrent());
  builder.AddString("name", Name());
  builder.AddNumber("duration", Duration());
  builder.AddString("description", Description());
  return builder.Result();
}

}  // namespace mitto
