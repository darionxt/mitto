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
#include "mitto/dom/document_write_intervention.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/script_element_base.h"
#include "mitto/dom/subresource_integrity.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "url/gurl.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// Blink's IsSpaceOrNewline(): ASCII whitespace with the vertical tab, and
// beyond ASCII each character whose bidirectional class is white space, such
// as U+1680 and U+3000.
bool IsSpaceOrNewline(char16_t c) {
  return c <= 0x7F ? c == u' ' || (c >= u'\t' && c <= u'\r')
                   : u_charDirection(c) == U_WHITE_SPACE_NEUTRAL;
}

// Blink's String::StripWhiteSpace().
std::u16string StripWhiteSpace(std::u16string_view text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && IsSpaceOrNewline(text[start])) {
    ++start;
  }
  while (end > start && IsSpaceOrNewline(text[end - 1])) {
    --end;
  }
  return std::u16string(text.substr(start, end - start));
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
void QueueErrorEvent(ScriptElementBase& element) {
  Window* const window = element.AsElement().GetDocument().DefaultView();
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::Persistent<Element>& element) {
                        ToScriptElement(*element)->DispatchErrorEvent();
                      },
                      cppgc::Persistent<Element>(&element.AsElement())));
}

// Steps 18, 25 and 26's request of a classic script: the mode and credentials
// mode that its crossorigin attribute asks for, its referrerpolicy, and the
// priority that Blink gives it, which a lazy one starts low at, as an async,
// deferred or script-inserted one is, and which its fetchpriority and its
// blocking rendering move.
// Upstream: third_party/blink/renderer/core/script/script_loader.cc
// Upstream: third_party/blink/renderer/core/html/cross_origin_attribute.cc
FetchParameters ClassicScriptParams(ScriptElementBase& element,
                                    const GURL& url,
                                    bool parser_inserted,
                                    bool render_blocking) {
  FetchParameters params;
  params.url = url;
  params.type = ResourceType::kScript;
  if (const std::optional<std::u16string> cross_origin =
          element.CrossOriginAttributeValue()) {
    params.mode = FetchMode::kCors;
    params.credentials =
        base::EqualsCaseInsensitiveASCII(*cross_origin, u"use-credentials")
            ? FetchCredentialsMode::kInclude
            : FetchCredentialsMode::kSameOrigin;
  }
  params.referrer_policy = FetchReferrerPolicyFromString(
      element.ReferrerPolicyAttributeValue().value_or(u""));
  params.lazy = !parser_inserted || element.AsyncAttributeValue() ||
                element.DeferAttributeValue();
  params.priority_hint = FetchPriorityHintFromAttribute(
      element.FetchPriorityAttributeValue().value_or(u""));
  params.render_blocking = render_blocking;
  return params;
}

}  // namespace

ScriptLoader::ScriptLoader(ScriptElementBase& element)
    : element_(&element.AsElement()) {}

ScriptElementBase& ScriptLoader::ScriptElement() const {
  return *ToScriptElement(*element_);
}

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
  const std::optional<std::u16string> event =
      ScriptElement().EventAttributeValue();
  const std::optional<std::u16string> for_attribute =
      ScriptElement().ForAttributeValue();
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
  if (was_parser_inserted && !ScriptElement().AsyncAttributeValue()) {
    force_async_ = true;
  }
  // Steps 5 to 7.
  const std::u16string source_text = element_->ChildTextContent();
  const std::optional<std::u16string> src =
      ScriptElement().SourceAttributeValue();
  if (!src && source_text.empty()) {
    return nullptr;
  }
  if (!element_->IsConnected()) {
    return nullptr;
  }
  Document& element_document = element_->GetDocument();
  Window* const context_window = element_document.DefaultView();
  // Steps 8 to 11.
  script_type_ =
      GetScriptTypeAtPrepare(ScriptElement().TypeAttributeValue(),
                             ScriptElement().LanguageAttributeValue());
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
      ScriptElement().NomoduleAttributeValue()) {
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
        ScriptElement().IntegrityAttributeValue();
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
      ScriptElement().CharsetAttributeValue();
  std::string encoding = charset ? CanonicalTextEncodingName(*charset) : "";
  if (encoding.empty()) {
    encoding = element_document.Encoding();
  }
  const bool potentially_render_blocking =
      ScriptElement().IsPotentiallyRenderBlocking();

  if (src) {
    // Step 31.
    if (script_type_ == ScriptTypeAtPrepare::kImportMap) {
      QueueErrorEvent(ScriptElement());
      return nullptr;
    }
    const std::u16string stripped_src = StripHtmlSpaces(*src);
    if (stripped_src.empty()) {
      QueueErrorEvent(ScriptElement());
      return nullptr;
    }
    is_external_script_ = true;
    const GURL url = element_document.CompleteUrl(stripped_src);
    if (!url.is_valid()) {
      QueueErrorEvent(ScriptElement());
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
        QueueErrorEvent(ScriptElement());
        return nullptr;
      case ScriptTypeAtPrepare::kWebBundle:
        context_window->AddConsoleMessage(
            ConsoleLevel::kError, "External webbundle is not yet supported.");
        QueueErrorEvent(ScriptElement());
        return nullptr;
      case ScriptTypeAtPrepare::kModule:
        // Module scripts are a Known Gap.
        return nullptr;
      case ScriptTypeAtPrepare::kClassic:
        // Step 31.7: block rendering on el, which Blink does ahead of the
        // steps of each type. A module script, a Known Gap that never runs,
        // blocks nothing.
        if (potentially_render_blocking) {
          element_document.AddPendingRenderBlockingScript(
              ScriptElement().AsElement());
        }
        // Chrome's intervention against a script of another site that
        // blocks the parser and that document.write() inserted.
        MaybeDisallowFetchForDocWrittenScript(
            url,
            !parser_inserted_ || ScriptElement().AsyncAttributeValue() ||
                ScriptElement().DeferAttributeValue(),
            element_document);
        FetchParameters params =
            ClassicScriptParams(ScriptElement(), url, parser_inserted_,
                                potentially_render_blocking);
        params.integrity = std::move(integrity);
        params.charset = std::move(encoding);
        prepared_pending_script_ =
            PendingScript::Fetch(ScriptElement(), params);
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
      case ScriptTypeAtPrepare::kClassic: {
        // DevTools names an inline script that the parser inserted after its
        // document, with its errors counted from where its text starts, and
        // one that the DOM inserted, or that document.write() wrote, after
        // nothing, from its start, as Blink resets the position in a write.
        const bool named =
            parser_inserted_ && !element_document.IsInDocumentWrite();
        prepared_pending_script_ = PendingScript::CreateInline(
            ScriptElement(), source_text,
            named ? GURL(element_document.UrlForBinding()) : GURL(),
            named ? ScriptElement().StartLine() : 0,
            named ? ScriptElement().StartColumn() : 0);
        break;
      }
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
    if (ScriptElement().AsyncAttributeValue() || force_async_) {
      return ScriptSchedulingType::kAsync;
    }
    if (!parser_inserted_) {
      return ScriptSchedulingType::kInOrder;
    }
    if (ScriptElement().DeferAttributeValue()) {
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
