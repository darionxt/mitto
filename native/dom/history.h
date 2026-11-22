// The HTML Standard's History, the window's history: the session history of
// the tab that a Session opened, which the Page keeps (session_history.h in
// the Page program), as Blink's History reads it through its frame's client.
// pushState() and replaceState() add or replace an entry within the document,
// with a state that the Runtime's SerializedScriptValue writes as Blink's does
// for storage, and back(), forward() and go() move through the entries from a
// later task.
//
// Upstream: third_party/blink/renderer/core/frame/history.cc

#ifndef MITTO_DOM_HISTORY_H_
#define MITTO_DOM_HISTORY_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class Window;

class History final : public bindings::ScriptWrappable {
 public:
  static History* Create(Window* window);
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit History(Window* window);
  ~History() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. Each throws Blink's SecurityError once the
  // window's document is no longer fully active.
  uint32_t Length(bindings::ExceptionState& exception_state) const;
  std::u16string ScrollRestoration(
      bindings::ExceptionState& exception_state) const;
  void SetScrollRestoration(const std::u16string& value,
                            bindings::ExceptionState& exception_state);
  v8::Local<v8::Value> State(bindings::ExceptionState& exception_state);
  void Go(int32_t delta, bindings::ExceptionState& exception_state);
  void Back(bindings::ExceptionState& exception_state);
  void Forward(bindings::ExceptionState& exception_state);
  void PushState(v8::Local<v8::Value> data,
                 const std::u16string& title,
                 const std::optional<std::u16string>& url,
                 bindings::ExceptionState& exception_state);
  void ReplaceState(v8::Local<v8::Value> data,
                    const std::u16string& title,
                    const std::optional<std::u16string>& url,
                    bindings::ExceptionState& exception_state);

  // What a PopStateEvent of the history reads, so that its state is the
  // object that history.state gives while the entry keeps the same state.
  bool IsSameAsCurrentState(const SerializedScriptValue* state) const;
  v8::Local<v8::Value> StateHelper(bindings::ExceptionState& exception_state);

 private:
  bool IsFullyActive() const;
  // The current entry's state, none for a window that is not fully active.
  std::shared_ptr<const SerializedScriptValue> StateInternal() const;
  void StateObjectAdded(v8::Local<v8::Value> data,
                        const std::optional<std::u16string>& url,
                        bool replace,
                        bindings::ExceptionState& exception_state);

  cppgc::Member<Window> window_;
  // What history.state gave last, and for which state, as Blink keeps it so
  // that each read gives the same object while the state stays.
  std::shared_ptr<const SerializedScriptValue> last_state_object_requested_;
  v8::TracedReference<v8::Value> cached_state_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HISTORY_H_
