#include "mitto/dom/document_script_runner.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/notreached.h"
#include "mitto/dom/document.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// kTimeoutForLowPriorityAsyncScriptExecution's default.
constexpr base::TimeDelta kLowPriorityTimeout = base::Seconds(1);

using WeakRunner = cppgc::WeakPersistent<DocumentScriptRunner>;
using HeldScript = cppgc::Persistent<PendingScript>;

}  // namespace

DocumentScriptRunner::DocumentScriptRunner(Document& document)
    : document_(&document) {}

DocumentScriptRunner::~DocumentScriptRunner() = default;

void DocumentScriptRunner::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  pending_in_order_scripts_.Trace(visitor);
  pending_async_scripts_.Trace(visitor);
}

void DocumentScriptRunner::QueueScriptForExecution(
    PendingScript* pending_script) {
  CHECK(pending_script);
  document_->IncrementLoadEventDelayCount();
  switch (pending_script->scheduling_type()) {
    case ScriptSchedulingType::kAsync:
      pending_async_scripts_.push_back(pending_script);
      break;
    case ScriptSchedulingType::kInOrder:
      pending_in_order_scripts_.push_back(pending_script);
      break;
    default:
      NOTREACHED();
  }
  // This can call PendingScriptFinished() at once.
  pending_script->WatchForLoad(base::BindOnce(
      [](const WeakRunner& runner,
         const cppgc::WeakPersistent<PendingScript>& pending_script) {
        if (runner && pending_script) {
          runner->PendingScriptFinished(pending_script.Get());
        }
      },
      WeakRunner(this), cppgc::WeakPersistent<PendingScript>(pending_script)));
}

void DocumentScriptRunner::PendingScriptFinished(
    PendingScript* pending_script) {
  switch (pending_script->scheduling_type()) {
    case ScriptSchedulingType::kAsync:
      pending_async_scripts_.EraseIf([pending_script](PendingScript* item) {
        return item == pending_script;
      });
      PostAsyncScript(pending_script);
      break;
    case ScriptSchedulingType::kInOrder:
      while (!pending_in_order_scripts_.empty() &&
             pending_in_order_scripts_[0]->IsReady()) {
        PendingScript* const first = pending_in_order_scripts_[0];
        pending_in_order_scripts_.EraseIf(
            [first](PendingScript* item) { return item == first; });
        Window* const window = document_->DefaultView();
        if (WindowTasks* const tasks = window ? window->GetTasks() : nullptr) {
          tasks->PostTask(TaskType::kNetworking,
                          base::BindOnce(&RunPendingScript, WeakRunner(this),
                                         HeldScript(first)));
        }
      }
      break;
    default:
      NOTREACHED();
  }
}

void DocumentScriptRunner::PostAsyncScript(PendingScript* pending_script) {
  Window* const window = document_->DefaultView();
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  if (!pending_script->IsEligibleForLowPriorityAsyncScriptExecution()) {
    tasks->PostTask(TaskType::kNetworking,
                    base::BindOnce(&RunPendingScript, WeakRunner(this),
                                   HeldScript(pending_script)));
    return;
  }
  // The script runs from whichever task comes first: one of a low priority,
  // or one of the normal priority a second later.
  auto ran = base::MakeRefCounted<base::RefCountedData<bool>>(false);
  const auto run_once = [](scoped_refptr<base::RefCountedData<bool>> ran,
                           const WeakRunner& runner,
                           const HeldScript& pending_script) {
    if (ran->data) {
      return;
    }
    ran->data = true;
    RunPendingScript(runner, pending_script);
  };
  tasks->PostTask(TaskType::kLowPriorityScriptExecution,
                  base::BindOnce(run_once, ran, WeakRunner(this),
                                 HeldScript(pending_script)));
  tasks->PostDelayedTask(TaskType::kNetworking,
                         base::BindOnce(run_once, ran, WeakRunner(this),
                                        HeldScript(pending_script)),
                         kLowPriorityTimeout);
}

// static
void DocumentScriptRunner::RunPendingScript(const WeakRunner& runner,
                                            const HeldScript& pending_script) {
  if (runner) {
    runner->ExecutePendingScript(pending_script.Get());
  }
}

void DocumentScriptRunner::ExecutePendingScript(PendingScript* pending_script) {
  pending_script->ExecuteScriptBlock();
  document_->DecrementLoadEventDelayCount();
}

}  // namespace mitto
