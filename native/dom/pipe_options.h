// The options of a pipe, as Blink's PipeOptions keeps StreamPipeOptions.
//
// Upstream: third_party/blink/renderer/core/streams/pipe_options.h
// Upstream: third_party/blink/renderer/core/streams/pipe_options.cc

#ifndef MITTO_DOM_PIPE_OPTIONS_H_
#define MITTO_DOM_PIPE_OPTIONS_H_

#include <optional>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
struct StreamPipeOptions;
}  // namespace bindings

class AbortSignal;

class PipeOptions final : public cppgc::GarbageCollected<PipeOptions> {
 public:
  PipeOptions();
  explicit PipeOptions(
      const std::optional<bindings::StreamPipeOptions>& options);
  ~PipeOptions();

  bool PreventClose() const { return prevent_close_; }
  bool PreventAbort() const { return prevent_abort_; }
  bool PreventCancel() const { return prevent_cancel_; }
  AbortSignal* Signal() const { return signal_.Get(); }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  bool prevent_close_ = false;
  bool prevent_abort_ = false;
  bool prevent_cancel_ = false;
  cppgc::Member<AbortSignal> signal_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PIPE_OPTIONS_H_
