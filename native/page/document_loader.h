// Loads the Page's document from the response of the navigation that its
// Session makes when the Page opens, on the Page program's main thread: a new
// context for it, and the document's parser (document_parser.h), which this
// feeds the body as the parser takes it, decoded with the encoding that Chrome
// works out for it (text_resource_decoder_builder.h). What arrives waits
// until the parser takes it, and the Session has no credit for it until then.
//
// Upstream: third_party/blink/renderer/core/loader/document_loader.cc

#ifndef MITTO_PAGE_DOCUMENT_LOADER_H_
#define MITTO_PAGE_DOCUMENT_LOADER_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/document_parser.h"
#include "mitto/page/page_requests.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class Document;
class DocumentLoadTiming;
class ScriptRunner;
class TextResourceDecoder;

class DocumentLoader final : public PageRequests::Client,
                             public DocumentParser::Client {
 public:
  DocumentLoader(ScriptRunner& runner, PageRequests& requests, SendFrame send);

  DocumentLoader(const DocumentLoader&) = delete;
  DocumentLoader& operator=(const DocumentLoader&) = delete;

  ~DocumentLoader() override;

  // PageRequests::Client, for the navigation:
  void OnResponseStarted(const PageResponseStarted& response) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseTotals(const PageResponseEnded& ended) override;
  void OnResponseEnded(int net_error) override;

  // DocumentParser::Client:
  bool FeedParser() override;
  void ParserDetached() override;

 private:
  // Gives the parser what the decoder decoded, after the document has taken
  // the decoder's encoding.
  void AppendDecoded(std::u16string text);
  // Tells the Session that the window's load event has been dispatched.
  void DidDispatchLoadEvent();

  const raw_ref<ScriptRunner> runner_;
  const raw_ref<PageRequests> requests_;
  const SendFrame send_;

  GURL url_;
  cppgc::Persistent<Document> document_;
  // How the document loads, which its window's Performance Timeline reads.
  cppgc::Persistent<DocumentLoadTiming> timing_;
  // How many bytes of the body have come, as decoded as the Page gets them.
  uint64_t body_bytes_ = 0;
  // The document's parser, until it stops for good, as when the document has
  // loaded or document.open() has replaced it.
  cppgc::Persistent<DocumentParser> parser_;
  // Decodes the body as Blink's TextResourceDecoder does, with the encoding
  // that the response, the document or Chrome's detector gives.
  std::unique_ptr<TextResourceDecoder> decoder_;
  // Pieces of the body that have arrived and wait for the parser.
  base::circular_deque<std::string> waiting_body_;
  bool body_ended_ = false;
  // Whether the parser has had all of the body.
  bool data_finished_ = false;
  base::WeakPtrFactory<DocumentLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_DOCUMENT_LOADER_H_
