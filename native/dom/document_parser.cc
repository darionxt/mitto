#include "mitto/dom/document_parser.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/notreached.h"
#include "base/time/time.h"
#include "base/timer/elapsed_timer.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_write_intervention.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"
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

using HeldParser = cppgc::Persistent<DocumentParser>;
using WeakParser = cppgc::WeakPersistent<DocumentParser>;

// HeapDeque::TakeFirst().
PendingScript* TakeFirst(HeapVector<PendingScript>& scripts) {
  PendingScript* const first = scripts[0];
  bool taken = false;
  scripts.EraseIf(
      [&taken](PendingScript*) { return !std::exchange(taken, true); });
  return first;
}

}  // namespace

// A counter that is one higher while the scope lasts, as Blink's
// ShouldCompleteScope, EndIfDelayedForbiddenScope and NestingLevelIncrementer
// keep theirs.
class DocumentParser::ScopedCounter {
 public:
  explicit ScopedCounter(int& counter) : counter_(counter) { ++*counter_; }
  ScopedCounter(const ScopedCounter&) = delete;
  ScopedCounter& operator=(const ScopedCounter&) = delete;
  ~ScopedCounter() { --*counter_; }

 private:
  const raw_ref<int> counter_;
};

// Blink's InsertionPointRecord: an insertion point just before the next input
// character while the scope lasts, and the one before after it.
class DocumentParser::InsertionPointRecord {
 public:
  explicit InsertionPointRecord(HtmlDocumentParser& tree) : tree_(tree) {
    tree_->PushInsertionPoint();
  }
  InsertionPointRecord(const InsertionPointRecord&) = delete;
  InsertionPointRecord& operator=(const InsertionPointRecord&) = delete;
  ~InsertionPointRecord() { tree_->PopInsertionPoint(); }

 private:
  const raw_ref<HtmlDocumentParser> tree_;
};

DocumentParser::DocumentParser(Document& document, Client* client)
    : document_(&document),
      client_(client),
      created_by_script_(!client),
      references_(
          bindings::MakeGarbageCollected<HtmlTreeBuilderReferences>(document)),
      tree_(std::make_unique<HtmlDocumentParser>(document, references_.Get())) {
}

DocumentParser::~DocumentParser() = default;

void DocumentParser::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  visitor->Trace(references_);
  visitor->Trace(parser_blocking_script_);
  scripts_after_parsing_.Trace(visitor);
}

void DocumentParser::DidReceiveData() {
  // HTMLDocumentParser::Append() and FinishAppend(): the parser takes what
  // came from a task, as Chrome's ProcessHtmlDataImmediately is off.
  if (IsStopped()) {
    return;
  }
  SchedulePumpTokenizer();
}

void DocumentParser::AppendData(std::u16string_view text) {
  if (!IsDetached()) {
    tree_->Append(text);
  }
}

void DocumentParser::FinishData() {
  if (!IsDetached()) {
    tree_->Finish();
  }
}

void DocumentParser::Finish() {
  const HeldParser protect(this);
  const ScopedCounter should_complete(should_complete_);
  const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
  if (IsDetached()) {
    return;
  }
  // The explicit end of the input of a script-created parser. The client of
  // a response's parser ends its input once it has given all of the body.
  if (created_by_script_ && !tree_->HasSeenEndOfFile()) {
    tree_->Finish();
  }
  should_attempt_to_end_on_eof_ = true;
  if (IsScheduled()) {
    return;
  }
  AttemptToEnd();
}

bool DocumentParser::HasInsertionPoint() const {
  if (!tree_) {
    return false;
  }
  // Blink models the explicit end of the input of a script-created parser as
  // it models the network's end, which the FIXME there says may differ from
  // the standard.
  return tree_->HasInsertionPoint() ||
         (created_by_script_ && !tree_->HasSeenEndOfFile());
}

void DocumentParser::Insert(std::u16string_view source) {
  if (IsStopped() || source.empty()) {
    return;
  }
  const HeldParser protect(this);
  tree_->Insert(source);
  // Everything that can be parsed now is, without a yield.
  const ScopedCounter should_complete(should_complete_);
  const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
  PumpTokenizerIfPossible();
  // What was written, scanned for the scripts that Chrome's preload scanner
  // would fetch while the parser waits.
  if (IsPaused() && document_ && document_->DefaultView()) {
    if (!insertion_scanner_) {
      insertion_scanner_ = std::make_unique<DocWrittenScriptScanner>();
    }
    insertion_scanner_->Scan(source, *document_);
  }
  EndIfDelayed();
}

