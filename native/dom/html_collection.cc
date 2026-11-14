#include "mitto/dom/html_collection.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"
#include "mitto/bindings/v8_html_collection.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_tree_version.h"
#include "mitto/dom/element.h"
#include "mitto/dom/node.h"
#include "mitto/dom/qualified_name.h"

namespace mitto {

namespace {

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// The classes of a class attribute or of getElementsByClassName()'s
// argument, each once, as Blink's SpaceSplitString keeps them.
std::vector<std::u16string> SplitClasses(std::u16string_view text,
                                         bool fold_case) {
  std::vector<std::u16string> classes;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsHTMLSpace(text[start])) {
      ++start;
    }
    size_t end = start;
    while (end < text.size() && !IsHTMLSpace(text[end])) {
      ++end;
    }
    if (end > start) {
      std::u16string name(text.substr(start, end - start));
      if (fold_case) {
        name = base::ToLowerASCII(name);
      }
      if (std::find(classes.begin(), classes.end(), name) == classes.end()) {
        classes.push_back(std::move(name));
      }
    }
    start = end;
  }
  return classes;
}

void AppendRows(Node& section, HeapVector<Element>& rows) {
  for (Node* child = section.FirstChild(); child;
       child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(u"tr")) {
      rows.push_back(static_cast<Element*>(child));
    }
  }
}

// Appends the rows of a table in the order of Blink's HTMLTableRowsCollection:
// those of each thead child, then the tr children and the rows of each tbody
// child, then the rows of each tfoot child.
void AppendTableRows(Node& table, HeapVector<Element>& rows) {
  for (Node* child = table.FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(u"thead")) {
      AppendRows(*child, rows);
    }
  }
  for (Node* child = table.FirstChild(); child; child = child->NextSibling()) {
    if (!child->IsElementNode()) {
      continue;
    }
    auto* const element = static_cast<Element*>(child);
    if (element->HasHTMLTagName(u"tr")) {
      rows.push_back(element);
    } else if (element->HasHTMLTagName(u"tbody")) {
      AppendRows(*child, rows);
    }
  }
  for (Node* child = table.FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(u"tfoot")) {
      AppendRows(*child, rows);
    }
  }
}

}  // namespace

bool IsAllNamedElement(const Element& element) {
  for (const std::u16string_view name :
       {u"a", u"button", u"embed", u"form", u"frame", u"frameset", u"iframe",
        u"img", u"input", u"map", u"meta", u"object", u"select", u"textarea"}) {
    if (element.HasHTMLTagName(name)) {
      return true;
    }
  }
  return false;
}

// static
HTMLCollection* HTMLCollection::Create(Node* root, Type type) {
  return bindings::MakeGarbageCollected<HTMLCollection>(root, type);
}

// static
HTMLCollection* HTMLCollection::CreateTagCollection(
    Node* root,
    const std::u16string& name) {
  // Blink makes an HTMLTagCollection in an HTML document.
  HTMLCollection* const collection = Create(
      root, root->GetDocument().IsHTMLDocument() ? Type::kHTMLTagCollection
                                                 : Type::kTagCollection);
  collection->name_ = name;
  return collection;
}

// static
HTMLCollection* HTMLCollection::CreateTagCollectionNS(
    Node* root,
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) {
  HTMLCollection* const collection = Create(root, Type::kTagCollectionNS);
  collection->name_ = local_name;
  collection->namespace_uri_ = namespace_uri;
  return collection;
}

// static
HTMLCollection* HTMLCollection::CreateClassCollection(
    Node* root,
    const std::u16string& class_names) {
  HTMLCollection* const collection = Create(root, Type::kClassCollection);
  collection->name_ = class_names;
  collection->class_names_ =
      SplitClasses(class_names, root->GetDocument().InQuirksMode());
  return collection;
}

// static
HTMLCollection* HTMLCollection::CreateDocumentAllNamedItems(
    Node* root,
    const std::u16string& name) {
  HTMLCollection* const collection = Create(root, Type::kDocumentAllNamedItems);
  collection->name_ = name;
  return collection;
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
  elements_.Trace(visitor);
}

// Blink's ElementMatches() of each kind of collection.
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
    case Type::kTableRows:
      return true;
    case Type::kTBodies:
      return element.HasHTMLTagName(u"tbody");
    case Type::kSectionRows:
      return element.HasHTMLTagName(u"tr");
    case Type::kRowCells:
      return element.HasHTMLTagName(u"td") || element.HasHTMLTagName(u"th");
    case Type::kTagCollection:
      return name_ == u"*" || name_ == element.TagQName().ToString();
    case Type::kHTMLTagCollection:
      if (name_ == u"*") {
        return true;
      }
      if (element.IsHTMLElement()) {
        return base::ToLowerASCII(name_) == element.TagQName().ToString();
      }
      return name_ == element.TagQName().ToString();
    case Type::kTagCollectionNS:
      if (name_ != u"*" && name_ != element.LocalName()) {
        return false;
      }
      return (namespace_uri_ && *namespace_uri_ == u"*") ||
             namespace_uri_ == element.NamespaceURI();
    case Type::kClassCollection: {
      if (class_names_.empty()) {
        return false;
      }
      const std::u16string* classes = element.FindAttribute(u"class");
      if (!classes) {
        return false;
      }
      const std::vector<std::u16string> element_classes =
          SplitClasses(*classes, element.GetDocument().InQuirksMode());
      return std::all_of(class_names_.begin(), class_names_.end(),
                         [&element_classes](const std::u16string& name) {
                           return std::find(element_classes.begin(),
                                            element_classes.end(),
                                            name) != element_classes.end();
                         });
    }
    case Type::kDocumentAllNamedItems: {
      if (IsAllNamedElement(element)) {
        const std::u16string* name = element.FindAttribute(u"name");
        if (name && *name == name_) {
          return true;
        }
      }
      const std::u16string* id = element.FindAttribute(u"id");
      return id && *id == name_;
    }
  }
}

const HeapVector<Element>& HTMLCollection::Elements() const {
  if (elements_version_ == DomTreeVersion()) {
    return elements_;
  }
  elements_.clear();
  elements_version_ = DomTreeVersion();
  if (!root_ || type_ == Type::kEmpty || type_ == Type::kDocApplets) {
    return elements_;
  }
  if (type_ == Type::kTableRows) {
    AppendTableRows(*root_, elements_);
    return elements_;
  }
  if (type_ == Type::kChildren || type_ == Type::kTBodies ||
      type_ == Type::kSectionRows || type_ == Type::kRowCells) {
    for (Node* child = root_->FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsElementNode() && Matches(*static_cast<Element*>(child))) {
        elements_.push_back(static_cast<Element*>(child));
      }
    }
    return elements_;
  }
  for (Node* node = root_->FirstChild(); node;
       node = node->NextInTree(root_.Get())) {
    if (node->IsElementNode() && Matches(*static_cast<Element*>(node))) {
      elements_.push_back(static_cast<Element*>(node));
    }
  }
  return elements_;
}

uint32_t HTMLCollection::Length() const {
  return static_cast<uint32_t>(Elements().size());
}

Element* HTMLCollection::Item(uint32_t index) const {
  const HeapVector<Element>& elements = Elements();
  return index < elements.size() ? elements[index] : nullptr;
}

// The DOM standard's namedItem(): the first element whose ID is `name`, or of
// the HTML namespace whose name attribute is.
Element* HTMLCollection::NamedItem(const std::u16string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  const std::vector<Element*> elements = Elements().ToVector();
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
  for (Element* element : Elements().ToVector()) {
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
