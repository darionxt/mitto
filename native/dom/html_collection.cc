#include "mitto/dom/html_collection.h"

#include <algorithm>

#include "mitto/bindings/v8_html_collection.h"
#include "mitto/dom/element.h"
#include "mitto/dom/node.h"

namespace mitto {

// static
HTMLCollection* HTMLCollection::Create(Node* root, Type type) {
  return bindings::MakeGarbageCollected<HTMLCollection>(root, type);
}

// static
bindings::ScriptWrappable* HTMLCollection::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<HTMLCollection>(
      info, bindings::V8HTMLCollection::kWrapperTypeInfo, nullptr,
      Type::kEmpty);
}

HTMLCollection::HTMLCollection(Node* root, Type type)
    : root_(root), type_(type) {}

HTMLCollection::~HTMLCollection() = default;

const bindings::WrapperTypeInfo* HTMLCollection::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLCollection::kWrapperTypeInfo;
}

void HTMLCollection::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(root_);
}

// Blink's IsMatchingHTMLElement() for each document collection.
bool HTMLCollection::Matches(const Element& element) const {
  switch (type_) {
    case Type::kEmpty:
    case Type::kDocApplets:
      return false;
    case Type::kDocImages:
      return element.HasHTMLTagName(u"img");
    case Type::kDocEmbeds:
      return element.HasHTMLTagName(u"embed");
    case Type::kDocLinks:
      return (element.HasHTMLTagName(u"a") ||
              element.HasHTMLTagName(u"area")) &&
             element.FindAttribute(u"href");
    case Type::kDocForms:
      return element.HasHTMLTagName(u"form");
    case Type::kDocScripts:
      return element.HasHTMLTagName(u"script");
    case Type::kDocAnchors:
      return element.HasHTMLTagName(u"a") && element.FindAttribute(u"name");
    case Type::kChildren:
      return true;
  }
}

std::vector<Element*> HTMLCollection::Elements() const {
  std::vector<Element*> elements;
  if (!root_ || type_ == Type::kEmpty || type_ == Type::kDocApplets) {
    return elements;
  }
  if (type_ == Type::kChildren) {
    for (Node* child = root_->FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsElementNode()) {
        elements.push_back(static_cast<Element*>(child));
      }
    }
    return elements;
  }
  for (Node* node = root_->FirstChild(); node;
       node = node->NextInTree(root_.Get())) {
    if (node->IsElementNode() && Matches(*static_cast<Element*>(node))) {
      elements.push_back(static_cast<Element*>(node));
    }
  }
  return elements;
}

uint32_t HTMLCollection::Length() const {
  return static_cast<uint32_t>(Elements().size());
}

Element* HTMLCollection::Item(uint32_t index) const {
  const std::vector<Element*> elements = Elements();
  return index < elements.size() ? elements[index] : nullptr;
}

// The DOM standard's namedItem(): the first element whose ID is `name`, or of
// the HTML namespace whose name attribute is.
Element* HTMLCollection::NamedItem(const std::u16string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  const std::vector<Element*> elements = Elements();
  for (Element* element : elements) {
    const std::u16string* id = element->FindAttribute(u"id");
    if (id && *id == name) {
      return element;
    }
  }
  for (Element* element : elements) {
    if (!element->IsHTMLElement()) {
      continue;
    }
    const std::u16string* element_name = element->FindAttribute(u"name");
    if (element_name && *element_name == name) {
      return element;
    }
  }
  return nullptr;
}

std::vector<std::u16string> HTMLCollection::NamedPropertyEnumerator() const {
  std::vector<std::u16string> names;
  const auto add = [&names](const std::u16string* name) {
    if (name && !name->empty() &&
        std::find(names.begin(), names.end(), *name) == names.end()) {
      names.push_back(*name);
    }
  };
  for (Element* element : Elements()) {
    add(element->FindAttribute(u"id"));
    if (element->IsHTMLElement()) {
      add(element->FindAttribute(u"name"));
    }
  }
  return names;
}

bool HTMLCollection::NamedPropertyQuery(const std::u16string& name) const {
  return NamedItem(name) != nullptr;
}

}  // namespace mitto