void DocumentParser::Detach() {
  if (IsDetached()) {
    return;
  }
  const HeldParser protect(this);
  scheduled_ = Scheduled::kNotScheduled;
  state_ = State::kDetached;
  // HTMLParserScriptRunner::Detach(): the scripts that it holds go.
  if (PendingScript* const script = parser_blocking_script_.Release()) {
    script->Dispose();
  }
  for (PendingScript* const script : scripts_after_parsing_.ToVector()) {
    script->Dispose();
  }
  scripts_after_parsing_.clear();
  document_ = nullptr;
  // The tokenizer and the tree builder go now, unless a pump or a script that
  // is still on the stack uses them, in which case they go with the parser.
  if (!InPumpSession() && !IsExecutingScript() && !tree_->HasInsertionPoint()) {
    tree_.reset();
  }
  if (Client* const client = std::exchange(client_, nullptr)) {
    client->ParserDetached();
  }
}

void DocumentParser::PostTask(TaskType type, void (DocumentParser::*method)()) {
  Window* const window = document_ ? document_->DefaultView() : nullptr;
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(
      type, base::BindOnce(
                [](const HeldParser& parser, void (DocumentParser::*method)()) {
                  (parser.Get()->*method)();
                },
                HeldParser(this), method));
}

void DocumentParser::SchedulePumpTokenizer() {
  DCHECK(!IsStopped());
  DCHECK(!InPumpSession());
  DCHECK(!ShouldComplete());
  if (IsScheduled()) {
    return;
  }
  PostTask(TaskType::kNetworking,
           &DocumentParser::DeferredPumpTokenizerIfPossible);
  scheduled_ = Scheduled::kScheduled;
}

void DocumentParser::ScheduleEndIfDelayed() {
  DCHECK(!IsStopped());
  DCHECK(!InPumpSession());
  DCHECK(!ShouldComplete());
  if (!IsScheduled()) {
    PostTask(TaskType::kNetworking,
             &DocumentParser::DeferredPumpTokenizerIfPossible);
  }
  // A pump that is already scheduled ends the parse if it was delayed too.
  scheduled_ = Scheduled::kScheduledWithEndIfDelayed;
}

void DocumentParser::DeferredPumpTokenizerIfPossible() {
  if (!IsScheduled()) {
    return;
  }
  const bool end_if_delayed =
      scheduled_ == Scheduled::kScheduledWithEndIfDelayed;
  scheduled_ = Scheduled::kNotScheduled;
  if (!end_if_delayed) {
    PumpTokenizerIfPossible();
    return;
  }
  const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
  PumpTokenizerIfPossible();
  EndIfDelayed();
}

void DocumentParser::PumpTokenizerIfPossible() {
  bool yielded = false;
  if (!IsStopped() && (!IsPaused() || ShouldEndIfDelayed())) {
    yielded = PumpTokenizer();
  }
  if (yielded) {
    SchedulePumpTokenizer();
  } else if (should_attempt_to_end_on_eof_) {
    // Finish() was called while the parser had input to go: it ends once it
    // has taken all of it.
    AttemptToEnd();
  } else if (ShouldEndIfDelayed()) {
    if (ShouldComplete() || IsStopped() || IsStopping()) {
      EndIfDelayed();
    } else {
      ScheduleEndIfDelayed();
    }
  }
}

bool DocumentParser::PumpTokenizer() {
  DCHECK(!IsStopped());
  const ScopedCounter pump_session(pump_session_nesting_level_);
  // Within a write, in a nested pump and in a script-created parser, it takes
  // all that it can.
  const bool should_run_until_completion =
      ShouldComplete() || IsSynchronous() || InNestedPumpSession();
  // Once the document has a body, the parser yields when its budget has run
  // out, which it checks after each script and each piece of the body.
  const base::ElapsedTimer timer;
  base::TimeDelta budget = times_yielded_ <= kNumYieldsWithDefaultBudget
                               ? kDefaultParserBudget
                               : kLongParserBudget;
  bool should_yield = false;
  const auto check_yield = [&] {
    if (should_run_until_completion || IsPaused() || !document_->Body()) {
      return false;
    }
    should_yield = timer.Elapsed() >= budget;
    return should_yield;
  };
  while (!IsStopped() && !IsPaused()) {
    if (HTMLScriptElement* const script = tree_->Pump()) {
      ProcessScriptElement(*script);
      if (IsStopped() || IsPaused()) {
        break;
      }
      // After a script in the body, the parser yields within the default
      // budget at the latest, even in its longer budget.
      if (document_->Body()) {
        budget = std::min(budget, timer.Elapsed() + kDefaultParserBudget);
      }
      if (check_yield()) {
        break;
      }
      continue;
    }
    // The tokenizer has given what it has. A response's parser takes the
    // next piece of the body, but not within a write, which reads only what
    // the scripts wrote.
    if (!client_ || tree_->HasInsertionPoint() || InNestedPumpSession() ||
        !client_->FeedParser()) {
      break;
    }
    if (check_yield()) {
      break;
    }
  }
  if (should_yield) {
    ++times_yielded_;
  }
  return should_yield;
}

