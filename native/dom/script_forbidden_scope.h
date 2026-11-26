// Blink's ScriptForbiddenScope, as far as the Runtime asks it: no script may
// run on its thread while one is on the stack, as while an execution context
// that ends tells what it held, so that a load that stops then leaves the
// page's own stream alone, where it would cancel it at any other time.
//
// Upstream: third_party/blink/renderer/platform/bindings/script_forbidden_scope.h
// Upstream: third_party/blink/renderer/platform/bindings/script_forbidden_scope.cc

#ifndef MITTO_DOM_SCRIPT_FORBIDDEN_SCOPE_H_
#define MITTO_DOM_SCRIPT_FORBIDDEN_SCOPE_H_

#include "base/memory/stack_allocated.h"

namespace mitto {

class ScriptForbiddenScope {
  STACK_ALLOCATED();

 public:
  ScriptForbiddenScope();
  ScriptForbiddenScope(const ScriptForbiddenScope&) = delete;
  ScriptForbiddenScope& operator=(const ScriptForbiddenScope&) = delete;
  ~ScriptForbiddenScope();

  static bool IsScriptForbidden();
};

}  // namespace mitto

#endif  // MITTO_DOM_SCRIPT_FORBIDDEN_SCOPE_H_
