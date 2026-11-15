#include "mitto/dom/script_element_base.h"

#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/svg_elements.h"

namespace mitto {

ScriptElementBase* ToScriptElement(Element& element) {
  // The element factories make each script element of the two namespaces
  // one of these.
  if (element.HasHTMLTagName(u"script")) {
    return static_cast<HTMLScriptElement*>(&element);
  }
  if (element.LocalName() == u"script" &&
      element.NamespaceURI() == html_names::kSvgNamespace) {
    return static_cast<SVGScriptElement*>(&element);
  }
  return nullptr;
}

}  // namespace mitto
