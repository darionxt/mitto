#include "mitto/dom/url_search_params.h"

#include <algorithm>
#include <string_view>
#include <variant>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_url_search_params.h"
#include "mitto/dom/url.h"
#include "url/url_util.h"

namespace mitto {

namespace {

// The URL Standard's urlencoded parser's decoding: '+' is a space, and the
// percent-encoded bytes are UTF-8, as Blink's DecodeString() reads them.
std::u16string DecodeString(std::u16string_view input) {
  std::u16string replaced(input);
  std::replace(replaced.begin(), replaced.end(), u'+', u' ');
  const url::UrlEscapeDecoder decoder(base::UTF16ToUTF8(replaced),
                                      url::DecodeUrlMode::kUtf8);
  return std::u16string(decoder.view());
}

// FormDataEncoder::EncodeStringAsFormData() for
// application/x-www-form-urlencoded, without normalizing line breaks.
void EncodeStringAsFormData(std::string& buffer, std::string_view string) {
  constexpr std::string_view kSafeCharacters = "-._*";
  constexpr std::string_view kHexChars = "0123456789ABCDEF";
  for (const char character : string) {
    const unsigned char c = static_cast<unsigned char>(character);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c != '\0' && kSafeCharacters.find(c) != std::string_view::npos)) {
      buffer += static_cast<char>(c);
    } else if (c == ' ') {
      buffer += '+';
    } else {
      buffer += '%';
      buffer += kHexChars[c / 16];
      buffer += kHexChars[c % 16];
    }
  }
}

}  // namespace

class URLSearchParams::IterationSource final
    : public bindings::PairIterationSource {
 public:
  explicit IterationSource(URLSearchParams* params) : params_(params) {}

  void Trace(cppgc::Visitor* visitor) const override {
    bindings::PairIterationSource::Trace(visitor);
    visitor->Trace(params_);
  }

 protected:
  bool FetchNextItem(v8::Isolate* isolate,
                     v8::Local<v8::Value>* key,
                     v8::Local<v8::Value>* value) override {
    if (current_ >= params_->params_.size()) {
      return false;
    }
    const auto& [name, param_value] = params_->params_[current_++];
    *key = bindings::ToV8String(isolate, name);
    *value = bindings::ToV8String(isolate, param_value);
    return true;
  }

 private:
  const cppgc::Member<URLSearchParams> params_;
  size_t current_ = 0;
};

// static
URLSearchParams* URLSearchParams::Create(
    const bindings::
        USVStringSequenceSequenceOrUSVStringUSVStringRecordOrUSVString& init,
    bindings::ExceptionState& exception_state) {
  if (const auto* query = std::get_if<std::u16string>(&init)) {
    std::u16string_view without_mark(*query);
    if (without_mark.starts_with(u'?')) {
      without_mark.remove_prefix(1);
    }
    return Create(std::u16string(without_mark), nullptr);
  }
  URLSearchParams* const params = Create(std::u16string(), nullptr);
  if (const auto* pairs =
          std::get_if<std::vector<std::vector<std::u16string>>>(&init)) {
    for (const std::vector<std::u16string>& pair : *pairs) {
      if (pair.size() != 2) {
        // Blink's message, to which the bindings add the context again.
        exception_state.ThrowTypeError(
            bindings::ExceptionMessages::FailedToConstruct(
                "URLSearchParams",
                "Sequence initializer must only contain pair elements"));
        return nullptr;
      }
      params->AppendWithoutUpdate(pair[0], pair[1]);
    }
    return params;
  }
  for (const auto& [name, value] :
       std::get<std::vector<std::pair<std::u16string, std::u16string>>>(
           init)) {
    params->AppendWithoutUpdate(name, value);
  }
  return params;
}

// static
URLSearchParams* URLSearchParams::Create(const std::u16string& query,
                                         URL* url_object) {
  return bindings::MakeGarbageCollected<URLSearchParams>(query, url_object);
}

URLSearchParams::URLSearchParams(const std::u16string& query, URL* url_object)
    : url_object_(url_object) {
  if (!query.empty()) {
    SetInputWithoutUpdate(query);
  }
}

URLSearchParams::~URLSearchParams() = default;

