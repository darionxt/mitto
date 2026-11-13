#include "mitto/page/document_loader.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/elapsed_timer.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/performance_timing_confidence.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "mitto/page/resource_timing.h"
#include "mitto/page/script_runner.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"

namespace mitto {
namespace {

// HTMLDocumentParser's timed budget: 10 milliseconds for each of the first
// six yields, and 500 after them, as Chrome has it off Android.
constexpr int kNumYieldsWithDefaultBudget = 6;
constexpr base::TimeDelta kDefaultParserBudget = base::Milliseconds(10);
constexpr base::TimeDelta kLongParserBudget = base::Milliseconds(500);

// The HTML Standard's perform a microtask checkpoint, as Blink's event loop
// performs one before the parser prepares a script.
void PerformMicrotaskCheckpoint() {
  v8::MicrotasksScope::PerformCheckpoint(v8::Isolate::GetCurrent());
}

}  // namespace

DocumentLoader::DocumentLoader(ScriptRunner& runner,
                               PageRequests& requests,
                               SendFrame send)
    : runner_(runner), requests_(requests), send_(std::move(send)) {}

DocumentLoader::~DocumentLoader() = default;

void DocumentLoader::OnResponseStarted(const PageResponseStarted& response) {
  // The document of the response takes the place of about:blank's, in a
  // context of its own, as Blink makes a new window for a tab's first
  // navigation, whose about:blank has an opaque origin.
  url_ = GURL(response.url);
  // The Session's base::TimeTicks, which the Page's clock shares.
  document_ = &runner_->CommitDocument(
      url_, base::TimeTicks() + base::Microseconds(response.navigation_start),
      response.connection);
  // What DocumentLoader::CommitNavigation() takes from the navigation's
  // timings and response, for the window's Performance Timeline and
  // window.chrome. A restart for Critical-CH is a redirect to where the
  // navigation started, after which the fetch starts again.
  timing_ = &runner_->document_load_timing();
  DocumentLoadTiming& timing = *timing_;
  timing.has_response = true;
  timing.redirect_count = static_cast<uint16_t>(
      response.redirect_count + (response.restarted_for_critical_ch ? 1 : 0));
  timing.has_cross_origin_redirect = response.cross_origin_redirect;
  timing.fetch_start = TicksOf(response.timing.request_start);
  if (timing.redirect_count > 0) {
    timing.redirect_start = TicksOf(response.first_request_start);
    timing.redirect_end = timing.fetch_start;
  }
  timing.critical_ch_restart = TicksOf(response.critical_ch_restart);
  // CreateNavigationTimingInstance(): no start time, the document's URL, and
  // a response that always passes the TAO check, whose cache state the bytes
  // that had come with its headers decide.
  PageResponseStarted navigation_response = response;
  navigation_response.timing_allow_passed = true;
  timing.navigation = CreateResourceTimingInfo(
      base::TimeTicks(), base::UTF8ToUTF16(response.url), &navigation_response,
      base::TimeTicks::Now());
  timing.navigation.cache_state = response.validated ? CacheState::kValidated
                                  : response.received_bytes == 0
                                      ? CacheState::kLocal
                                      : CacheState::kNone;
  timing.connection_info = response.connection_info;
  timing.alpn_negotiated_protocol = response.alpn_negotiated_protocol;
  timing.was_fetched_via_spdy = response.was_fetched_via_spdy;
  timing.was_alpn_negotiated = response.was_alpn_negotiated;
  timing.dom_loading = base::TimeTicks::Now();
  document_->SetReadiness(Document::Readiness::kLoading);
  // Blink's Document::lastModifiedTime(): the response's Last-Modified, where
  // it parses.
  const std::string last_modified =
      HeaderField(response.headers, "last-modified");
  base::Time last_modified_time;
  if (!last_modified.empty() &&
      base::Time::FromString(last_modified.c_str(), &last_modified_time)) {
    document_->SetLastModified(last_modified_time);
  }
  // The decoder that DocumentLoader makes at commit, for a top-level
  // document of a browser in the Persona's locale. A response with no MIME
  // type is HTML here, as the parser reads it, where Chrome's network service
  // sniffs one.
  const MimeTypeAndCharset content_type =
      GetMimeTypeAndCharset(HeaderField(response.headers, "content-type"));
  decoder_ = BuildTextResourceDecoder(
      url_,
      content_type.mime_type.empty() ? "text/html" : content_type.mime_type,
      content_type.charset, document_->DefaultView()->Start().locale,
      /*parent=*/nullptr);
  parser_ = std::make_unique<HtmlDocumentParser>(*document_);
  // The load event waits for what the document's scripts hold back.
  document_->SetCheckCompleted(base::BindRepeating(
      &DocumentLoader::CheckCompleted, weak_factory_.GetWeakPtr()));
}

void DocumentLoader::AppendDecoded(std::u16string text) {
  // The document's encoding is the decoder's after each piece, as
  // DecodedDataDocumentParser::UpdateDocument() sets it.
  document_->SetEncoding(decoder_->encoding(),
                         decoder_->EncodingWasDetectedHeuristically());
  parser_->Append(text);
}

void DocumentLoader::OnResponseBody(std::string bytes) {
  body_bytes_ += bytes.size();
  waiting_body_.push_back(std::move(bytes));
  // HTMLDocumentParser::Append(): the parser takes it from a task of its own.
  SchedulePumpTokenizer();
}

void DocumentLoader::OnResponseTotals(const PageResponseEnded& ended) {
  // WindowPerformance::OnBodyLoadFinished() and FinishedLoading(): the body's
  // sizes, which the navigation's entry shows, and when it ended.
  if (!timing_) {
    return;
  }
  timing_->navigation.encoded_body_size =
      static_cast<uint64_t>(ended.encoded_body_size);
  timing_->navigation.decoded_body_size = body_bytes_;
  timing_->response_end = ended.completion_time != 0
                              ? TicksOf(ended.completion_time)
                              : base::TimeTicks::Now();
}

void DocumentLoader::OnResponseEnded(int net_error) {
  // After a response, a body that stops short still makes a document of what
  // came, as in Chrome. HTMLDocumentParser::Finish(): a parser that is to go
  // on from a task ends then.
  body_ended_ = true;
  if (!pump_scheduled_) {
    PumpTokenizerIfPossible();
  }
}

void DocumentLoader::SchedulePumpTokenizer() {
  if (!parser_ || pump_scheduled_ || stopping_) {
    return;
  }
  pump_scheduled_ = true;
  runner_->PostTask(TaskType::kNetworking,
                    base::BindOnce(
                        [](const base::WeakPtr<DocumentLoader>& loader) {
                          if (!loader) {
                            return;
                          }
                          loader->pump_scheduled_ = false;
                          loader->PumpTokenizerIfPossible();
                        },
                        weak_factory_.GetWeakPtr()));
}

void DocumentLoader::PumpTokenizerIfPossible() {
  if (!parser_ || IsPaused() || stopping_) {
    return;
  }
  if (PumpTokenizer()) {
    SchedulePumpTokenizer();
    return;
  }
  if (!IsPaused() && parser_finished_) {
    PrepareToStopParsing();
  }
}

bool DocumentLoader::PumpTokenizer() {
  // The parser yields once the document has a body and the budget has run
  // out, which it checks after each script and each piece of the body.
  const base::ElapsedTimer timer;
  const base::TimeDelta budget = times_yielded_ <= kNumYieldsWithDefaultBudget
                                     ? kDefaultParserBudget
                                     : kLongParserBudget;
  const auto should_yield = [this, &timer, budget] {
    if (!document_->Body() || timer.Elapsed() < budget) {
      return false;
    }
    ++times_yielded_;
    return true;
  };
  while (true) {
    while (HTMLScriptElement* const script = parser_->Pump()) {
      ProcessScriptElement(*script);
      if (IsPaused()) {
        return false;
      }
      if (should_yield()) {
        return true;
      }
    }
    if (!waiting_body_.empty()) {
      const std::string bytes = std::move(waiting_body_.front());
      waiting_body_.pop_front();
      AppendDecoded(decoder_->Decode(base::as_byte_span(bytes)));
      requests_->TookBody(kNavigationRequest, bytes.size());
      if (should_yield()) {
        return true;
      }
      continue;
    }
    if (body_ended_ && !parser_finished_) {
      AppendDecoded(decoder_->Flush());
      parser_->Finish();
      parser_finished_ = true;
      // The parser may still reach scripts before the end.
      continue;
    }
    return false;
  }
}

void DocumentLoader::ProcessScriptElement(HTMLScriptElement& element) {
  // The HTML Standard's script end tag: a microtask checkpoint unless a
  // script runs, then prepare the script element at a script nesting level
  // one deeper.
  if (script_nesting_level_ == 0) {
    PerformMicrotaskCheckpoint();
  }
  ++script_nesting_level_;
  PendingScript* const pending_script = element.Loader().PrepareScript(
      script_nesting_level_ == 1
          ? ScriptLoader::ParserBlockingInlineOption::kAllow
          : ScriptLoader::ParserBlockingInlineOption::kDeny);
  --script_nesting_level_;
  if (pending_script) {
    switch (pending_script->scheduling_type()) {
      case ScriptSchedulingType::kDefer:
        scripts_after_parsing_.emplace_back(pending_script);
        break;
      case ScriptSchedulingType::kParserBlocking:
      case ScriptSchedulingType::kParserBlockingInline:
        CHECK(!parser_blocking_script_);
        parser_blocking_script_ = pending_script;
        if (!pending_script->IsReady()) {
          pending_script->WatchForLoad(
              base::BindOnce(&DocumentLoader::PendingScriptFinished,
                             weak_factory_.GetWeakPtr()));
        }
        break;
      default:
        NOTREACHED();
    }
  }
  if (parser_blocking_script_ && script_nesting_level_ == 0) {
    ExecuteParsingBlockingScripts();
  }
}

void DocumentLoader::ExecuteParsingBlockingScripts() {
  while (parser_blocking_script_ && parser_blocking_script_->IsReady()) {
    PendingScript* const pending_script = parser_blocking_script_.Get();
    pending_script->StopWatchingForLoad();
    if (script_nesting_level_ == 0) {
      PerformMicrotaskCheckpoint();
    }
    parser_blocking_script_ = nullptr;
    ++script_nesting_level_;
    pending_script->ExecuteScriptBlock();
    --script_nesting_level_;
  }
}

void DocumentLoader::PendingScriptFinished() {
  runner_->PostTask(TaskType::kInternalContinueScriptLoading,
                    base::BindOnce(&DocumentLoader::NotifyScriptLoaded,
                                   weak_factory_.GetWeakPtr()));
}

void DocumentLoader::NotifyScriptLoaded() {
  if (stopping_) {
    AttemptToRunDeferredScriptsAndEnd();
    return;
  }
  ExecuteParsingBlockingScripts();
  // ResumeParsingAfterPause(): the parser goes on from a later task.
  if (!IsPaused()) {
    SchedulePumpTokenizer();
  }
}

void DocumentLoader::PrepareToStopParsing() {
  stopping_ = true;
  timing_->dom_interactive = base::TimeTicks::Now();
  document_->SetReadiness(Document::Readiness::kInteractive);
  // Document::SetReadyState() tells Chrome's browser process, which answers
  // with the navigation's confidence from a later task: high, as for any
  // navigation once the browser has started, with its noise.
  runner_->PostTask(
      TaskType::kInternalNavigationAssociated,
      base::BindOnce(
          [](const cppgc::Persistent<DocumentLoadTiming>& timing) {
            timing->high_confidence =
                PerformanceTimingConfidence::DrawHighConfidence();
          },
          timing_));
  AttemptToRunDeferredScriptsAndEnd();
}

void DocumentLoader::AttemptToRunDeferredScriptsAndEnd() {
  if (parsing_finished_ || !ExecuteScriptsWaitingForParsing()) {
    return;
  }
  FinishedParsing();
}

bool DocumentLoader::ExecuteScriptsWaitingForParsing() {
  // SeparateDeferModuleScriptTasks, which stable has on: one deferred script
  // in each task.
  if (scripts_after_parsing_.empty()) {
    return true;
  }
  PendingScript* const first = scripts_after_parsing_.front().Get();
  if (!first->IsReady()) {
    if (!first->IsWatchingForLoad()) {
      first->WatchForLoad(base::BindOnce(&DocumentLoader::PendingScriptFinished,
                                         weak_factory_.GetWeakPtr()));
    }
    return false;
  }
  const cppgc::Persistent<PendingScript> script = first;
  scripts_after_parsing_.pop_front();
  script->StopWatchingForLoad();
  if (script_nesting_level_ == 0) {
    PerformMicrotaskCheckpoint();
  }
  script->ExecuteScriptBlock();
  if (scripts_after_parsing_.empty()) {
    return true;
  }
  runner_->PostTask(
      TaskType::kInternalContinueScriptLoading,
      base::BindOnce(
          [](const base::WeakPtr<DocumentLoader>& loader) {
            if (loader && loader->ExecuteScriptsWaitingForParsing()) {
              loader->NotifyScriptLoaded();
            }
          },
          weak_factory_.GetWeakPtr()));
  return false;
}

void DocumentLoader::FinishedParsing() {
  parsing_finished_ = true;
  // DOMContentLoaded at the document, from the task that ends the parse, as
  // Blink dispatches it. It reaches the document's listeners: events that go
  // through the tree and on to the window are a Known Gap.
  bindings::EventInit init;
  init.bubbles = true;
  Event* const event = Event::Create(u"DOMContentLoaded", init);
  event->SetTrusted(true);
  timing_->dom_content_loaded_event_start = base::TimeTicks::Now();
  document_->DispatchEvent(event);
  timing_->dom_content_loaded_event_end = base::TimeTicks::Now();
  PerformMicrotaskCheckpoint();
  CheckCompleted();
}

void DocumentLoader::CheckCompleted() {
  // Document::ShouldComplete(), with what a Page's documents hold back: the
  // async and in-order scripts.
  if (!parsing_finished_ || load_fired_ || document_->IsDelayingLoadEvent()) {
    return;
  }
  load_fired_ = true;
  timing_->dom_complete = base::TimeTicks::Now();
  document_->SetReadiness(Document::Readiness::kComplete);
  bindings::EventInit init;
  Event* const event = Event::Create(u"load", init);
  event->SetTrusted(true);
  timing_->load_event_start = base::TimeTicks::Now();
  if (Window* const window = document_->DefaultView()) {
    window->DispatchEventWithTarget(event, document_.Get());
  }
  timing_->load_event_end = base::TimeTicks::Now();
  send_.Run(EncodeLoaded());
}

}  // namespace mitto
