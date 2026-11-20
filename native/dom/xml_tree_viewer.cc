#include "mitto/dom/xml_tree_viewer.h"

#include <string>
#include <string_view>

#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/processing_instruction.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// DocumentXMLTreeViewer.css, as Blink's resource holds it, which the style
// element's text is.
constexpr char16_t kStyleSheet[] =
    uR"css(/* Copyright 2014 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

:root {
  color-scheme: light dark;
}

div.header {
    border-bottom: 2px solid black;
    padding-bottom: 5px;
    margin: 10px;
}

@media (prefers-color-scheme: dark) {
  div.header {
    border-bottom: 2px solid white;
  }
}

div.folder > div.hidden {
    display:none;
}

div.folder > span.hidden {
    display:none;
}

.pretty-print {
    margin-top: 1em;
    margin-left: 20px;
    font-family: monospace;
    font-size: 13px;
}

#webkit-xml-viewer-source-xml {
    display: none;
}

.opened {
    margin-left: 1em;
}

.comment {
    white-space: pre;
}

.cdata {
    white-space: pre-wrap;
}

.space-preserve {
    white-space: pre-wrap;
}

.space-default {
    white-space: normal;
}

.folder-button {
    user-select: none;
    cursor: pointer;
    display: inline-block;
    margin-left: -10px;
    width: 10px;
    background-repeat: no-repeat;
    background-position: left top;
    vertical-align: bottom;
}

.fold {
    background: url("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' fill='%23909090' width='10' height='10'><path d='M0 0 L8 0 L4 7 Z'/></svg>");
    height: 10px;
}

.open {
    background: url("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' fill='%23909090' width='10' height='10'><path d='M0 0 L0 8 L7 4 Z'/></svg>");
    height: 10px;
}
)css";

constexpr char16_t kSourceId[] = u"webkit-xml-viewer-source-xml";

Element* CreateHtmlElement(Document& document, const std::u16string& name) {
  return document.CreateElement(html_names::kXhtmlNamespace, std::nullopt,
                                name);
}

void Append(Node& parent, Node* child) {
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  parent.AppendChild(child, exception_state);
}

// classList.add() of each class in turn, on an element with none.
void AddClasses(Element& element, std::u16string_view classes) {
  element.SetAttributeWithoutValidation(u"class", std::u16string(classes));
}

void SetText(Node& node, const std::u16string& text) {
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  node.SetTextContentForBinding(text, exception_state);
}

Element* CreateLine(Document& document) {
  Element* const line = CreateHtmlElement(document, u"div");
  AddClasses(*line, u"line");
  return line;
}

Element* CreateText(Document& document, const std::u16string& value) {
  Element* const text = CreateHtmlElement(document, u"span");
  SetText(*text, value);
  return text;
}

Element* CreateComment(Document& document, const std::u16string& value) {
  Element* const comment = CreateHtmlElement(document, u"span");
  AddClasses(*comment, u"comment html-comment");
  SetText(*comment, value);
  return comment;
}

Element* CreateAttribute(Document& document, const Element::Attribute& node) {
  Element* const attribute = CreateHtmlElement(document, u"span");
  AddClasses(*attribute, u"html-attribute");
  Element* const name = CreateHtmlElement(document, u"span");
  AddClasses(*name, u"html-attribute-name");
  SetText(*name, node.name.ToString());
  Element* const value = CreateHtmlElement(document, u"span");
  AddClasses(*value, u"html-attribute-value");
  SetText(*value, node.value);
  Append(*attribute, document.CreateTextNode(u" "));
  Append(*attribute, name);
  Append(*attribute, document.CreateTextNode(u"=\""));
  Append(*attribute, value);
  Append(*attribute, document.CreateTextNode(u"\""));
  return attribute;
}

Element* CreateTag(Document& document,
                   const Element& node,
                   bool is_closing,
                   bool is_empty) {
  Element* const tag = CreateHtmlElement(document, u"span");
  AddClasses(*tag, u"html-tag");
  Append(*tag,
         document.CreateTextNode((is_closing ? u"</" : u"<") + node.TagName()));
  if (!is_closing) {
    for (const Element::Attribute& attribute : node.attributes()) {
      Append(*tag, CreateAttribute(document, attribute));
    }
  }
  Append(*tag, document.CreateTextNode(is_empty ? u"/>" : u">"));
  return tag;
}