bindings::PairIterationSource* URLSearchParams::CreateIterationSource() {
  return bindings::MakeGarbageCollected<IterationSource>(this);
}

const bindings::WrapperTypeInfo* URLSearchParams::GetWrapperTypeInfo() const {
  return &bindings::V8URLSearchParams::kWrapperTypeInfo;
}

void URLSearchParams::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(url_object_);
  bindings::ScriptWrappable::Trace(visitor);
}

void URLSearchParams::RunUpdateSteps() {
  if (!url_object_ || url_object_->IsInUpdate()) {
    return;
  }
  url_object_->SetSearchInternal(ToString());
}

void URLSearchParams::SetInputWithoutUpdate(std::u16string_view query) {
  params_.clear();
  size_t start = 0;
  while (start < query.size()) {
    size_t name_value_end = query.find(u'&', start);
    if (name_value_end == std::u16string_view::npos) {
      name_value_end = query.size();
    }
    if (name_value_end > start) {
      size_t end_of_name = query.find(u'=', start);
      if (end_of_name == std::u16string_view::npos ||
          end_of_name > name_value_end) {
        end_of_name = name_value_end;
      }
      std::u16string value;
      if (end_of_name != name_value_end) {
        value = DecodeString(query.substr(
            end_of_name + 1, name_value_end - end_of_name - 1));
      }
      AppendWithoutUpdate(
          DecodeString(query.substr(start, end_of_name - start)), value);
    }
    start = name_value_end + 1;
  }
}

std::u16string URLSearchParams::ToString() const {
  std::string encoded;
  for (const auto& [name, value] : params_) {
    if (!encoded.empty()) {
      encoded += '&';
    }
    EncodeStringAsFormData(encoded, base::UTF16ToUTF8(name));
    encoded += '=';
    EncodeStringAsFormData(encoded, base::UTF16ToUTF8(value));
  }
  return base::ASCIIToUTF16(encoded);
}

uint32_t URLSearchParams::Size() const {
  return static_cast<uint32_t>(params_.size());
}

void URLSearchParams::AppendWithoutUpdate(const std::u16string& name,
                                          const std::u16string& value) {
  params_.emplace_back(name, value);
}

void URLSearchParams::Append(const std::u16string& name,
                             const std::u16string& value) {
  AppendWithoutUpdate(name, value);
  RunUpdateSteps();
}

void URLSearchParams::DeleteAllWithNameOrTuple(
    const std::u16string& name,
    const std::optional<std::u16string>& value) {
  std::erase_if(params_, [&name, &value](const auto& param) {
    return param.first == name && (!value || param.second == *value);
  });
  RunUpdateSteps();
}

std::optional<std::u16string> URLSearchParams::Get(
    const std::u16string& name) const {
  for (const auto& [param_name, param_value] : params_) {
    if (param_name == name) {
      return param_value;
    }
  }
  return std::nullopt;
}

std::vector<std::u16string> URLSearchParams::GetAll(
    const std::u16string& name) const {
  std::vector<std::u16string> values;
  for (const auto& [param_name, param_value] : params_) {
    if (param_name == name) {
      values.push_back(param_value);
    }
  }
  return values;
}

bool URLSearchParams::Has(const std::u16string& name,
                          const std::optional<std::u16string>& value) const {
  return std::any_of(params_.begin(), params_.end(),
                     [&name, &value](const auto& param) {
                       return param.first == name &&
                              (!value || param.second == *value);
                     });
}

void URLSearchParams::Set(const std::u16string& name,
                          const std::u16string& value) {
  bool found = false;
  for (auto it = params_.begin(); it != params_.end();) {
    if (it->first != name) {
      ++it;
    } else if (!found) {
      it->second = value;
      found = true;
      ++it;
    } else {
      it = params_.erase(it);
    }
  }
  if (!found) {
    Append(name, value);
  } else {
    RunUpdateSteps();
  }
}

void URLSearchParams::Sort() {
  // By UTF-16 code units, as CodeUnitCompareLessThan() compares names.
  std::stable_sort(
      params_.begin(), params_.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
  RunUpdateSteps();
}

}  // namespace mitto
