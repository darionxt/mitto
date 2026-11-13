// A script element's part in the HTML Standard's script processing model:
// its already started, parser document and force async, and "prepare the
// script element", which runs an inline classic script at once, hands an
// async or in-order one to its document's DocumentScriptRunner, and gives
// the parser the script that it waits for or runs after parsing.
//
// Upstream: third_party/blink/renderer/core/script/script_loader.h
// Upstream: third_party/blink/renderer/core/script/script_loader.cc

#ifndef MITTO_DOM_SCRIPT_LOADER_H_
#define MITTO_DOM_SCRIPT_LOADER_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/dom/pending_script.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class HTMLScriptElement;

class ScriptLoader final : public cppgc::GarbageCollected<ScriptLoader> {
 public:
  // The HTML Standard's script type, as Blink tells them apart.
  enum class ScriptTypeAtPrepare {
    // A data block, which does not run.
    kInvalid,
    kClassic,
    kModule,
    kImportMap,
    kSpeculationRules,
    kWebBundle,
  };

  // Whether an inline script that the parser inserted may wait for style
  // sheets that block scripts: only while the parser's script nesting level
  // is 1.
  enum class ParserBlockingInlineOption {
    kDeny,
    kAllow,
  };

  explicit ScriptLoader(HTMLScriptElement& element);
  ~ScriptLoader();

  void Trace(cppgc::Visitor* visitor) const;

  // The HTML Standard's script block's type string, from `type` and
  // `language`, null for a missing attribute.
  static ScriptTypeAtPrepare GetScriptTypeAtPrepare(
      const std::optional<std::u16string>& type,
      const std::optional<std::u16string>& language);

  // The parser makes its script elements parser-inserted, with `document`
  // as their parser document and no force async. One that the fragment
  // parser makes has already started, so it never runs.
  void SetCreatedByParser(Document& document, bool already_started);
  // A copy of a script that has already started has too.
  void SetAlreadyStarted() { already_started_ = true; }

  bool IsParserInserted() const { return parser_inserted_; }
  bool AlreadyStarted() const { return already_started_; }
  bool IsForceAsync() const { return force_async_; }
  ScriptTypeAtPrepare GetScriptType() const { return script_type_; }

  // When a script that the parser did not insert prepares itself: once it
  // becomes connected, once something is inserted into it while it is
  // connected, and once it gets a src while connected.
  void DidNotifySubtreeInsertionsToDocument();
  void ChildrenChanged(bool child_insertion);
  void HandleSourceAttribute(const std::u16string& source);
  // An async attribute added, or the async setter: no force async.
  void HandleAsyncAttribute() { force_async_ = false; }

  // The HTML Standard's "prepare the script element". Returns the script for
  // the parser to wait for or to run after parsing, for a parser-inserted
  // script, and null otherwise.
  PendingScript* PrepareScript(ParserBlockingInlineOption option);

 private:
  // GetScriptSchedulingTypePerSpec().
  ScriptSchedulingType GetScriptSchedulingTypePerSpec(
      Document& element_document,
      ParserBlockingInlineOption option) const;
  // The event and for attributes: a classic script runs only for the
  // window's load.
  bool IsScriptForEventSupported() const;
  PendingScript* TakePendingScript(ScriptSchedulingType type);

  cppgc::Member<HTMLScriptElement> element_;
  // The HTML Standard's parser document, which Blink keeps as a flag beside
  // the document.
  bool parser_inserted_ = false;
  cppgc::WeakMember<Document> parser_document_;
  bool already_started_ = false;
  bool force_async_ = true;
  bool is_external_script_ = false;
  ScriptTypeAtPrepare script_type_ = ScriptTypeAtPrepare::kInvalid;
  cppgc::Member<PendingScript> prepared_pending_script_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SCRIPT_LOADER_H_
