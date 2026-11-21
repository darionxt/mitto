#include "mitto/dom/document_parser.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/elapsed_timer.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_kinds.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_preload_scanner.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/libxml_tree_builder.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/script_element_base.h"
#include "mitto/dom/script_loader.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/dom/xml_tree_builder.h"
#include "url/gurl.h"
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

bool IsFeatureEnabled(bindings::RuntimeFeature feature) {
  v8::Isolate* const isolate = v8::Isolate::TryGetCurrent();
  const bindings::RuntimeFeatureSet* const features =
      isolate ? bindings::PerIsolateData::FeaturesOf(isolate) : nullptr;
  return features && features->IsEnabled(feature);
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

DocumentParser::DocumentParser(Document& document, Client* client, Kind kind)
    : document_(&document),
      kind_(kind),
      client_(client),
      created_by_script_(!client),
      references_(
          bindings::MakeGarbageCollected<HtmlTreeBuilderReferences>(document)),
      tree_(std::make_unique<HtmlDocumentParser>(document, references_.Get())) {
  // XMLDocumentParserRs's script runner exists only for a document of a
  // frame, and a document of no window marks its scripts already started.
  if (kind_ == Kind::kXml) {
    // Document::CreateParser(): the Rust xml crate's reader with
    // XMLParsingRust, or with XMLRustForNonXslt for a document of no frame or
    // an SVG document, and libxml2 for any other.
    const bool no_window = !document.DefaultView();
    if (IsFeatureEnabled(bindings::RuntimeFeature::kXMLParsingRust) ||
        (IsFeatureEnabled(bindings::RuntimeFeature::kXMLRustForNonXslt) &&
         (no_window || document.ContentType() == u"image/svg+xml"))) {
      xml_ = std::make_unique<XmlTreeBuilder>(
          *references_, /*scripts_already_started=*/no_window);
    } else {
      xml_ = std::make_unique<LibxmlTreeBuilder>(
          *references_, /*scripts_already_started=*/no_window);
    }
  }
  // The tree stops after the token that added a style sheet that blocks the
  // parser, which owns it.
  tree_->SetShouldPause(base::BindRepeating(
      [](const DocumentParser* parser) {
        return parser->added_pending_parser_blocking_stylesheet_;
      },
      base::Unretained(this)));
}

DocumentParser::~DocumentParser() = default;

void DocumentParser::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  visitor->Trace(json_pre_);
  visitor->Trace(image_resource_);
  visitor->Trace(references_);
  visitor->Trace(parser_blocking_script_);
  scripts_after_parsing_.Trace(visitor);
}

MarkupPosition DocumentParser::TextPosition() const {
  return xml_ ? xml_->TextPosition() : tree_->TextPosition();
}

void DocumentParser::DidReceiveData() {
  // HTMLDocumentParser::Append() and FinishAppend(): the parser takes what
  // came from a task, as Chrome's ProcessHtmlDataImmediately is off. While it
  // waits for a script or a style sheet, what came goes into its input now,
  // where the preload scanner reads it.
  if (IsStopped()) {
    return;
  }
  if (IsPaused() && client_ && !InPumpSession()) {
    while (!IsStopped() && IsPaused() && client_->FeedParser()) {
    }
  }
  SchedulePumpTokenizer();
}

void DocumentParser::AppendBytes(const std::string& bytes) {
  if (IsDetached()) {
    return;
  }
  if (kind_ == Kind::kText && !started_) {
    started_ = true;
    tree_->StartTextDocument();
    return;
  }
  if (kind_ != Kind::kImage) {
    return;
  }
  if (!image_resource_) {
    started_ = true;
    image_resource_ = bindings::MakeGarbageCollected<Resource>(
        ResourceType::kImage, GURL(document_->UrlForBinding()),
        FetchMode::kNoCors, FetchCredentialsMode::kOmit);
    BuildImageDocument(*document_, *image_resource_);
    if (IsStopped()) {
      return;
    }
    // The response's type and URL; an image takes no notice of its status.
    FetchResponseHead head;
    head.url = base::UTF16ToUTF8(document_->UrlForBinding());
    image_resource_->ResponseReceived(
        std::move(head), base::UTF16ToUTF8(document_->ContentType()));
  }
  // Nothing more goes to an image that has failed to decode.
  if (image_resource_->GetStatus() != ResourceStatus::kDecodeError) {
    image_resource_->AppendData(bytes);
  }
}

