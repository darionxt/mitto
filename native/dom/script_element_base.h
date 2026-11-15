// What a ScriptLoader needs of a script element, the HTML Standard's or SVG's,
// as Blink's ScriptElementBase has it: the values of the attributes that
// prepare the script, the events it fires, and where its text starts.
//
// Upstream: third_party/blink/renderer/core/script/script_element_base.h

#ifndef MITTO_DOM_SCRIPT_ELEMENT_BASE_H_
#define MITTO_DOM_SCRIPT_ELEMENT_BASE_H_

#include <optional>
#include <string>

namespace mitto {

class Element;
class ScriptLoader;

class ScriptElementBase {
 public:
  virtual Element& AsElement() = 0;
  virtual const Element& AsElement() const = 0;
  virtual ScriptLoader& Loader() const = 0;

  // The attributes, null for one that is missing, and those that SVG's
  // script element lacks, such as async, missing there.
  virtual std::optional<std::u16string> SourceAttributeValue() const = 0;
  virtual std::optional<std::u16string> TypeAttributeValue() const = 0;
  virtual std::optional<std::u16string> LanguageAttributeValue() const = 0;
  virtual std::optional<std::u16string> EventAttributeValue() const = 0;
  virtual std::optional<std::u16string> ForAttributeValue() const = 0;
  virtual std::optional<std::u16string> CharsetAttributeValue() const = 0;
  virtual std::optional<std::u16string> IntegrityAttributeValue() const = 0;
  virtual std::optional<std::u16string> CrossOriginAttributeValue() const = 0;
  virtual std::optional<std::u16string> ReferrerPolicyAttributeValue()
      const = 0;
  virtual std::optional<std::u16string> FetchPriorityAttributeValue() const = 0;
  virtual bool AsyncAttributeValue() const = 0;
  virtual bool DeferAttributeValue() const = 0;
  virtual bool NomoduleAttributeValue() const = 0;

  // https://html.spec.whatwg.org/C/#potentially-render-blocking
  virtual bool IsPotentiallyRenderBlocking() const = 0;

  // Where the script's text starts in its document, a zero-based line and
  // column, as Blink's TextPosition: the offsets of an inline script's
  // errors.
  virtual int StartLine() const = 0;
  virtual int StartColumn() const = 0;

  // Fire load and error at the element, which do not bubble.
  virtual void DispatchLoadEvent() = 0;
  virtual void DispatchErrorEvent() = 0;

 protected:
  ~ScriptElementBase() = default;
};

// The script element that `element` is, Blink's ScriptLoaderFromElement(), or
// null for any other element.
ScriptElementBase* ToScriptElement(Element& element);

}  // namespace mitto

#endif  // MITTO_DOM_SCRIPT_ELEMENT_BASE_H_
