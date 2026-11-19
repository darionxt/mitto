#include "mitto/dom/pipe_options.h"

#include <optional>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/abort_signal.h"

namespace mitto {

PipeOptions::PipeOptions() = default;

PipeOptions::~PipeOptions() = default;

PipeOptions::PipeOptions(
    const std::optional<bindings::StreamPipeOptions>& options)
    : prevent_close_(options && options->prevent_close),
      prevent_abort_(options && options->prevent_abort),
      prevent_cancel_(options && options->prevent_cancel),
      signal_(options && options->signal ? *options->signal : nullptr) {}

void PipeOptions::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(signal_);
}

}  // namespace mitto
