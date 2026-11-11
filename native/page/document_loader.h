// Loads the Page's document from the response of the navigation that its
// Session makes when the Page opens, on the Page program's main thread: a new
// context for it, the HTML parser fed as the body arrives, the document's
// classic scripts in document order, and its readiness with DOMContentLoaded
// and load, in their simplest form.
//
// A script without src runs as soon as the parser reaches its end tag. One
// with src is fetched through the Session as a script request, with the
// document as its Initiator, and the parser waits for it, as for a
// parser-blocking script, while other tasks run. One with src and defer or
// async is fetched at once and runs after parsing, in document order, before
// DOMContentLoaded: a schedule the HTML Standard allows, where Chrome's exact
// timing comes with its preload scanner. Scripts that the DOM inserts and
// module scripts do not run yet.
//
// Upstream: third_party/blink/renderer/core/loader/document_loader.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_document_parser.cc
// Upstream: third_party/blink/renderer/core/script/script_loader.cc
// Upstream: third_party/blink/renderer/core/script/script_runner.cc

#ifndef MITTO_PAGE_DOCUMENT_LOADER_H_
#define MITTO_PAGE_DOCUMENT_LOADER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/page/page_requests.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class Document;
class HTMLScriptElement;
class HtmlDocumentParser;
class ScriptRunner;

// Decodes UTF-8 that arrives in pieces, as the Encoding Standard's UTF-8
// decode does: a byte order mark at the start goes, and each byte sequence
// that is not UTF-8 becomes U+FFFD. A sequence cut between two pieces waits
// for the rest.
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_utf8.cc
class Utf8StreamDecoder {
 public:
  Utf8StreamDecoder();
  ~Utf8StreamDecoder();

  std::u16string Decode(std::string_view bytes);
  // What is left at the end of the stream.
  std::u16string Flush();

 private:
  std::string pending_;
  bool started_ = false;
};

class DocumentLoader final : public PageRequests::Client {
 public:
  DocumentLoader(ScriptRunner& runner, PageRequests& requests, SendFrame send);

  DocumentLoader(const DocumentLoader&) = delete;
  DocumentLoader& operator=(const DocumentLoader&) = delete;

  ~DocumentLoader() override;

  // PageRequests::Client, for the navigation:
  void OnResponseStarted(const PageResponseStarted& response) override;
  void OnResponseBody(std::string bytes) override;
  void OnResponseEnded(int net_error) override;

 private:
  class ScriptFetch;

  // Feeds the parser what has arrived, and runs each script it reaches, until
  // it waits for a script, needs more of the body, or has finished.
  void Pump();

  // The HTML Standard's "prepare the script element", for a script that the
  // parser inserted. Returns whether the parser waits for it.
  bool PrepareScript(HTMLScriptElement& element);

  // Runs a fetched script, or fires error at its element if it could not be
  // fetched, and then load for one that ran.
  void ExecuteFetched(ScriptFetch& fetch);

  // A script's fetch has ended.
  void OnScriptFetched(ScriptFetch& fetch);

  // The HTML Standard's "the end", once the parser has finished: interactive,
  // the scripts that run after parsing, DOMContentLoaded, complete and load.
  void RunAfterParsing();
  void FireDOMContentLoaded();
  void FireLoad();

  // Fires error at `element` from a later task, as the HTML Standard queues
  // it for a script whose URL cannot be fetched.
  void QueueErrorEvent(HTMLScriptElement& element);

  const raw_ref<ScriptRunner> runner_;
  const raw_ref<PageRequests> requests_;
  const SendFrame send_;

  GURL url_;
  cppgc::Persistent<Document> document_;
  std::unique_ptr<HtmlDocumentParser> parser_;
  Utf8StreamDecoder decoder_;
  // Pieces of the body that have arrived and wait for the parser, which waits
  // for a script. The Session has no credit for them until the parser takes
  // them.
  base::circular_deque<std::string> waiting_body_;
  bool body_ended_ = false;
  bool parser_finished_ = false;
  bool after_parsing_started_ = false;
  bool dom_content_loaded_queued_ = false;
  // The script that the parser waits for, if any.
  std::unique_ptr<ScriptFetch> blocking_script_;
  // The scripts that run after parsing, in document order.
  base::circular_deque<std::unique_ptr<ScriptFetch>> after_parsing_;
  base::WeakPtrFactory<DocumentLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_DOCUMENT_LOADER_H_