void DocumentParser::ResumeParsingAfterPause() {
  DCHECK(!IsExecutingScript());
  if (IsStopped() || IsPaused()) {
    return;
  }
  insertion_scanner_.reset();
  if (!IsSynchronous() && !ShouldComplete() && !InPumpSession()) {
    SchedulePumpTokenizer();
    return;
  }
  const ScopedCounter should_complete(should_complete_);
  PumpTokenizerIfPossible();
}

bool DocumentParser::ShouldDelayEnd() const {
  return InPumpSession() || IsPaused() || IsExecutingScript() || IsScheduled();
}

void DocumentParser::AttemptToEnd() {
  DCHECK(should_attempt_to_end_on_eof_);
  // Blink's AttemptToEndForbiddenScope: from now on, EndIfDelayed() ends it.
  should_attempt_to_end_on_eof_ = false;
  if (ShouldDelayEnd()) {
    end_was_delayed_ = true;
    return;
  }
  PrepareToStopParsing();
}

void DocumentParser::EndIfDelayed() {
  const ScopedCounter should_complete(should_complete_);
  const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
  if (IsDetached() || !end_was_delayed_ || ShouldDelayEnd()) {
    return;
  }
  end_was_delayed_ = false;
  PrepareToStopParsing();
}

void DocumentParser::PrepareToStopParsing() {
  if (IsDetached()) {
    return;
  }
  DCHECK(!tree_->HasInsertionPoint());
  const HeldParser protect(this);
  {
    // What is left is the characters that the tokenizer holds, and the end of
    // a response's body that the client has not given yet.
    const ScopedCounter should_complete(should_complete_);
    const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
    PumpTokenizerIfPossible();
  }
  if (IsStopped()) {
    return;
  }
  if (IsPaused()) {
    // Blink cannot get here, as the end waits for a pump that is scheduled.
    end_was_delayed_ = true;
    return;
  }
  state_ = State::kStopping;
  const cppgc::Persistent<Document> document = document_.Get();
  document->SetReadiness(Document::Readiness::kInteractive);
  // Its readystatechange may have detached the parser.
  if (IsDetached()) {
    return;
  }
  AttemptToRunDeferredScriptsAndEnd();
}

void DocumentParser::AttemptToRunDeferredScriptsAndEnd() {
  DCHECK(IsStopping());
  DCHECK(!tree_->HasInsertionPoint());
  if (!ExecuteScriptsWaitingForParsing()) {
    return;
  }
  End();
}

void DocumentParser::End() {
  DCHECK(!IsDetached());
  // HTMLTreeBuilder::Finished(), then DocumentParser::StopParsing().
  const HeldParser protect(this);
  const cppgc::Persistent<Document> document = document_.Get();
  document->FinishedParsing();
  StopParsing();
}

void DocumentParser::StopParsing() {
  if (!IsDetached()) {
    state_ = State::kStopped;
  }
  scheduled_ = Scheduled::kNotScheduled;
}

void DocumentParser::ProcessScriptElement(HTMLScriptElement& element) {
  ProcessScriptElementInternal(element);
  // A microtask of the checkpoint may have detached the parser, as
  // document.open() does.
  if (!document_) {
    return;
  }
  if (!parser_blocking_script_) {
    return;
  }
  // Within a script that runs, the parser stops here and goes on once the
  // outermost script is done, as the HTML Standard's nested tokenizer does.
  if (IsExecutingScript()) {
    return;
  }
  ExecuteParsingBlockingScripts();
}

