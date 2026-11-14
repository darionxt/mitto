// A document's HTML parser with the scripts that it reaches, as Blink's
// HTMLDocumentParser, HTMLParserScriptRunner and HTMLDocumentParserState have
// them: the parser of a navigation's response, or the script-created parser
// that document.open() makes, which document.write() feeds.
//
// The parser of a response takes the body from tasks of the networking task
// source, and yields after a budget of time, as Chrome's does. It runs an
// inline script as soon as it reaches its end tag, and waits for a
// parser-blocking one, which goes on from a task of a very high priority once
// the script is ready, and the parser after it from a later task. Deferred
// scripts run after parsing, each from a task of its own, then the document
// finishes parsing (Document::FinishedParsing()). A script-created parser
// parses what it is given at once, as Blink's kForceSynchronousParsing does.
//
// While a script that the parser reached runs, the parser has an insertion
// point just after the script's end tag, and its script nesting level is one
// deeper: what the script writes goes in there and is parsed at once, with any
// scripts in it, before the script goes on. A parser-blocking script that a
// script writes makes the parser wait once the scripts around it are done.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_document_parser.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_document_parser_state.h
// Upstream: third_party/blink/renderer/core/html/parser/html_input_stream.h
// Upstream: third_party/blink/renderer/core/html/parser/html_parser_reentry_permit.h
// Upstream: third_party/blink/renderer/core/script/html_parser_script_runner.cc
// Upstream: third_party/blink/renderer/core/dom/document_parser.cc

#ifndef MITTO_DOM_DOCUMENT_PARSER_H_
#define MITTO_DOM_DOCUMENT_PARSER_H_

#include <memory>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class DocWrittenScriptScanner;
class Document;
class HTMLScriptElement;
class HtmlDocumentParser;
class PendingScript;

class DocumentParser final : public cppgc::GarbageCollected<DocumentParser> {
 public:
  // What feeds the parser of a navigation's response: the Page's loader,
  // which decodes the body only as the parser takes it.
  class Client {
   public:
    virtual ~Client() = default;
    // Gives the parser, through AppendData(), the next piece of the body that
    // has arrived, or once all of it has, what the decoder still held, then
    // calls FinishData(). Returns false if nothing waits.
    virtual bool FeedParser() = 0;
    // The parser has stopped for good, and takes no more of the body.
    virtual void ParserDetached() = 0;
  };

  // The parser of `document`'s response, which `client` feeds, and which the
  // document holds until it has loaded.
  DocumentParser(Document& document, Client* client);
  ~DocumentParser();

  void Trace(cppgc::Visitor* visitor) const;

  // For the client.
  //
  // More of the body has arrived (HTMLDocumentParser::Append()): the parser
  // takes it from a task of the networking task source.
  void DidReceiveData();
  // In FeedParser(): adds what was decoded, and ends the input.
  void AppendData(std::u16string_view text);
  void FinishData();

  // HTMLDocumentParser::Finish(): no more input comes, so the parser ends
  // once it has taken all of it, as the end of the body does for the parser
  // of a response and document.close() for a script-created one.
  void Finish();

  // What document.write() and document.open() ask.
  //
  // Whether the parser was made by document.open().
  bool WasCreatedByScript() const { return created_by_script_; }
  // The HTML Standard's insertion point is defined: while a script that the
  // parser reached runs, and in a script-created parser until its input
  // ends.
  bool HasInsertionPoint() const;
  // HTMLDocumentParser::insert(): parses `source` at the insertion point,
  // with the scripts that it holds.
  void Insert(std::u16string_view source);
  // Whether the parser's script nesting level is above zero.
  bool IsExecutingScript() const { return script_nesting_level_ > 0; }
  // DocumentParser's states: parsing until the end begins, then stopping
  // until the document has finished parsing, then stopped, and detached once
  // it has let go of its document.
  bool IsParsing() const { return state_ == State::kParsing; }
  bool IsStopping() const { return state_ == State::kStopping; }
  bool IsStopped() const { return state_ >= State::kStopped; }
  bool IsDetached() const { return state_ == State::kDetached; }
  // HTMLDocumentParser::Detach(): stops for good, drops the scripts it waits
  // for, and lets go of the document.
  void Detach();

