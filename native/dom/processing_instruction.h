// The DOM's ProcessingInstruction node, with the attributes that Chrome reads
// of its data as an XML start tag's: getAttribute() and the rest parse the
// data, and setting or removing one writes the data again, which observers
// hear of as a change of it. Its sheet is a Known Gap, which gives null, as
// for a processing instruction that is not a style sheet.
//
// Upstream: third_party/blink/renderer/core/dom/processing_instruction.cc
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser.cc

#ifndef MITTO_DOM_PROCESSING_INSTRUCTION_H_
#define MITTO_DOM_PROCESSING_INSTRUCTION_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mitto/dom/character_data.h"

namespace mitto {

class ProcessingInstruction final : public CharacterData {
 public:
  static ProcessingInstruction* Create(Document& document,
                                       const std::u16string& target,
                                       const std::u16string& data);

  ProcessingInstruction(Document& document,
                        const std::u16string& target,
                        const std::u16string& data);
  ~ProcessingInstruction() override;

  // Blink's Document::IsValidName(): XML's Name production, by the Unicode
  // categories that Blink allows, which createProcessingInstruction() checks
  // the target with.
  static bool IsValidName(std::u16string_view name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Target() const { return target_; }
  std::optional<std::u16string> GetAttribute(const std::u16string& name);
  bool HasAttribute(const std::u16string& name);
  void SetAttribute(const std::u16string& name,
                    const std::u16string& value,
                    bindings::ExceptionState& exception_state);
  void RemoveAttribute(const std::u16string& name);
  void ToggleAttribute(const std::u16string& name,
                       std::optional<bool> force,
                       bindings::ExceptionState& exception_state);
  bool HasAttributes();
  std::vector<std::u16string> GetAttributeNames();

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;
  // CharacterData's: Blink's DidChangeData() marks the attributes for reading
  // again, after any change of the data through the DOM.
  void DidChangeData() override;

 private:
  // Blink's ProcessAttributesIfNeeded(): the data's attributes, read again
  // once the data has changed. None while the data is no list of them.
  void ProcessAttributesIfNeeded();
  // UpdateDataFromAttributes(): name="value" for each, in order.
  void UpdateDataFromAttributes();

  const std::u16string target_;
  std::vector<std::pair<std::u16string, std::u16string>> attributes_;
  // Whether attributes_ is to be read from the data again, as Blink's
  // attributes_dirty_ says: from the start, and after a change of the data
  // through the DOM, but not after one that the attributes made.
  bool attributes_dirty_ = true;
};

}  // namespace mitto

#endif  // MITTO_DOM_PROCESSING_INSTRUCTION_H_