void ApplyXmlSpace(Element& element, const Element& node) {
  const std::u16string* const value = node.FindAttribute(QualifiedName{
      std::nullopt, u"space", std::u16string(html_names::kXmlNamespace)});
  if (value && *value == u"preserve") {
    AddClasses(element, u"space-preserve");
  } else if (value && *value == u"default") {
    AddClasses(element, u"space-default");
  }
}

class TreeBuilder {
 public:
  TreeBuilder(Document& document, Element& source, Element& header_span)
      : document_(document), source_(source), header_span_(header_span) {}

  void ProcessNode(Node& parent, Node& node) {
    switch (node.GetNodeType()) {
      case Node::kProcessingInstructionNode:
        ProcessProcessingInstruction(parent,
                                     static_cast<ProcessingInstruction&>(node));
        break;
      case Node::kElementNode:
        ProcessElement(parent, static_cast<Element&>(node));
        break;
      case Node::kCommentNode: {
        Element* const line = CreateLine(*document_);
        Append(*line, CreateComment(
                          *document_,
                          u"<!-- " + static_cast<CharacterData&>(node).Data() +
                              u" -->"));
        Append(parent, line);
        break;
      }
      case Node::kTextNode: {
        const std::u16string& data = static_cast<CharacterData&>(node).Data();
        // Text of white space alone shows nothing.
        if (data.find_first_not_of(u" \t\n\r\f\v\u00a0\u1680\u2000\u2001"
                                   u"\u2002\u2003\u2004\u2005\u2006\u2007"
                                   u"\u2008\u2009\u200a\u2028\u2029\u202f"
                                   u"\u205f\u3000\ufeff") ==
            std::u16string::npos) {
          break;
        }
        Append(parent, CreateText(*document_, data));
        break;
      }
      case Node::kCdataSectionNode: {
        Element* const line = CreateLine(*document_);
        Element* const cdata = CreateHtmlElement(*document_, u"span");
        AddClasses(*cdata, u"cdata");
        SetText(*cdata, u"<![CDATA[ " +
                            static_cast<CharacterData&>(node).Data() + u" ]]>");
        Append(*line, cdata);
        Append(parent, line);
        break;
      }
      default:
        break;
    }
  }

 private:
  void ProcessElement(Node& parent, Element& node) {
    Node* const first = node.FirstChild();
    if (!first) {
      Element* const line = CreateLine(*document_);
      Append(*line, CreateTag(*document_, node, false, true));
      Append(parent, line);
    } else if (first->GetNodeType() == Node::kTextNode &&
               !first->NextSibling()) {
      Element* const line = CreateLine(*document_);
      ApplyXmlSpace(*line, node);
      Append(*line, CreateTag(*document_, node, false, false));
      Append(*line, CreateText(*document_,
                               static_cast<CharacterData*>(first)->Data()));
      Append(*line, CreateTag(*document_, node, true, false));
      Append(parent, line);
    } else {
      // createFolder().
      Element* const folder = CreateHtmlElement(*document_, u"div");
      AddClasses(*folder, u"folder");
      Element* const start = CreateLine(*document_);
      Element* const button = CreateHtmlElement(*document_, u"span");
      AddClasses(*button, u"folder-button fold");
      Append(*start, button);
      Append(*folder, start);
      Element* const opened = CreateHtmlElement(*document_, u"div");
      AddClasses(*opened, u"opened");
      Append(*folder, opened);
      Element* const folded = CreateText(*document_, u"...");
      AddClasses(*folded, u"folded hidden");
      Append(*folder, folded);
      Element* const end = CreateLine(*document_);
      Append(*folder, end);
      ApplyXmlSpace(*folder, node);
      Append(*start, CreateTag(*document_, node, false, false));
      for (Node* child = node.FirstChild(); child;
           child = child->NextSibling()) {
        ProcessNode(*opened, *child);
      }
      Append(*end, CreateTag(*document_, node, true, false));
      Append(parent, folder);
    }
  }