void DocumentParser::ProcessScriptElementInternal(HTMLScriptElement& element) {
  // The HTML Standard's script end tag: a microtask checkpoint unless a
  // script runs, then, with the insertion point just after the end tag,
  // prepare the script element at a script nesting level one deeper, which
  // may run a script that writes into the parser.
  if (!IsExecutingScript()) {
    PerformMicrotaskCheckpoint();
  }
  const InsertionPointRecord insertion_point(*tree_);
  const ScopedCounter nesting_level(script_nesting_level_);
  PendingScript* const pending_script = element.Loader().PrepareScript(
      script_nesting_level_ == 1
          ? ScriptLoader::ParserBlockingInlineOption::kAllow
          : ScriptLoader::ParserBlockingInlineOption::kDeny);
  if (!pending_script) {
    return;
  }
  switch (pending_script->scheduling_type()) {
    case ScriptSchedulingType::kDefer:
      scripts_after_parsing_.push_back(pending_script);
      break;
    case ScriptSchedulingType::kParserBlocking:
    case ScriptSchedulingType::kParserBlockingInline:
      CHECK(!parser_blocking_script_);
      parser_blocking_script_ = pending_script;
      // The caller runs it at once if it is ready.
      if (!pending_script->IsReady()) {
        pending_script->WatchForLoad(base::BindOnce(
            [](const WeakParser& parser) {
              if (parser) {
                parser->PendingScriptFinished();
              }
            },
            WeakParser(this)));
      }
      break;
    default:
      NOTREACHED();
  }
}

void DocumentParser::ExecuteParsingBlockingScripts() {
  // The HTML Standard's script end tag, "otherwise": while the pending
  // parsing-blocking script is ready, it runs with the insertion point just
  // before the next input character.
  while (parser_blocking_script_ && parser_blocking_script_->IsReady() &&
         document_ && document_->IsScriptExecutionReady()) {
    DCHECK(!IsExecutingScript());
    const InsertionPointRecord insertion_point(*tree_);
    ExecutePendingParserBlockingScriptAndDispatchEvent();
  }
}

void DocumentParser::ExecutePendingParserBlockingScriptAndDispatchEvent() {
  const cppgc::Persistent<PendingScript> pending_script =
      parser_blocking_script_.Get();
  pending_script->StopWatchingForLoad();
  if (!IsExecutingScript()) {
    PerformMicrotaskCheckpoint();
    if (!document_ || !document_->IsScriptExecutionReady()) {
      return;
    }
  }
  parser_blocking_script_ = nullptr;
  const ScopedCounter nesting_level(script_nesting_level_);
  pending_script->ExecuteScriptBlock();
}

void DocumentParser::PendingScriptFinished() {
  if (IsDetached()) {
    return;
  }
  // The script that the parser waits for is ready: the parser goes on from a
  // task of a very high priority, as Blink posts NotifyScriptLoaded().
  PostTask(TaskType::kInternalContinueScriptLoading,
           &DocumentParser::NotifyScriptLoaded);
}

void DocumentParser::NotifyScriptLoaded() {
  DCHECK(!IsExecutingScript());
  if (IsStopped()) {
    return;
  }
  if (IsStopping()) {
    AttemptToRunDeferredScriptsAndEnd();
    return;
  }
  ExecuteParsingBlockingScripts();
  // The parser goes on from a later task.
  if (!IsPaused()) {
    ResumeParsingAfterPause();
  }
}

bool DocumentParser::ExecuteScriptsWaitingForParsing() {
  // SeparateDeferModuleScriptTasks, which stable has on: one deferred script
  // in each task.
  if (scripts_after_parsing_.empty()) {
    return true;
  }
  DCHECK(!IsExecutingScript());
  DCHECK(!parser_blocking_script_);
  if (!document_->IsScriptExecutionReady()) {
    return false;
  }
  PendingScript* const first = scripts_after_parsing_[0];
  if (!first->IsReady()) {
    if (!first->IsWatchingForLoad()) {
      first->WatchForLoad(base::BindOnce(
          [](const WeakParser& parser) {
            if (parser) {
              parser->PendingScriptFinished();
            }
          },
          WeakParser(this)));
    }
    return false;
  }
  const cppgc::Persistent<PendingScript> script =
      TakeFirst(scripts_after_parsing_);
  script->StopWatchingForLoad();
  if (!IsExecutingScript()) {
    PerformMicrotaskCheckpoint();
  }
  script->ExecuteScriptBlock();
  if (!document_) {
    return false;
  }
  if (scripts_after_parsing_.empty()) {
    return true;
  }
  PostTask(TaskType::kInternalContinueScriptLoading,
           &DocumentParser::ContinueScriptsWaitingForParsing);
  return false;
}

void DocumentParser::ContinueScriptsWaitingForParsing() {
  if (ExecuteScriptsWaitingForParsing()) {
    NotifyScriptLoaded();
  }
}

}  // namespace mitto
