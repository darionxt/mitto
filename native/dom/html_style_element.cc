#include "mitto/dom/html_style_element.h"

#include <string_view>

#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_style_element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

void FireSimpleEvent(EventTarget& target, const std::u16string& type) {
  bindings::EventInit init;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  target.DispatchEvent(event);
}

}  // namespace

// static
HTMLStyleElement* HTMLStyleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLStyleElement>(document);
}

HTMLStyleElement::HTMLStyleElement(Document& document)
    : HTMLElement(document, u"style") {}

HTMLStyleElement::~HTMLStyleElement() = default;

const bindings::WrapperTypeInfo* HTMLStyleElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLStyleElement::kWrapperTypeInfo;
}

void HTMLStyleElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(sheet_);
  HTMLElement::Trace(visitor);
}

void HTMLStyleElement::SetCreatedByParser() {
  created_by_parser_ = true;
  has_finished_parsing_children_ = false;
}

void HTMLStyleElement::FinishParsingChildren() {
  Process();
  has_finished_parsing_children_ = true;
}

Node::InsertionNotificationRequest HTMLStyleElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  // StyleElement::ProcessStyleSheet(): one the parser makes waits for the
  // end of its text.
  if (IsConnected() && has_finished_parsing_children_) {
    Process();
  }
  return InsertionNotificationRequest::kInsertionDone;
}

void HTMLStyleElement::RemovedFrom(Node& insertion_point) {
  HTMLElement::RemovedFrom(insertion_point);
  if (insertion_point.IsConnected() && sheet_) {
    ClearSheet();
  }
}

void HTMLStyleElement::ChildrenChanged(ChildrenChangeType type,
                                       Node& child,
                                       bool by_parser) {
  HTMLElement::ChildrenChanged(type, child, by_parser);
  if (has_finished_parsing_children_) {
    Process();
  }
}

void HTMLStyleElement::AttributeChanged(const QualifiedName& name,
                                        const std::u16string* old_value,
                                        const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"type") {
    // StyleElement::ChildrenChanged().
    if (has_finished_parsing_children_) {
      Process();
    }
  } else if (name.local_name == u"blocking") {
    // BlockingAttributeChanged(): a sheet that a script inserted, and that
    // no longer asks to block rendering, blocks nothing more.
    if (pending_sheet_type_ ==
            Document::PendingSheetType::kDynamicRenderBlocking &&
        !IsPotentiallyRenderBlocking()) {
      GetDocument().RemovePendingBlockingSheet(*this, pending_sheet_type_);
      pending_sheet_type_ = Document::PendingSheetType::kNonBlocking;
    }
  }
  // A change of media changes which rules apply, and fetches nothing.
}

void HTMLStyleElement::Process() {
  if (!IsConnected()) {
    return;
  }
  // StyleElement::CreateSheetOrModule(), for a sheet that no Content
  // Security Policy refuses, which a Page does not enforce for style.
  if (sheet_) {
    ClearSheet();
  }
  const std::u16string* type = FindAttribute(u"type");
  if (type && !type->empty() &&
      !base::EqualsCaseInsensitiveASCII(*type, u"text/css")) {
    return;
  }
  Document& document = GetDocument();
  const std::u16string* media = FindAttribute(u"media");
  const bool media_matches = !media || MediaAttributeMatches(document, *media);
  // ComputePendingSheetTypeAndRenderBlockingBehavior(), as a link's sheet
  // has it. A document of no window, as DOMParser's, has no scripts or
  // rendering to hold back, and its parser goes on past a sheet whose import
  // waits, as in Chrome.
  Document::PendingSheetType sheet_type =
      Document::PendingSheetType::kNonBlocking;
  bool render_blocking = false;
  if (media_matches && document.DefaultView()) {
    if (created_by_parser_) {
      HTMLElement* const body = document.Body();
      sheet_type = Document::PendingSheetType::kBlocking;
      render_blocking = !body || !body->IsInclusiveAncestorOf(*this);
    } else if (IsPotentiallyRenderBlocking()) {
      sheet_type = Document::PendingSheetType::kDynamicRenderBlocking;
      render_blocking = true;
    }
  }
  pending_sheet_type_ = sheet_type;
  // StyleEngine::CreateSheet(): the sheet counts as pending before it
  // parses, and its imports resolve against the document's URL and go with
  // the document's referrer and encoding.
  loading_ = true;
  if (sheet_type != Document::PendingSheetType::kNonBlocking) {
    document.AddPendingBlockingSheet(*this, sheet_type);
  }
  sheet_ = bindings::MakeGarbageCollected<StyleSheetContents>(
      document, GURL(), GURL(), StyleSheetReferrer(), document.Encoding(), this,
      nullptr);
  sheet_->SetRenderBlocking(render_blocking);
  sheet_->ParseAuthorStyleSheet(ChildTextContent());
  loading_ = false;
  sheet_->CheckLoaded();
}

