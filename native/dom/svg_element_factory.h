// Makes the element of the SVG namespace that a name calls for, as Blink's
// SVGElementFactory and Document::CreateRawElement() make it.

#ifndef MITTO_DOM_SVG_ELEMENT_FACTORY_H_
#define MITTO_DOM_SVG_ELEMENT_FACTORY_H_

#include <string>

namespace mitto {

class Document;
class SVGElement;

// An element named `local_name`, which is case-sensitive, of SVG's interface
// for that name, or an SVGElement for a name that SVG does not know, as
// Chrome's SVGUnknownElement is one.
SVGElement* CreateSVGElement(Document& document,
                             const std::u16string& local_name);

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ELEMENT_FACTORY_H_
