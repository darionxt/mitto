#include "mitto/dom/script_loader.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_script_runner.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/subresource_integrity.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// The HTML Standard's ASCII whitespace, and the vertical tab that Blink's
// StripWhiteSpace() strips too.
constexpr std::u16string_view kStripped = u" \t\n\v\f\r";

std::u16string StripWhiteSpace(std::u16string_view text) {
  return std::u16string(base::TrimString(text, kStripped, base::TRIM_ALL));
}

// StripLeadingAndTrailingHtmlSpaces(): the HTML Standard's ASCII whitespace.
std::u16string StripHtmlSpaces(std::u16string_view text) {
  return std::u16string(base::TrimString(text, u" \t\n\f\r", base::TRIM_ALL));
}

// MIMETypeRegistry::IsSupportedJavaScriptMIMEType(), which ignores case.
bool IsSupportedJavaScriptMimeType(std::u16string_view mime_type) {
  static constexpr std::u16string_view kJavaScriptMimeTypes[] = {
      u"application/ecmascript",
      u"application/javascript",
      u"application/x-ecmascript",
      u"application/x-javascript",
      u"text/ecmascript",
      u"text/javascript",
      u"text/javascript1.0",
      u"text/javascript1.1",
      u"text/javascript1.2",
      u"text/javascript1.3",
      u"text/javascript1.4",
      u"text/javascript1.5",
      u"text/jscript",
      u"text/livescript",
      u"text/x-ecmascript",
      u"text/x-javascript",
  };
  for (const std::u16string_view javascript : kJavaScriptMimeTypes) {
    if (base::EqualsCaseInsensitiveASCII(mime_type, javascript)) {
      return true;
    }
  }
  return false;
}

// IsValidClassicScriptTypeAndLanguage(): steps 8 and 9 of prepare the script
// element.
bool IsValidClassicScriptTypeAndLanguage(
    const std::optional<std::u16string>& type,
    const std::optional<std::u16string>& language) {
  if (!type) {
    if (!language || language->empty()) {
      return true;
    }
    return IsSupportedJavaScriptMimeType(u"text/" + *language);
  }
  if (type->empty()) {
    return true;
  }
  return IsSupportedJavaScriptMimeType(StripWhiteSpace(*type));
}

bool TypeIs(const std::optional<std::u16string>& type,
            std::u16string_view name) {
  return type && base::EqualsCaseInsensitiveASCII(*type, name);
}

// Queues an element task on the DOM manipulation task source to fire error
// at `element`.
void QueueErrorEvent(HTMLScriptElement& element) {
  Window* const window = element.GetDocument().DefaultView();
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::Persistent<HTMLScriptElement>& element) {
                        element->DispatchErrorEvent();
                      },
                      cppgc::Persistent<HTMLScriptElement>(&element)));
}

// Steps 18, 25 and 26's request of a classic script: the mode and credentials
// mode that its crossorigin attribute asks for, its referrerpolicy, and the
// priority that Blink gives it, which a lazy one starts low at, as an async,
// deferred or script-inserted one is, and which its fetchpriority and its
// blocking rendering move.
// Upstream: third_party/blink/renderer/core/script/script_loader.cc
// Upstream: third_party/blink/renderer/core/html/cross_origin_attribute.cc
FetchRequestData ClassicScriptRequest(HTMLScriptElement& element,
                                      bool parser_inserted,
                                      bool render_blocking) {
  FetchRequestData request;
  if (const std::optional<std::u16string> cross_origin =
          element.GetAttribute(u"crossorigin")) {
    request.mode = FetchMode::kCors;
    request.credentials =
        base::EqualsCaseInsensitiveASCII(*cross_origin, u"use-credentials")
            ? FetchCredentialsMode::kInclude
            : FetchCredentialsMode::kSameOrigin;
  }
  request.referrer_policy = FetchReferrerPolicyFromString(
      element.GetAttribute(u"referrerpolicy").value_or(u""));
  const bool lazy = !parser_inserted || element.HasAttribute(u"async") ||
                    element.HasAttribute(u"defer");
  request.priority = ComputeFetchPriority(
      lazy,
      FetchPriorityHintFromAttribute(
          element.GetAttribute(u"fetchpriority").value_or(u"")),
      render_blocking);
  return request;
}

}  // namespace

ScriptLoader::ScriptLoader(HTMLScriptElement& element) : element_(&element) {}

ScriptLoader::~ScriptLoader() = default;

void ScriptLoader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(element_);
  visitor->Trace(parser_document_);
  visitor->Trace(prepared_pending_script_);
}

