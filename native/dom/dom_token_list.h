// The DOM's DOMTokenList: the ordered set of tokens in an element's content
// attribute, such as classList's of `class`, which reads the attribute each
// time and writes it back serialized. One that a Known Gap makes, such as
// relList while its element has no class, has no element, and so no tokens,
// and keeps nothing.
//
// Upstream: third_party/blink/renderer/core/dom/dom_token_list.h
// Upstream: third_party/blink/renderer/core/dom/dom_token_list.cc
// Upstream: third_party/blink/renderer/core/dom/space_split_string.cc

#ifndef MITTO_DOM_DOM_TOKEN_LIST_H_
#define MITTO_DOM_DOM_TOKEN_LIST_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Element;

class DOMTokenList : public bindings::ScriptWrappable {
 public:
  // The list of `element`'s content attribute `attribute_name`, of no
  // namespace, which has no supported tokens.
  static DOMTokenList* Create(Element& element, std::u16string attribute_name);
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  DOMTokenList(Element* element, std::u16string attribute_name);
  ~DOMTokenList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  std::optional<std::u16string> Item(uint32_t index) const;
  bool Contains(const std::u16string& token) const;
  void Add(const std::vector<std::u16string>& tokens,
           bindings::ExceptionState& exception_state);
  void Remove(const std::vector<std::u16string>& tokens,
              bindings::ExceptionState& exception_state);
  bool Toggle(const std::u16string& token,
              std::optional<bool> force,
              bindings::ExceptionState& exception_state);
  bool Replace(const std::u16string& token,
               const std::u16string& new_token,
               bindings::ExceptionState& exception_state);
  bool Supports(const std::u16string& token,
                bindings::ExceptionState& exception_state);
  std::u16string Value() const;
  void SetValue(const std::u16string& value);

 protected:
  // The DOM standard's validation steps, Blink's ValidateTokenValue(), for a
  // token in lower case: a list with no supported tokens throws.
  virtual bool ValidateTokenValue(
      const std::u16string& token,
      bindings::ExceptionState& exception_state) const;

  Element* GetElement() const { return element_.Get(); }

 private:
  // The token set: the attribute's value split at HTML spaces, each token
  // once, in order.
  std::vector<std::u16string> Tokens() const;
  // The DOM standard's update steps: the set serialized into the attribute.
  void Update(const std::vector<std::u16string>& tokens);

  const cppgc::Member<Element> element_;
  const std::u16string attribute_name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_TOKEN_LIST_H_
