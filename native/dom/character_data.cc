#include "mitto/dom/character_data.h"

#include "base/functional/function_ref.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/attached_traversals.h"
#include "mitto/dom/document.h"
#include "mitto/dom/mutation_observer.h"
#include "mitto/dom/mutation_record.h"
#include "v8/include/cppgc/persistent.h"

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
  ReplaceDataInternal(offset, 0, data);
}

void CharacterData::DeleteData(uint32_t offset,
                               uint32_t count,
                               bindings::ExceptionState& exception_state) {
  uint32_t real_count;
  if (!ValidateOffsetCount(offset, count, Length(), real_count,
                           exception_state)) {
    return;
  }
  ReplaceDataInternal(offset, real_count, std::u16string());
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
  ReplaceDataInternal(offset, real_count, data);
}

void CharacterData::SetNodeValue(const std::optional<std::u16string>& value,
                                 bindings::ExceptionState& exception_state) {
  SetData(value.value_or(std::u16string()));
}

void CharacterData::SetData(const std::u16string& data) {
  ReplaceDataInternal(0, Length(), data);
}

void CharacterData::AppendData(const std::u16string& data) {
  ReplaceDataInternal(Length(), 0, data);
}

void CharacterData::SetDataWithoutUpdate(const std::u16string& data) {
  ModifyData([&data](std::u16string& current) { current = data; });
}

void CharacterData::ReplaceDataInternal(uint32_t offset,
                                        uint32_t count,
                                        const std::u16string& data) {
  ModifyData([offset, count, &data](std::u16string& current) {
    current.replace(offset, count, data);
  });
  if (AttachedTraversals* const traversals =
          GetDocument().ExistingTraversals()) {
    traversals->DidReplaceText(*this, offset, count,
                               static_cast<uint32_t>(data.size()));
  }
}

void CharacterData::ModifyData(
    base::FunctionRef<void(std::u16string& data)> change) {
  // Blink's CharacterData::DidModifyData(): a record for the observers that
  // the change concerns, which no script can change meanwhile. The old data is
  // kept only for them, since copying it for each append would make the
  // parser's growing text quadratic, where Blink's strings share their
  // characters.
  MutationObserverInterestGroup* const recipients =
      MutationObserverInterestGroup::CreateForCharacterDataMutation(*this);
  if (!recipients) {
    change(data_);
    return;
  }
  const cppgc::Persistent<MutationObserverInterestGroup> held(recipients);
  const std::u16string old_data = data_;
  change(data_);
  recipients->EnqueueMutationRecord(
      MutationRecord::CreateCharacterData(this, old_data));
}

std::optional<std::u16string> CharacterData::NodeValue() const {
  return data_;
}

}  // namespace mitto
