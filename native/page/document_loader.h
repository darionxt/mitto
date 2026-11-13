// Loads the Page's document from the response of the navigation that its
// Session makes when the Page opens, on the Page program's main thread: a new
// context for it, the HTML parser fed as the body arrives, decoded with the
// encoding that Chrome works out for it (text_resource_decoder_builder.h), the
// scripts that the parser runs, and the document's readiness with
// DOMContentLoaded and load, as Blink's HTMLDocumentParser and
// HTMLParserScriptRunner have them.
//
// The parser takes the body from tasks of the networking task source, and
// yields after a budget of time, as Chrome's does. It runs an inline script
// as soon as it reaches its end tag, and waits for a parser-blocking one,
// which goes on from a task of a very high priority once the script is
// ready, and the parser after it from a later task. Deferred scripts run
// after parsing, each from a task of its own, then DOMContentLoaded fires,
// and load once nothing holds it back, such as an async script.
//
// Upstream: third_party/blink/renderer/core/loader/document_loader.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_document_parser.cc
// Upstream: third_party/blink/renderer/core/script/html_parser_script_runner.cc
// Upstream: third_party/blink/renderer/core/dom/document.cc

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
class DocumentLoadTiming;
class HTMLScriptElement;
class HtmlDocumentParser;
class PendingScript;
class ScriptRunner;
class TextResourceDecoder;

class DocumentLoader final : public PageRequests::Client {
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

 private:
  // Gives the parser what the decoder decoded, after the document has taken
  // the decoder's encoding.
  void AppendDecoded(std::u16string text);

  // HTMLDocumentParser::SchedulePumpTokenizer(): the parser goes on from a
  // task of the networking task source.
  void SchedulePumpTokenizer();
  // PumpTokenizerIfPossible(): feeds the parser what has arrived until it
  // yields, waits for a script, or needs more, and ends the parse once all
  // of the body is in.
  void PumpTokenizerIfPossible();
  // PumpTokenizer(): returns whether the parser yielded.
  bool PumpTokenizer();
  // Whether the parser waits for a parser-blocking script.
  bool IsPaused() const { return parser_blocking_script_; }

  // HTMLParserScriptRunner::ProcessScriptElement(): prepares a script whose
  // end tag the parser reached.
  void ProcessScriptElement(HTMLScriptElement& element);
  // ExecuteParsingBlockingScripts(): runs the parser-blocking script once it
  // is ready.
  void ExecuteParsingBlockingScripts();
  // A script that the parser waits for is ready: the parser goes on from a
  // task of a very high priority, as Blink posts NotifyScriptLoaded().
  void PendingScriptFinished();
  void NotifyScriptLoaded();

  // The HTML Standard's "the end", once the parser has taken all of the body:
  // interactive, the deferred scripts, each from a task of its own, then
  // DOMContentLoaded, and load once nothing holds it back.
  void PrepareToStopParsing();
  void AttemptToRunDeferredScriptsAndEnd();
  // ExecuteScriptsWaitingForParsing(): returns whether every deferred script
  // has run.
  bool ExecuteScriptsWaitingForParsing();
  // Document::FinishedParsing().
  void FinishedParsing();
  // Document::CheckCompleted(): complete and load, once parsing has finished
  // and nothing holds the load event back.
  void CheckCompleted();

  const raw_ref<ScriptRunner> runner_;
  const raw_ref<PageRequests> requests_;
  const SendFrame send_;

  GURL url_;
  cppgc::Persistent<Document> document_;
  // How the document loads, which its window's Performance Timeline reads.
  cppgc::Persistent<DocumentLoadTiming> timing_;
  // How many bytes of the body have come, as decoded as the Page gets them.
  uint64_t body_bytes_ = 0;
  std::unique_ptr<HtmlDocumentParser> parser_;
  // Decodes the body as Blink's TextResourceDecoder does, with the encoding
  // that the response, the document or Chrome's detector gives.
  std::unique_ptr<TextResourceDecoder> decoder_;
  // Pieces of the body that have arrived and wait for the parser. The
  // Session has no credit for them until the parser takes them.
  base::circular_deque<std::string> waiting_body_;
  bool body_ended_ = false;
  bool parser_finished_ = false;
  bool pump_scheduled_ = false;
  // How often the parser has yielded, after which it has a longer budget.
  int times_yielded_ = 0;
  // The parser's script nesting level.
  int script_nesting_level_ = 0;
  // Once the parser has taken all of the body: the end has started, parsing
  // has finished with DOMContentLoaded, and load has fired.
  bool stopping_ = false;
  bool parsing_finished_ = false;
  bool load_fired_ = false;
  // The script that the parser waits for, if any.
  cppgc::Persistent<PendingScript> parser_blocking_script_;
  // The scripts that run after parsing, in document order.
  base::circular_deque<cppgc::Persistent<PendingScript>> scripts_after_parsing_;
  base::WeakPtrFactory<DocumentLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_DOCUMENT_LOADER_H_
