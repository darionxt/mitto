// What an element's state says for the pseudo-classes that read it, such as
// :checked, :disabled, :link or :dir(), as Blink's elements answer them for
// a document that no one has typed in, clicked or focused.
//
// Each answer comes from the element's tag name, attributes and tree, as
// Blink's classes of form controls, links and media work it out until a
// script changes their state, since the Runtime has no such classes yet.
// When the forms of the HTML Standard come (roadmap, M8), each function here
// asks the element instead.
//
// Upstream: third_party/blink/renderer/core/dom/element.cc
// Upstream: third_party/blink/renderer/core/html/html_element.cc
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/html_form_control_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/html_input_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/html_select_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/html_option_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/text_control_element.cc

#ifndef MITTO_DOM_PSEUDO_CLASS_STATE_H_
#define MITTO_DOM_PSEUDO_CLASS_STATE_H_

#include <string>

namespace mitto {

class Element;

namespace pseudo_class_state {

// :link and :any-link.
bool IsLink(const Element& element);
// :enabled and :disabled. An element may be neither.
bool MatchesEnabled(const Element& element);
bool MatchesDisabled(const Element& element);
bool IsChecked(const Element& element);
bool IsDefault(const Element& element);
bool IsIndeterminate(const Element& element);
// :required and :optional. An element may be neither.
bool IsRequired(const Element& element);
bool IsOptional(const Element& element);
// :read-write and :read-only. An SVG or MathML element is neither.
bool IsReadWrite(const Element& element);
bool IsReadOnly(const Element& element);
bool IsPlaceholderShown(const Element& element);
// :valid and :invalid. An element may be neither.
bool IsValid(const Element& element);
bool IsInvalid(const Element& element);
// :in-range and :out-of-range. An element may be neither.
bool IsInRange(const Element& element);
bool IsOutOfRange(const Element& element);
bool IsDefined(const Element& element);
bool IsOpen(const Element& element);
// A dialog that showModal() opened, as no fullscreen element is.
bool IsModal(const Element& element);
// :-internal-text-field, which Blink lets an author's selector name.
bool IsTextField(const Element& element);
// :dir(rtl) for true, :dir(ltr) for false.
bool IsRightToLeft(const Element& element);
// The language that :lang() compares, or "" for none.
std::u16string InheritedLanguage(const Element& element);

}  // namespace pseudo_class_state

}  // namespace mitto

#endif  // MITTO_DOM_PSEUDO_CLASS_STATE_H_