void DocumentParser::AppendData(std::u16string_view text) {
  if (IsDetached()) {
    return;
  }
  switch (kind_) {
    case Kind::kHtml:
      // HTMLDocumentParser::Append(): the preload scanner takes each piece
      // from the first on, and scans the first, and any while the parser
      // waits.
      if (!preload_scanner_ && HasPreloader() && !IsSynchronous()) {
        preload_scanner_ = std::make_unique<HtmlPreloadScanner>(
            *document_, TokenPreloadScanner::ScannerType::kMainDocument);
      }
      if (preload_scanner_) {
        preload_scanner_->AppendToEnd(text);
        if (IsPaused() || !seen_first_byte_) {
          ScanAndPreload(*preload_scanner_);
        }
      }
      tree_->Append(text);
      seen_first_byte_ = true;
      return;
    case Kind::kText:
      // TextDocumentParser scans nothing, as kDisallowPrefetching has it.
      tree_->Append(text);
      return;
    case Kind::kXml:
      // DecodedDataDocumentParser gives the parser no empty piece.
      if (!text.empty()) {
        xml_->Append(text);
      }
      return;
    case Kind::kJson:
      // Each piece that decodes to text is a text node of its own, as
      // insertAdjacentText() adds it.
      if (!text.empty()) {
        if (!started_) {
          started_ = true;
          json_pre_ = &BuildJsonDocument(*document_);
        }
        json_pre_->ParserAppendChild(
            Text::Create(*document_, std::u16string(text)));
      }
      return;
    case Kind::kImage:
    case Kind::kMedia:
      return;
  }
}

bool DocumentParser::HasPreloader() const {
  return document_ && document_->DefaultView() &&
         document_->DefaultView()->HasContext();
}

void DocumentParser::ScanAndPreload(HtmlPreloadScanner& scanner) {
  HtmlPreloadScanner::Result result =
      scanner.Scan(document_->ValidBaseElementUrl());
  seen_csp_meta_tags_ += result.csp_meta_tag_count;
  for (PreloadRequest& request : result.requests) {
    queued_preloads_.push_back(std::move(request));
  }
  // MaybeFetchQueuedPreloads(): AllowPreloadingWithCSPMetaTag is off, so the
  // queue waits for good once a CSP meta element has been seen.
  if (queued_preloads_.empty() || seen_csp_meta_tags_) {
    return;
  }
  std::vector<PreloadRequest> preloads = std::move(queued_preloads_);
  queued_preloads_.clear();
  const cppgc::Persistent<Document> document = document_.Get();
  for (const PreloadRequest& preload : preloads) {
    preload.Start(*document);
  }
}

