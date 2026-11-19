#include "mitto/dom/document_kinds.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_image_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/image_header.h"
#include "mitto/dom/resource.h"
#include "url/gurl.h"
#include "url/third_party/mozilla/url_parse.h"
#include "url/url_util.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// An HTML element with `attributes` set in order, as Blink's setAttribute()
// calls set them.
Element* Make(
    Document& document,
    const std::u16string& local_name,
    std::vector<std::pair<std::u16string, std::u16string>> attributes = {}) {
  Element* const element = document.CreateElement(html_names::kXhtmlNamespace,
                                                  std::nullopt, local_name);
  for (const auto& [name, value] : attributes) {
    element->SetAttributeWithoutValidation(name, value);
  }
  return element;
}

void Append(Node& parent, Node* child) {
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  parent.AppendChild(child, exception_state);
}

}  // namespace

Element& BuildJsonDocument(Document& document) {
  Element* const html = Make(document, u"html");
  document.ParserAppendChild(html);
  Element* const head = Make(document, u"head");
  head->ParserAppendChild(
      Make(document, u"meta",
           {{u"name", u"color-scheme"}, {u"content", u"light dark"}}));
  head->ParserAppendChild(Make(document, u"meta", {{u"charset", u"utf-8"}}));
  html->ParserAppendChild(head);
  Element* const body = Make(document, u"body");
  html->ParserAppendChild(body);
  Element* const pre = Make(document, u"pre");
  body->ParserAppendChild(pre);
  body->ParserAppendChild(
      Make(document, u"div", {{u"class", u"json-formatter-container"}}));
  return *pre;
}

void BuildImageDocument(Document& document, Resource& content) {
  Element* const html = Make(document, u"html", {{u"style", u"height: 100%;"}});
  Append(document, html);
  Element* const head = Make(document, u"head");
  Append(*head, Make(document, u"meta",
                     {{u"name", u"viewport"},
                      {u"content", u"width=device-width, minimum-scale=0.1"}}));
  Element* const body = Make(
      document, u"body",
      {{u"style",
        u"margin: 0px; height: 100%; background-color: rgb(14, 14, 14);"}});
  document.WillInsertBody();
  // UpdateImageStyle() before the image's size is known, for a frame that
  // shrinks it to fit with the desktop's mode.
  Element* const image = Make(
      document, u"img",
      {{u"style", u"display: block;-webkit-user-select: none;margin: auto;"}});
  static_cast<HTMLImageElement*>(image)->StartLoadingImageDocument(content);
  Append(*body, image);
  Append(*html, head);
  Append(*html, body);
}

void ImageDocumentLoaded(Document& document, const Resource& content) {
  // UpdateTitle(): the decoded name of the URL's file, or else its host.
  if (content.HasImage() && content.Image()->width) {
    const GURL& url = content.Url();
    const std::string_view spec = url.possibly_invalid_spec();
    url::Component path = url.parsed_for_possibly_invalid_spec().path;
    // KURL::LastPathComponent(): "bar" of "/foo/bar/" too.
    if (path.len > 0 && spec[path.end() - 1] == '/') {
      --path.len;
    }
    url::Component file;
    url::ExtractFileName(spec, path, &file);
    std::u16string title;
    if (file.is_nonempty()) {
      title = url::UrlEscapeDecoder(spec.substr(file.begin, file.len),
                                    url::DecodeUrlMode::kUtf8OrIsomorphic)
                  .view();
    }
    if (title.empty()) {
      title = base::UTF8ToUTF16(url.host());
    }
    // ImageTitle(): "name (width×height)".
    title += u" (" + base::NumberToString16(content.Image()->width) +
             u"\u00D7" + base::NumberToString16(content.Image()->height) + u")";
    document.SetTitle(title);
  }
  Element* const body = document.Body();
  Element* const image = body ? body->FirstElementChild() : nullptr;
  if (!image || !image->HasHTMLTagName(u"img")) {
    return;
  }
  image->SetAttributeWithoutValidation(
      u"style",
      u"display: block;-webkit-user-select: none;margin: auto;"
      u"background-color: hsl(0, 0%, 90%);transition: background-color 300ms;");
}

void BuildMediaDocument(Document& document) {
  Element* const html = Make(document, u"html");
  Append(document, html);
  Element* const head = Make(document, u"head");
  Append(*head,
         Make(document, u"meta",
              {{u"name", u"viewport"}, {u"content", u"width=device-width"}}));
  Element* const media =
      Make(document, u"video",
           {{u"controls", u""}, {u"autoplay", u""}, {u"name", u"media"}});
  Element* const source = Make(
      document, u"source",
      {{u"src", document.UrlForBinding()}, {u"type", document.ContentType()}});
  if (GURL(document.UrlForBinding()).SchemeIsHTTPOrHTTPS()) {
    media->SetAttributeWithoutValidation(u"crossorigin", u"anonymous");
  }
  Append(*media, source);
  Element* const body = Make(document, u"body");
  document.WillInsertBody();
  Append(*body, media);
  Append(*html, head);
  Append(*html, body);
}

}  // namespace mitto