  void ProcessProcessingInstruction(Node& parent, ProcessingInstruction& node) {
    bool in_prolog = node.ParentNode() == &*source_;
    for (Node* sibling = node.PreviousSibling(); in_prolog && sibling;
         sibling = sibling->PreviousSibling()) {
      if (sibling->IsElementNode()) {
        in_prolog = false;
      }
    }
    Element* const line = CreateLine(*document_);
    Append(*line, CreateComment(*document_, u"<?" + node.Target() + u" " +
                                                node.Data() + u"?>"));
    Append(parent, line);
    if (in_prolog && node.Target() == u"xml-stylesheet") {
      // What headerSpan.innerHTML sets.
      SetText(*header_span_,
              u"This document cannot be formatted as intended. It uses XSLT, "
              u"which the browser does not support. You might be able to ");
      Element* const link = CreateHtmlElement(*document_, u"a");
      link->SetAttributeWithoutValidation(u"target", u"_blank");
      link->SetAttributeWithoutValidation(u"rel", u"noopener noreferrer");
      link->SetAttributeWithoutValidation(
          u"href", u"https://chromewebstore.google.com/search/XSLT%20Polyfill");
      SetText(*link, u"install a browser extension");
      Append(*header_span_, link);
      Append(*header_span_,
             document_->CreateTextNode(u" that allows you to view it."));
    }
  }

  const raw_ref<Document> document_;
  const raw_ref<Element> source_;
  const raw_ref<Element> header_span_;
};

// sourceXMLLoaded(), at the window's load.
void FillTree(const cppgc::Persistent<Element>& tree,
              const cppgc::Persistent<Element>& header_span) {
  Document& document = tree->GetDocument();
  // Another viewer may have taken the source away.
  Element* const source = document.GetElementById(kSourceId);
  if (!source) {
    return;
  }
  TreeBuilder builder(document, *source, *header_span);
  for (Node* child = source->FirstChild(); child;
       child = child->NextSibling()) {
    builder.ProcessNode(*tree, *child);
  }
  // initButtons(): each element of the class folder, in tree order, is
  // folder0, folder1 and on. Its button's handlers, which fold it, are the
  // Known Gap.
  int index = 0;
  for (Node* node = document.FirstChild(); node;
       node = node->NextInTree(&document)) {
    if (!node->IsElementNode()) {
      continue;
    }
    auto* const element = static_cast<Element*>(node);
    const std::u16string* const classes = element->FindAttribute(u"class");
    if (!classes) {
      continue;
    }
    bool is_folder = false;
    size_t start = 0;
    while (start < classes->size() && !is_folder) {
      start = classes->find_first_not_of(u" \t\n\f\r", start);
      if (start == std::u16string::npos) {
        break;
      }
      const size_t end = classes->find_first_of(u" \t\n\f\r", start);
      is_folder = classes->substr(start, end - start) == u"folder";
      start = end;
    }
    if (is_folder) {
      element->SetAttributeWithoutValidation(
          u"id", u"folder" + base::NumberToString16(index++));
    }
  }
}

}  // namespace

bool HasNoStyleInformation(const Document& document) {
  return !document.SawElementsInKnownNamespaces() && document.DefaultView();
}

void TransformDocumentToXmlTreeView(Document& document) {
  // prepareWebKitXMLViewer() with no root of its own.
  Element* const html = CreateHtmlElement(document, u"html");
  Element* const head = CreateHtmlElement(document, u"head");
  Append(*html, head);
  Element* const style = CreateHtmlElement(document, u"style");
  style->SetAttributeWithoutValidation(u"id", u"xml-viewer-style");
  Append(*head, style);
  Element* const body = CreateHtmlElement(document, u"body");
  Append(*html, body);
  Element* const source = CreateHtmlElement(document, u"div");
  source->SetAttributeWithoutValidation(u"id", kSourceId);
  Append(*body, source);
  while (Node* const child = document.FirstChild()) {
    document.ParserRemoveChild(*child);
    if (child->GetNodeType() != Node::kDocumentTypeNode) {
      Append(*source, child);
    }
  }
  Append(document, html);
  Element* const header = CreateHtmlElement(document, u"div");
  Append(*body, header);
  AddClasses(*header, u"header");
  Element* const header_span = CreateHtmlElement(document, u"span");
  Append(*header, header_span);
  SetText(*header_span,
          u"This XML file does not appear to have any style information "
          u"associated with it. The document tree is shown below.");
  Append(*header, CreateHtmlElement(document, u"br"));
  Element* const tree = CreateHtmlElement(document, u"div");
  Append(*body, tree);
  AddClasses(*tree, u"pretty-print");
  document.RunBeforeLoadEvent(
      base::BindOnce(&FillTree, cppgc::Persistent<Element>(tree),
                     cppgc::Persistent<Element>(header_span)));
  // SetXMLViewerStyle().
  SetText(*style, kStyleSheet);
}

}  // namespace mitto