void DocumentParser::FinishData() {
  if (IsDetached()) {
    return;
  }
  // A media document builds its tree at the end, and an image or JSON
  // document that has started its tree takes no end of input, which would
  // make the HTML parser's: an empty JSON body gives what HTML's empty input
  // gives, and an empty image a document with no children, as in Chrome.
  if (kind_ == Kind::kMedia) {
    BuildMediaDocument(*document_);
    return;
  }
  if (kind_ == Kind::kImage) {
    if (image_resource_) {
      // ImageDocumentParser::Finish(): all of the body is the image's.
      const cppgc::Persistent<Resource> content = image_resource_.Get();
      content->Finish(0);
      if (!IsDetached()) {
        ImageDocumentLoaded(*document_, *content);
      }
    }
    return;
  }
  if (kind_ == Kind::kJson && started_) {
    return;
  }
  if (xml_) {
    xml_->Finish();
    return;
  }
  tree_->Finish();
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
  if (created_by_script_ && xml_ && !xml_->HasSeenEndOfFile()) {
    xml_->Finish();
  } else if (created_by_script_ && !xml_ && !tree_->HasSeenEndOfFile()) {
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
  if (xml_) {
    return created_by_script_ && !xml_->HasSeenEndOfFile();
  }
  return tree_->HasInsertionPoint() ||
         (created_by_script_ && !tree_->HasSeenEndOfFile());
}

void DocumentParser::Insert(std::u16string_view source) {
  // An XML document's content starts its parser even when empty, as
  // XMLDocumentParser::DoWrite() makes its context then.
  if (IsStopped() || (source.empty() && !xml_)) {
    return;
  }
  const HeldParser protect(this);
  if (xml_) {
    // What DOMParser and setting a document's content give an XML document.
    xml_->Append(source);
  } else {
    tree_->Insert(source);
  }
  // Everything that can be parsed now is, without a yield.
  const ScopedCounter should_complete(should_complete_);
  const ScopedCounter end_if_delayed_forbidden(end_if_delayed_forbidden_);
  PumpTokenizerIfPossible();
  // What was written, scanned with a scanner of its own while the parser
  // waits, as the main scanner cannot take an insertion.
  if (IsPaused() && HasPreloader()) {
    if (!insertion_scanner_) {
      insertion_scanner_ = std::make_unique<HtmlPreloadScanner>(
          *document_, TokenPreloadScanner::ScannerType::kInsertion);
    }
    insertion_scanner_->AppendToEnd(source);
    ScanAndPreload(*insertion_scanner_);
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
  preload_scanner_.reset();
  insertion_scanner_.reset();
  queued_preloads_.clear();
  document_ = nullptr;
  // The tokenizer and the tree builder go now, unless a pump or a script that
  // is still on the stack uses them, in which case they go with the parser.
  if (!InPumpSession() && !IsExecutingScript() && !tree_->HasInsertionPoint()) {
    tree_.reset();
    xml_.reset();
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
  // Blink's XML parser takes all that it has.
  const bool should_run_until_completion =
      ShouldComplete() || IsSynchronous() || InNestedPumpSession() || xml_;
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
    Element* const script = xml_ ? xml_->Pump() : tree_->Pump();
    // Blink's XML parser does not wait for style sheets.
    if (!xml_) {
      CheckIfBlockingStylesheetAdded();
    }
    if (IsPaused() && !script) {
      break;
    }
    if (script) {
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
  // A parser that waits for a script or a style sheet has its preload
  // scanner read ahead, from where the parser stopped if it had none.
  if (!IsStopped() && IsPaused() && HasPreloader()) {
    if (!preload_scanner_) {
      preload_scanner_ = std::make_unique<HtmlPreloadScanner>(
          *document_, TokenPreloadScanner::ScannerType::kMainDocument);
      preload_scanner_->AppendToEnd(tree_->UnreadInput());
    }
    ScanAndPreload(*preload_scanner_);
  }
  if (should_yield) {
    ++times_yielded_;
  }
  return should_yield;
}

void DocumentParser::CheckIfBlockingStylesheetAdded() {
  if (added_pending_parser_blocking_stylesheet_) {
    added_pending_parser_blocking_stylesheet_ = false;
    waiting_for_stylesheets_ = true;
  }
}

void DocumentParser::ExecuteScriptsWaitingForResources() {
  if (IsStopped()) {
    return;
  }
  waiting_for_stylesheets_ = false;
  if (IsStopping()) {
    AttemptToRunDeferredScriptsAndEnd();
    return;
  }
  // HTMLParserScriptRunner::ExecuteScriptsWaitingForResources().
  if (parser_blocking_script_ && !IsExecutingScript()) {
    ExecuteParsingBlockingScripts();
  }
  if (!IsPaused()) {
    ResumeParsingAfterPause();
  }
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
  // XMLDocumentParserRs::EndInternal(): the text that waits, or the block of
  // the errors, before the document is interactive.
  if (xml_) {
    xml_->End();
  }
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

void DocumentParser::ProcessScriptElement(Element& element) {
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

void DocumentParser::ProcessScriptElementInternal(Element& element) {
  // The HTML Standard's script end tag: a microtask checkpoint unless a
  // script runs, then, with the insertion point just after the end tag,
  // prepare the script element at a script nesting level one deeper, which
  // may run a script that writes into the parser.
  if (!IsExecutingScript()) {
    PerformMicrotaskCheckpoint();
  }
  const InsertionPointRecord insertion_point(*tree_);
  const ScopedCounter nesting_level(script_nesting_level_);
  PendingScript* const pending_script =
      ToScriptElement(element)->Loader().PrepareScript(
          script_nesting_level_ == 1
              ? ScriptLoader::ParserBlockingInlineOption::kAllow
              : ScriptLoader::ParserBlockingInlineOption::kDeny);
  if (!pending_script) {
    return;
  }
  // XMLParserScriptRunner::ProcessScriptElement(): the XML parser takes no
  // deferred script, so it drops a module script that waits for the end of
  // parsing, which never runs, as in Chrome.
  if (xml_ && pending_script->IsModule()) {
    if (Window* const window = document_->DefaultView()) {
      window->AddConsoleMessage(ConsoleLevel::kError,
                                "Module scripts in XML documents are currently "
                                "not supported. See crbug.com/717643");
    }
    return;
  }
  switch (pending_script->scheduling_type()) {
    case ScriptSchedulingType::kDefer:
      // The XML parser's script runner takes a deferred script as one that
      // blocks the parser.
      if (!xml_) {
        scripts_after_parsing_.push_back(pending_script);
        break;
      }
      [[fallthrough]];
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