void HTMLStyleElement::ClearSheet() {
  if (sheet_->IsLoading()) {
    if (pending_sheet_type_ != Document::PendingSheetType::kNonBlocking &&
        pending_sheet_type_ != Document::PendingSheetType::kNone) {
      GetDocument().RemovePendingBlockingSheet(*this, pending_sheet_type_);
    }
    pending_sheet_type_ = Document::PendingSheetType::kNone;
  }
  sheet_->ClearOwner();
  sheet_ = nullptr;
}

bool HTMLStyleElement::IsLoading() const {
  return loading_ || (sheet_ && sheet_->IsLoading());
}

bool HTMLStyleElement::IsPotentiallyRenderBlocking() const {
  if (const std::u16string* blocking = FindAttribute(u"blocking")) {
    for (const std::u16string_view token :
         base::SplitStringPiece(*blocking, u" \t\n\f\r", base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY)) {
      if (token == u"render") {
        return true;
      }
    }
  }
  return created_by_parser_;
}

void HTMLStyleElement::SheetLoaded(bool error) {
  // StyleElement::SheetLoaded(), then the element's events.
  if (IsLoading()) {
    return;
  }
  if (pending_sheet_type_ != Document::PendingSheetType::kNonBlocking &&
      pending_sheet_type_ != Document::PendingSheetType::kNone) {
    GetDocument().RemovePendingBlockingSheet(*this, pending_sheet_type_);
  }
  pending_sheet_type_ = Document::PendingSheetType::kNone;
  ScheduleEvent(/*loaded=*/!error);
}

void HTMLStyleElement::ScheduleEvent(bool loaded) {
  Document& document = GetDocument();
  // The old document of a window that a navigation has replaced fires
  // nothing. One of no window, as DOMImplementation and DOMParser make,
  // fires from the tasks of the window whose script made it, as Blink's
  // Document::GetTaskRunner() takes its execution context's.
  Window* const window = document.DefaultView();
  if (window &&
      (!window->HasContext() || &window->GetDocument() != &document)) {
    return;
  }
  WindowTasks* const tasks = document.GetTasks();
  if (!tasks) {
    return;
  }
  // From the networking task source, so that the sheet applies before the
  // next script loads, holding a window's load event back until it fires.
  const bool delays_load_event = window != nullptr;
  if (delays_load_event) {
    document.IncrementLoadEventDelayCount();
  }
  tasks->PostTask(
      TaskType::kNetworking,
      base::BindOnce(
          [](const cppgc::Persistent<HTMLStyleElement>& style,
             const cppgc::WeakPersistent<Document>& document, bool loaded,
             bool delays_load_event) {
            FireSimpleEvent(*style, loaded ? u"load" : u"error");
            if (delays_load_event && document) {
              document->DecrementLoadEventDelayCountAndCheckLoadEvent();
            }
          },
          cppgc::Persistent<HTMLStyleElement>(this),
          cppgc::WeakPersistent<Document>(&document), loaded,
          delays_load_event));
}

}  // namespace mitto
