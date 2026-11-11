#include "mitto/dom/character_data.h"

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"

namespace mitto {

namespace {

// Blink's message for an offset past the data, and ValidateOffsetCount(),
// which cuts the count to what the data holds after the offset.
bool ValidateOffsetCount(uint32_t offset,
                         uint32_t count,
                         uint32_t length,
                         uint32_t& real_count,
                         bindings::ExceptionState& exception_state) {
  if (offset > length) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The offset ", base::NumberToString(offset),
                      " is greater than the node's length (",
                      base::NumberToString(length), ")."}));
    return false;
  }
  real_count = count > length - offset ? length - offset : count;
  return true;
}

}  // namespace

CharacterData::CharacterData(Document& document, const std::u16string& data)
    : Node(&document), data_(data) {}

CharacterData::~CharacterData() = default;

std::u16string CharacterData::SubstringData(
    uint32_t offset,
    uint32_t count,
    bindings::ExceptionState& exception_state) const {
  uint32_t real_count;
  if (!ValidateOffsetCount(offset, count, Length(), real_count,
                           exception_state)) {
    return std::u16string();
  }
  return data_.substr(offset, real_count);
}

void CharacterData::InsertData(uint32_t offset,
                               const std::u16string& data,
                               bindings::ExceptionState& exception_state) {
  uint32_t real_count;
  if (!ValidateOffsetCount(offset, 0, Length(), real_count, exception_state)) {
    return;
  }
  data_.insert(offset, data);
}

void CharacterData::DeleteData(uint32_t offset,
                               uint32_t count,
                               bindings::ExceptionState& exception_state) {
  uint32_t real_count;
  if (!ValidateOffsetCount(offset, count, Length(), real_count,
                           exception_state)) {
    return;
  }
  data_.erase(offset, real_count);
}

void CharacterData::ReplaceData(uint32_t offset,
                                uint32_t count,
                                const std::u16string& data,
                                bindings::ExceptionState& exception_state) {
  uint32_t real_count;
  if (!ValidateOffsetCount(offset, count, Length(), real_count,
                           exception_state)) {
    return;
  }
  data_.replace(offset, real_count, data);
}

void CharacterData::SetNodeValue(const std::optional<std::u16string>& value,
                                 bindings::ExceptionState& exception_state) {
  data_ = value.value_or(std::u16string());
}

std::optional<std::u16string> CharacterData::NodeValue() const {
  return data_;
}

}  // namespace mitto
