#include "mitto/page/page_v8.h"

#include <stdint.h>

#include <utility>

#include "base/check.h"
#include "base/memory/ptr_util.h"
#include "base/numerics/safe_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/page/page_isolate.h"
#include "mitto/page/page_platform.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/v8_flags.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-snapshot.h"

// The program holds V8's startup snapshot, which page/BUILD.gn writes into it,
// so that it opens no file to start.
extern "C" {
extern const char mitto_v8_snapshot[];
extern const uint64_t mitto_v8_snapshot_size;
}

namespace mitto {

PageV8Options::PageV8Options() = default;
PageV8Options::PageV8Options(PageV8Options&&) = default;
PageV8Options& PageV8Options::operator=(PageV8Options&&) = default;
PageV8Options::~PageV8Options() = default;

PageV8::PageV8() = default;

// static
std::unique_ptr<PageV8> PageV8::Start(
    scoped_refptr<base::SingleThreadTaskRunner> main_thread,
    const PageV8Options& options,
    bindings::RuntimeFeatureSet features) {
  auto page = base::WrapUnique(new PageV8());
  // Flags come before V8's platform, as gin sets them, and V8 freezes them
  // when it initialises.
  SetV8Flags(EngineReleaseV8Flags(options.memory_gib));
  SetV8Flags(options.extra_flags);
  // RenderProcessImpl(): features::kWebAssemblyTrapHandler is on for 64-bit
  // desktops, and a guard region around WebAssembly's memory catches what
  // would otherwise be bounds checks. V8's own signal handler catches the
  // trap, as on Windows and macOS; on Linux Chrome's crash handler hands it to
  // V8 first, which comes to the same.
  // Upstream: content/public/renderer/content_renderer_client.cc
  v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/true);

  // Before anything of V8 asks for a random number, as in Chrome's renderers.
  v8::V8::SetEntropySource(&GenerateEntropy);
  static v8::StartupData snapshot = {
      mitto_v8_snapshot, base::checked_cast<int>(mitto_v8_snapshot_size)};
  v8::V8::SetSnapshotDataBlob(&snapshot);
  page->platform_ = std::make_unique<PagePlatform>(
      v8::platform::NewDefaultPlatform(), std::move(main_thread));
  v8::V8::InitializePlatform(page->platform_.get());
  CHECK(v8::V8::Initialize());

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared =
      std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  // The heap of the Runtime's C++ objects, which V8 collects with its own, as
  // Blink's renderers have it. The isolate owns it.
  params.cpp_heap =
      v8::CppHeap::Create(page->platform_.get(), v8::CppHeapCreateParams({}))
          .release();
  ConfigurePageIsolate(params, options.memory_gib);
  page->isolate_ = v8::Isolate::New(params);
  // Microtasks run when the outermost script that the Runtime runs returns,
  // and after each task, as Blink has them.
  page->isolate_->SetMicrotasksPolicy(v8::MicrotasksPolicy::kScoped);
  InitializePageIsolate(page->isolate_);
  page->isolate_->Enter();
  page->data_ =
      bindings::PerIsolateData::Create(page->isolate_, std::move(features));
  return page;
}

PageV8::~PageV8() {
  data_.reset();
  v8::Isolate* const isolate = isolate_.ExtractAsDangling();
  isolate->Exit();
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
}

}  // namespace mitto
