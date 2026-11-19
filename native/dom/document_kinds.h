// The trees that Blink's documents of a JSON, an image and a media response
// build in place of parsing markup, as a top-level frame of Windows Chrome
// shows them: JSON's text in a pre, an image in an img, and audio or video in
// a video element, with Blink's attributes and inline styles. An image's
// size comes from its header, which titles its document, but no image is
// decoded nor any media played, so what of the img's style follows its size
// or whether it moves is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/html/json_document.cc
// Upstream: third_party/blink/renderer/core/html/image_document.cc
// Upstream: third_party/blink/renderer/core/html/media/media_document.cc

#ifndef MITTO_DOM_DOCUMENT_KINDS_H_
#define MITTO_DOM_DOCUMENT_KINDS_H_

namespace mitto {

class Document;
class Element;
class Resource;

// JSONDocumentParser::CreateDocumentStructure(): html, a head with metas of
// the color scheme and the charset, and a body with the pre that holds the
// text and the container of the formatter's control, whose user agent shadow
// root a page cannot reach. Returns the pre.
Element& BuildJsonDocument(Document& document);

// ImageDocument::CreateDocumentStructure() for the outermost frame, which
// shrinks the image to fit: html, a head with the viewport's meta, and a body
// with the img of the document's URL, with their inline styles, whose image
// is `content`, the document's own response, which fetches nothing.
void BuildImageDocument(Document& document, Resource& content);
// ImageDocumentParser::Finish() once `content` has all of the body:
// UpdateTitle(), the file's name and the image's size, as its header gives
// it, and ImageLoaded(), the img's style of an image that has loaded and does
// not move, as Chrome gives it whether or not the image decodes. An animated
// image's, which has no transition, is the Known Gap.
void ImageDocumentLoaded(Document& document, const Resource& content);

// MediaDocumentParser::CreateDocumentStructure(): html, a head with the
// viewport's meta, and a body with a video element that plays the document's
// URL, of the document's MIME type, for audio too.
void BuildMediaDocument(Document& document);

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_KINDS_H_
