// The DOM's CharacterData: a node that holds text, the common part of Text,
// Comment and the other nodes whose value is their data.
//
// Upstream: third_party/blink/renderer/core/dom/character_data.h
// Upstream: third_party/blink/renderer/core/dom/character_data.cc

#ifndef MITTO_DOM_CHARACTER_DATA_H_
#define MITTO_DOM_CHARACTER_DATA_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/functional/function_ref.h"
#include "mitto/dom/node.h"

namespace mitto {

class CharacterData : public Node {
 public:
  ~CharacterData() override;

  // The members of the interface.
  const std::u16string& Data() const { return data_; }
  void SetData(const std::u16string& data);
  uint32_t Length() const { return static_cast<uint32_t>(data_.size()); }
  std::u16string SubstringData(uint32_t offset,
                               uint32_t count,
                               bindings::ExceptionState& exception_state) const;
  void AppendData(const std::u16string& data);
  void InsertData(uint32_t offset,
                  const std::u16string& data,
                  bindings::ExceptionState& exception_state);
  void DeleteData(uint32_t offset,
                  uint32_t count,
                  bindings::ExceptionState& exception_state);
  void ReplaceData(uint32_t offset,
                   uint32_t count,
                   const std::u16string& data,
                   bindings::ExceptionState& exception_state);

  // Node's.
  std::optional<std::u16string> NodeValue() const override;
  void SetNodeValue(const std::optional<std::u16string>& value,
                    bindings::ExceptionState& exception_state) override;

  // Sets the data, which the observers hear of, without moving the live ranges
  // in it, for splitText() and normalize(), which move them themselves, as
  // Blink's SetDataWithoutUpdate() and DidModifyData() do.
  void SetDataWithoutUpdate(const std::u16string& data);

 protected:
  CharacterData(Document& document, const std::u16string& data);

 private:
  // The DOM standard's "replace data", with `count` already cut to what the
  // data holds after `offset`: the observers hear of it and the live ranges in
  // the node move.
  void ReplaceDataInternal(uint32_t offset,
                           uint32_t count,
                           const std::u16string& data);
  // Makes `change` to the data, and queues a record of it for the observers
  // that it concerns.
  void ModifyData(base::FunctionRef<void(std::u16string& data)> change);

  std::u16string data_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CHARACTER_DATA_H_
