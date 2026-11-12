// V8 as a Page runs it, started as Chrome's renderer starts it: V8's flags
// first, then its platform, then the isolate of the main thread with Blink's
// settings, and the bindings' data with the Page's features. A Page that the
// library starts does this once its start message has come, because the
// flags, the heap's limits and the features follow its Session.
//
// Upstream: content/renderer/render_process_impl.cc
// Upstream: gin/v8_initializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_initializer.cc

#ifndef MITTO_PAGE_PAGE_V8_H_
#define MITTO_PAGE_PAGE_V8_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/runtime_feature_set.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

namespace bindings {
class PerIsolateData;
}  // namespace bindings

class PagePlatform;

// What a Page starts V8 with.
struct PageV8Options {
  PageV8Options();
  PageV8Options(PageV8Options&&);
  PageV8Options& operator=(PageV8Options&&);
  ~PageV8Options();

  // Flags after the Engine Release's, such as those of --js-flags=.
  std::vector<std::string> extra_flags;
  // The Machine's memory, in GiB, from which V8 works out its heap's limits.
  int memory_gib = 8;
};

class PageV8 {
 public:
  // Starts V8 on this thread, whose task runner is `main_thread`, and enters
  // its isolate. V8 starts once in a process.
  static std::unique_ptr<PageV8> Start(
      scoped_refptr<base::SingleThreadTaskRunner> main_thread,
      const PageV8Options& options,
      bindings::RuntimeFeatureSet features);

  PageV8(const PageV8&) = delete;
  PageV8& operator=(const PageV8&) = delete;

  // Ends V8. The main thread's tasks of V8 have to go first, with the task
  // executor that holds them.
  ~PageV8();

  v8::Isolate* isolate() const { return isolate_; }

 private:
  PageV8();

  std::unique_ptr<PagePlatform> platform_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  std::unique_ptr<bindings::PerIsolateData> data_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_V8_H_