 private:
  enum class State {
    kParsing,
    kStopping,
    kStopped,
    kDetached,
  };

  // HTMLDocumentParserState's DeferredParserState.
  enum class Scheduled {
    kNotScheduled,
    kScheduled,
    kScheduledWithEndIfDelayed,
  };

  class ScopedCounter;
  class InsertionPointRecord;

  // HTMLDocumentParserState's.
  bool IsSynchronous() const { return created_by_script_; }
  bool ShouldComplete() const {
    return should_complete_ > 0 || created_by_script_;
  }
  bool ShouldEndIfDelayed() const { return end_if_delayed_forbidden_ == 0; }
  bool InPumpSession() const { return pump_session_nesting_level_ > 0; }
  bool InNestedPumpSession() const { return pump_session_nesting_level_ > 1; }
  bool IsScheduled() const { return scheduled_ != Scheduled::kNotScheduled; }

  // Whether the parser waits for a parser-blocking script. Blink's parser
  // pause flag is never set in the Engine Release.
  bool IsPaused() const { return parser_blocking_script_; }

  void PostTask(TaskType type, void (DocumentParser::*method)());
  void SchedulePumpTokenizer();
  void ScheduleEndIfDelayed();
  void DeferredPumpTokenizerIfPossible();
  void PumpTokenizerIfPossible();
  // Returns whether the parser yielded.
  bool PumpTokenizer();
  void ResumeParsingAfterPause();
  // ShouldDelayEnd(), AttemptToEnd(), EndIfDelayed().
  bool ShouldDelayEnd() const;
  void AttemptToEnd();
  void EndIfDelayed();
  // The HTML Standard's "the end", once the parser has taken all of its
  // input: interactive, the deferred scripts, each from a task of its own,
  // then the document finishes parsing.
  void PrepareToStopParsing();
  void AttemptToRunDeferredScriptsAndEnd();
  void End();
  void StopParsing();

  // HTMLParserScriptRunner's.
  void ProcessScriptElement(HTMLScriptElement& element);
  void ProcessScriptElementInternal(HTMLScriptElement& element);
  void ExecuteParsingBlockingScripts();
  void ExecutePendingParserBlockingScriptAndDispatchEvent();
  void PendingScriptFinished();
  // HTMLDocumentParser::NotifyScriptLoaded(), from a task of a very high
  // priority.
  void NotifyScriptLoaded();
  // Returns whether every deferred script has run.
  bool ExecuteScriptsWaitingForParsing();
  void ContinueScriptsWaitingForParsing();

  cppgc::Member<Document> document_;
  // The loader that feeds the parser of a response, which outlives it while
  // it is attached; null for a script-created parser.
  raw_ptr<Client> client_;
  const bool created_by_script_;
  // The tokenizer and the tree builder, until the parser is detached and
  // nothing on the stack uses them, as a script that runs may still have an
  // insertion point in them once the parser has stopped.
  std::unique_ptr<HtmlDocumentParser> tree_;
  // What writes gave while the parser waits for a script, scanned as Blink's
  // insertion preload scanner does, until the parser goes on.
  std::unique_ptr<DocWrittenScriptScanner> insertion_scanner_;
  State state_ = State::kParsing;

  // HTMLDocumentParserState.
  Scheduled scheduled_ = Scheduled::kNotScheduled;
  int end_if_delayed_forbidden_ = 0;
  int should_complete_ = 0;
  int pump_session_nesting_level_ = 0;
  // How often the parser has yielded, after which it has a longer budget.
  int times_yielded_ = 0;
  bool should_attempt_to_end_on_eof_ = false;
  bool end_was_delayed_ = false;

  // HTMLParserReentryPermit's script nesting level.
  int script_nesting_level_ = 0;

  // HTMLParserScriptRunner's: the script that the parser waits for, and the
  // scripts that run after parsing, in document order.
  cppgc::Member<PendingScript> parser_blocking_script_;
  HeapVector<PendingScript> scripts_after_parsing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_PARSER_H_
