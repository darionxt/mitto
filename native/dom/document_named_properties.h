// The named properties of a window's document: a property of the document's
// wrapper for each of its named items, whose value document[name] gives, as
// Blink's LocalWindowProxy keeps them. Blink writes Document's named getter
// without its bindings, so that reading the document's other properties never
// asks it: the wrapper gets an accessor when an element adds a name, which
// shadows the prototype's property of that name, and loses it once no
// element has the name.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc

#ifndef MITTO_DOM_DOCUMENT_NAMED_PROPERTIES_H_
#define MITTO_DOM_DOCUMENT_NAMED_PROPERTIES_H_

#include <string>

namespace mitto {

class Document;

// LocalWindowProxy::NamedItemAdded(): gives the wrapper of `document`, if it
// is its window's document, the property of `name`, unless an own property
// that cannot be configured, such as location, is there.
void NamedItemAdded(Document& document, const std::u16string& name);

// NamedItemRemoved(): takes the property of `name` away once the document has
// no such named item.
void NamedItemRemoved(Document& document, const std::u16string& name);

// For a document that has just become its window's, the property of each of
// its named items, in the order in which they came, as Blink gives each as
// an element adds it.
void AddNamedItemProperties(Document& document);

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_NAMED_PROPERTIES_H_
