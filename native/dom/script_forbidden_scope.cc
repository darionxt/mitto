#include "mitto/dom/script_forbidden_scope.h"

namespace mitto {
namespace {

// How many scopes are on the stack of the thread, the main one or a dedicated
// worker's.
constinit thread_local int g_script_forbidden_count = 0;

}  // namespace

ScriptForbiddenScope::ScriptForbiddenScope() {
  ++g_script_forbidden_count;
}

ScriptForbiddenScope::~ScriptForbiddenScope() {
  --g_script_forbidden_count;
}

// static
bool ScriptForbiddenScope::IsScriptForbidden() {
  return g_script_forbidden_count > 0;
}

}  // namespace mitto