// static
ScriptLoader::ScriptTypeAtPrepare ScriptLoader::GetScriptTypeAtPrepare(
    const std::optional<std::u16string>& type,
    const std::optional<std::u16string>& language) {
  if (IsValidClassicScriptTypeAndLanguage(type, language)) {
    return ScriptTypeAtPrepare::kClassic;
  }
  if (TypeIs(type, u"module")) {
    return ScriptTypeAtPrepare::kModule;
  }
  if (TypeIs(type, u"importmap")) {
    return ScriptTypeAtPrepare::kImportMap;
  }
  if (TypeIs(type, u"speculationrules")) {
    return ScriptTypeAtPrepare::kSpeculationRules;
  }
  if (TypeIs(type, u"webbundle")) {
    return ScriptTypeAtPrepare::kWebBundle;
  }
  return ScriptTypeAtPrepare::kInvalid;
}

void ScriptLoader::SetCreatedByParser(Document& document) {
  parser_inserted_ = true;
  parser_document_ = &document;
  force_async_ = false;
}

void ScriptLoader::DidNotifySubtreeInsertionsToDocument() {
  if (!parser_inserted_) {
    PrepareScript(ParserBlockingInlineOption::kDeny);
  }
}

void ScriptLoader::ChildrenChanged(bool child_insertion) {
  if (child_insertion && !parser_inserted_ && element_->IsConnected()) {
    PrepareScript(ParserBlockingInlineOption::kDeny);
  }
}

void ScriptLoader::HandleSourceAttribute(const std::u16string& source) {
  if (!parser_inserted_ && element_->IsConnected() && !source.empty()) {
    PrepareScript(ParserBlockingInlineOption::kDeny);
  }
}

bool ScriptLoader::IsScriptForEventSupported() const {
  const std::optional<std::u16string> event = element_->GetAttribute(u"event");
  const std::optional<std::u16string> for_attribute =
      element_->GetAttribute(u"for");
  if (script_type_ != ScriptTypeAtPrepare::kClassic || !event ||
      !for_attribute) {
    return true;
  }
  if (!base::EqualsCaseInsensitiveASCII(StripWhiteSpace(*for_attribute),
                                        u"window")) {
    return false;
  }
  const std::u16string stripped_event = StripWhiteSpace(*event);
  return base::EqualsCaseInsensitiveASCII(stripped_event, u"onload") ||
         base::EqualsCaseInsensitiveASCII(stripped_event, u"onload()");
}

