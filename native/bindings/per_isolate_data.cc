#include "mitto/bindings/per_isolate_data.h"

#include <stdint.h>

#include <utility>

#include "base/check.h"
#include "base/memory/ptr_util.h"
#include "v8/include/v8-isolate.h"

namespace mitto::bindings {

namespace {

// The isolate's embedder data slot that holds the PerIsolateData.
constexpr uint32_t kDataSlot = 0;

}  // namespace

// static
std::unique_ptr<PerIsolateData> PerIsolateData::Create(
    v8::Isolate* isolate,
    RuntimeFeatureSet features) {
  CHECK(!isolate->GetData(kDataSlot));
  auto data = base::WrapUnique(new PerIsolateData(isolate, std::move(features)));
  isolate->SetData(kDataSlot, data.get());
  return data;
}

// static
PerIsolateData* PerIsolateData::From(v8::Isolate* isolate) {
  auto* const data = static_cast<PerIsolateData*>(isolate->GetData(kDataSlot));
  CHECK(data);
  return data;
}

PerIsolateData::PerIsolateData(v8::Isolate* isolate, RuntimeFeatureSet features)
    : isolate_(isolate),
      features_(std::move(features)),
      time_origin_(base::TimeTicks::Now()) {}

PerIsolateData::~PerIsolateData() {
  isolate_->SetData(kDataSlot, nullptr);
}

v8::Local<v8::FunctionTemplate> PerIsolateData::InterfaceTemplate(
    v8::Isolate* isolate,
    const WrapperTypeInfo* info) {
  const auto found = templates_.find(info);
  if (found != templates_.end()) {
    return found->second.Get(isolate);
  }
  const v8::Local<v8::FunctionTemplate> interface_template =
      v8::FunctionTemplate::New(isolate);
  // Kept before the members go in, so that an interface whose members name it,
  // such as a method that takes one of its own objects, finds it.
  templates_.emplace(info, v8::Eternal<v8::FunctionTemplate>(
                               isolate, interface_template));
  info->install_interface_template(isolate, interface_template);
  return interface_template;
}

bool PerIsolateData::HasInstance(v8::Isolate* isolate,
                                 const WrapperTypeInfo& info,
                                 v8::Local<v8::Value> value) const {
  const auto found = templates_.find(&info);
  return found != templates_.end() &&
         found->second.Get(isolate)->HasInstance(value);
}

}  // namespace mitto::bindings
