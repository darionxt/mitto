// Blink's XML tree viewer, which shows an XML document of a top-level frame
// that has no style information: the document's own nodes go into a hidden
// container under an HTML document with Blink's style sheet and a header, and
// at the window's load a tree of spans that shows them as markup, as Blink's
// DocumentXMLTreeViewer.js builds it in an isolated world. A page sees that
// tree, as in Chrome. Folding a part of it, which a mouse press on a button
// does in Chrome, is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/xml/document_xml_tree_viewer.cc
// Upstream: third_party/blink/renderer/core/xml/DocumentXMLTreeViewer.js
// Upstream: third_party/blink/renderer/core/xml/DocumentXMLTreeViewer.css

#ifndef MITTO_DOM_XML_TREE_VIEWER_H_
#define MITTO_DOM_XML_TREE_VIEWER_H_

namespace mitto {

class Document;

// HasNoStyleInformation(): whether the document of a navigation, which the
// XML parser has read with no error, no style sheet's and no XSLT's
// processing instruction, gets the viewer: a top-level document with no
// element of the HTML, SVG or MathML namespace.
bool HasNoStyleInformation(const Document& document);

// TransformDocumentToXMLTreeView() for a top-level frame: the viewer's frame
// at once, and its tree just before the window's load event.
void TransformDocumentToXmlTreeView(Document& document);

}  // namespace mitto

#endif  // MITTO_DOM_XML_TREE_VIEWER_H_