PendingScript* ScriptLoader::PrepareScript(ParserBlockingInlineOption option) {
  // Step 1.
  if (already_started_) {
    return nullptr;
  }
  // Steps 2 to 4.
  const bool was_parser_inserted = parser_inserted_;
  parser_inserted_ = false;
  if (was_parser_inserted && !element_->HasAttribute(u"async")) {
    force_async_ = true;
  }
  // Steps 5 to 7.
  const std::u16string source_text = element_->ChildTextContent();
  const std::optional<std::u16string> src = element_->GetAttribute(u"src");
  if (!src && source_text.empty()) {
    return nullptr;
  }
  if (!element_->IsConnected()) {
    return nullptr;
  }
  Document& element_document = element_->GetDocument();
  Window* const context_window = element_document.DefaultView();
  // Steps 8 to 11.
  script_type_ = GetScriptTypeAtPrepare(element_->GetAttribute(u"type"),
                                        element_->GetAttribute(u"language"));
  if (script_type_ == ScriptTypeAtPrepare::kInvalid) {
    return nullptr;
  }
  // Steps 12 and 13.
  if (was_parser_inserted) {
    parser_inserted_ = true;
    force_async_ = false;
  }
  already_started_ = true;
  // Step 15.
  if (parser_inserted_ && parser_document_ != &element_document) {
    return nullptr;
  }
  // Step 16: scripting is disabled for a document without a window.
  if (!context_window) {
    return nullptr;
  }
  // Step 17.
  if (script_type_ == ScriptTypeAtPrepare::kClassic &&
      element_->HasAttribute(u"nomodule")) {
    return nullptr;
  }
  // Step 19.
  if (!IsScriptForEventSupported()) {
    return nullptr;
  }
  // Step 24, with Blink's console errors for what the integrity attribute
  // skips.
  IntegrityMetadataSet integrity;
  if (src) {
    const std::optional<std::u16string> integrity_attribute =
        element_->GetAttribute(u"integrity");
    if (integrity_attribute && !integrity_attribute->empty()) {
      std::vector<std::string> messages;
      integrity = ParseIntegrityAttribute(*integrity_attribute, &messages);
      for (std::string& message : messages) {
        context_window->AddConsoleMessage(ConsoleLevel::kError,
                                          std::move(message));
      }
    }
  }
  // Steps 21 and 22: the encoding that the charset attribute names, with no
  // white space trimmed, or else the node document's.
  const std::optional<std::u16string> charset =
      element_->GetAttribute(u"charset");
  std::string encoding = charset ? CanonicalTextEncodingName(*charset) : "";
  if (encoding.empty()) {
    encoding = element_document.Encoding();
  }
  const bool potentially_render_blocking =
      element_->IsPotentiallyRenderBlocking();

  if (src) {
    // Step 31.
    if (script_type_ == ScriptTypeAtPrepare::kImportMap) {
      QueueErrorEvent(*element_);
      return nullptr;
    }
    const std::u16string stripped_src = StripHtmlSpaces(*src);
    if (stripped_src.empty()) {
      QueueErrorEvent(*element_);
      return nullptr;
    }
    is_external_script_ = true;
    const GURL url = element_document.CompleteUrl(stripped_src);
    if (!url.is_valid()) {
      QueueErrorEvent(*element_);
      return nullptr;
    }
    switch (script_type_) {
      case ScriptTypeAtPrepare::kInvalid:
      case ScriptTypeAtPrepare::kImportMap:
        NOTREACHED();
      case ScriptTypeAtPrepare::kSpeculationRules:
        context_window->AddConsoleMessage(
            ConsoleLevel::kError,
            "External speculation rules are not yet supported.");
        QueueErrorEvent(*element_);
        return nullptr;
      case ScriptTypeAtPrepare::kWebBundle:
        context_window->AddConsoleMessage(
            ConsoleLevel::kError, "External webbundle is not yet supported.");
        QueueErrorEvent(*element_);
        return nullptr;
      case ScriptTypeAtPrepare::kModule:
        // Module scripts are a Known Gap.
        return nullptr;
      case ScriptTypeAtPrepare::kClassic:
        prepared_pending_script_ = PendingScript::Fetch(
            *element_, url,
            ClassicScriptRequest(*element_, parser_inserted_,
                                 potentially_render_blocking),
            std::move(integrity), std::move(encoding),
            potentially_render_blocking);
        break;
    }
  } else {
    // Step 32.
    switch (script_type_) {
      case ScriptTypeAtPrepare::kInvalid:
        NOTREACHED();
      case ScriptTypeAtPrepare::kImportMap:
      case ScriptTypeAtPrepare::kWebBundle:
      case ScriptTypeAtPrepare::kSpeculationRules:
      case ScriptTypeAtPrepare::kModule:
        // Import maps, web bundles, speculation rules and module scripts are
        // Known Gaps.
        return nullptr;
      case ScriptTypeAtPrepare::kClassic:
        // DevTools names an inline script that the parser inserted after its
        // document, with its errors counted from where its text starts, and
        // one that the DOM inserted after nothing, from its start.
        prepared_pending_script_ = PendingScript::CreateInline(
            *element_, source_text,
            parser_inserted_ ? GURL(element_document.UrlForBinding()) : GURL(),
            parser_inserted_ ? element_->StartLine() : 0,
            parser_inserted_ ? element_->StartColumn() : 0);
        break;
    }
  }

  const ScriptSchedulingType scheduling_type =
      GetScriptSchedulingTypePerSpec(element_document, option);
  switch (scheduling_type) {
    case ScriptSchedulingType::kAsync:
    case ScriptSchedulingType::kInOrder:
      context_window->GetDocument().GetScriptRunner().QueueScriptForExecution(
          TakePendingScript(scheduling_type));
      return nullptr;
    case ScriptSchedulingType::kDefer:
    case ScriptSchedulingType::kParserBlocking:
    case ScriptSchedulingType::kParserBlockingInline:
      DCHECK(parser_inserted_);
      return TakePendingScript(scheduling_type);
    case ScriptSchedulingType::kImmediate:
      TakePendingScript(ScriptSchedulingType::kImmediate)->ExecuteScriptBlock();
      return nullptr;
    case ScriptSchedulingType::kNotSet:
      NOTREACHED();
  }
}

ScriptSchedulingType ScriptLoader::GetScriptSchedulingTypePerSpec(
    Document& element_document,
    ParserBlockingInlineOption option) const {
  // Step 31 for an external classic script.
  if (is_external_script_) {
    if (element_->HasAttribute(u"async") || force_async_) {
      return ScriptSchedulingType::kAsync;
    }
    if (!parser_inserted_) {
      return ScriptSchedulingType::kInOrder;
    }
    if (element_->HasAttribute(u"defer")) {
      return ScriptSchedulingType::kDefer;
    }
    return ScriptSchedulingType::kParserBlocking;
  }
  // Step 32.
  if (parser_inserted_ && option == ParserBlockingInlineOption::kAllow &&
      !element_document.IsScriptExecutionReady()) {
    return ScriptSchedulingType::kParserBlockingInline;
  }
  return ScriptSchedulingType::kImmediate;
}

PendingScript* ScriptLoader::TakePendingScript(ScriptSchedulingType type) {
  CHECK(prepared_pending_script_);
  PendingScript* const pending_script = prepared_pending_script_.Get();
  prepared_pending_script_ = nullptr;
  pending_script->set_scheduling_type(type);
  return pending_script;
}

}  // namespace mitto
