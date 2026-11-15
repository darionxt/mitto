// What form controls reflect of their content attributes as Blink's form
// classes read them: the form's encoding type, method and action that
// formEnctype, formMethod and formAction give, the autofill value that
// autocomplete gives, and a text control's maxLength and minLength.
//
// Upstream: third_party/blink/renderer/core/html/forms/html_form_control_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/html_form_control_element_with_state.cc
// Upstream: third_party/blink/renderer/core/html/forms/text_control_element.cc
// Upstream: third_party/blink/renderer/core/loader/form_submission.cc

#ifndef MITTO_DOM_FORM_CONTROL_ATTRIBUTES_H_
#define MITTO_DOM_FORM_CONTROL_ATTRIBUTES_H_

#include <stdint.h>

#include <string>
#include <string_view>

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Element;

// FormSubmission::Attributes::ParseEncodingType(): multipart/form-data or
// text/plain in any case, else application/x-www-form-urlencoded.
std::u16string_view ParseFormEncodingType(std::u16string_view type);
// MethodString(ParseMethodType()): post or dialog in any case, else get.
std::u16string_view ParseFormMethod(std::u16string_view method);

// HTMLFormControlElement's formEnctype and formMethod: "" while the content
// attribute is missing.
std::u16string FormEnctypeOf(const Element& element);
std::u16string FormMethodOf(const Element& element);
// formAction and the form's action: the document's URL while the content
// attribute is missing or empty, else the attribute resolved, which an URL
// that does not resolve gives as the canonicalizer wrote it.
std::u16string FormActionOf(const Element& element, std::u16string_view name);

// IDLExposedAutofillValue(): the autocomplete attribute's autofill detail
// tokens, lowercased, or "" for tokens that do not make one. A hidden input
// gives "" for "on" and "off".
std::u16string ExposedAutofillValue(const Element& element,
                                    bool is_hidden_input);

// TextControlElement's maxLength and minLength: the content attribute's
// integer, or -1 for one that is missing, does not parse or is negative.
int32_t TextControlMaxLength(const Element& element);
int32_t TextControlMinLength(const Element& element);
void SetTextControlMaxLength(Element& element,
                             int32_t value,
                             bindings::ExceptionState& exception_state);
void SetTextControlMinLength(Element& element,
                             int32_t value,
                             bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_FORM_CONTROL_ATTRIBUTES_H_
