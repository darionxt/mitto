// Makes the element of the HTML namespace that a name calls for, as Blink's
// HTMLElementFactory and Document::CreateElementForBinding() make it.

#ifndef MITTO_DOM_HTML_ELEMENT_FACTORY_H_
#define MITTO_DOM_HTML_ELEMENT_FACTORY_H_

#include <string>

namespace mitto {

class Document;
class HTMLElement;

// An element named `local_name`, which is in lower case, of the HTML
// Standard's interface for that name: HTMLUnknownElement for a name the
// standard does not know and HTMLElement for a valid custom element name, as
// in Chrome. A name whose interface the Runtime lacks yet gets HTMLElement, a
// Known Gap.
HTMLElement* CreateHTMLElement(Document& document,
                               const std::u16string& local_name);

// Whether the HTML Standard knows `local_name` as the name of an HTML element,
// whatever its interface.
bool IsKnownHTMLElementName(const std::u16string& local_name);

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENT_FACTORY_H_
