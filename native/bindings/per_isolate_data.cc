#include "mitto/bindings/per_isolate_data.h"

#include <stdint.h>

#include <utility>

#include "base/check.h"
#include "base/memory/ptr_util.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/interface_table.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-external.h"
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
  auto data =
      base::WrapUnique(new PerIsolateData(isolate, std::move(features)));
  isolate->SetData(kDataSlot, data.get());
  SetUpExceptionContext(isolate);
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
  CHECK(info->kind != WrapperTypeInfo::Kind::kNamespace);
  const auto found = templates_.find(info);
  if (found != templates_.end()) {
    return found->second.Get(isolate).As<v8::FunctionTemplate>();
  }
  const v8::Local<v8::FunctionTemplate> interface_template =
      v8::FunctionTemplate::New(isolate);
  // Kept before the members go in, so that an interface whose members name it,
  // such as a method that takes one of its own objects, finds it.
  templates_.emplace(info,
                     v8::Eternal<v8::Template>(isolate, interface_template));
  info->install_interface_template(isolate, interface_template, *info);
  return interface_template;
}

v8::Local<v8::ObjectTemplate> PerIsolateData::NamespaceTemplate(
    v8::Isolate* isolate,
    const WrapperTypeInfo* info) {
  CHECK(info->kind == WrapperTypeInfo::Kind::kNamespace);
  const auto found = templates_.find(info);
  if (found != templates_.end()) {
    return found->second.Get(isolate).As<v8::ObjectTemplate>();
  }
  const v8::Local<v8::ObjectTemplate> namespace_template =
      v8::ObjectTemplate::New(isolate);
  templates_.emplace(info,
                     v8::Eternal<v8::Template>(isolate, namespace_template));
  info->install_interface_template(isolate, namespace_template, *info);
  return namespace_template;
}

v8::Local<v8::FunctionTemplate> PerIsolateData::FactoryFunctionTemplate(
    v8::Isolate* isolate,
    const ExposedConstructInfo& construct) {
  const auto found = factory_function_templates_.find(&construct);
  if (found != factory_function_templates_.end()) {
    return found->second.Get(isolate);
  }
  const v8::Local<v8::FunctionTemplate> function_template =
      v8::FunctionTemplate::New(
          isolate, &KnownGapFactoryFunction,
          v8::External::New(isolate,
                            const_cast<ExposedConstructInfo*>(&construct),
                            kExposedConstructInfoTag),
          v8::Local<v8::Signature>(), construct.length,
          v8::ConstructorBehavior::kAllow, v8::SideEffectType::kHasSideEffect);
  function_template->Inherit(InterfaceTemplate(isolate, construct.target));
  function_template->SetClassName(ToV8StringFromUtf8(isolate, construct.name));
  function_template->SetExceptionContext(v8::ExceptionContext::kConstructor);
  factory_function_templates_.emplace(
      &construct,
      v8::Eternal<v8::FunctionTemplate>(isolate, function_template));
  return function_template;
}

bool PerIsolateData::FirstReachOfKnownGap(uint16_t number) {
  if (reported_known_gaps_.size() <= number) {
    reported_known_gaps_.resize(number + 1u);
  }
  if (reported_known_gaps_[number]) {
    return false;
  }
  reported_known_gaps_[number] = true;
  return true;
}

void PerIsolateData::ReportKnownGap(uint16_t number, std::string_view name) {
  if (FirstReachOfKnownGap(number) && known_gap_reporter_) {
    known_gap_reporter_.Run(name);
  }
}

v8::Local<v8::Private> PerIsolateData::KnownGapPrivate(v8::Isolate* isolate,
                                                       uint16_t number) {
  if (known_gap_privates_.size() <= number) {
    known_gap_privates_.resize(number + 1u);
  }
  v8::Eternal<v8::Private>& eternal = known_gap_privates_[number];
  if (eternal.IsEmpty()) {
    eternal.Set(isolate, v8::Private::New(isolate));
  }
  return eternal.Get(isolate);
}

void PerIsolateData::PushDictionaryContext(std::string_view dictionary_name) {
  dictionary_contexts_.push_back({std::string(dictionary_name), std::string()});
}

void PerIsolateData::SetDictionaryPropertyName(std::string_view property_name) {
  CHECK(!dictionary_contexts_.empty());
  dictionary_contexts_.back().property_name = property_name;
}

void PerIsolateData::PopDictionaryContext() {
  CHECK(!dictionary_contexts_.empty());
  dictionary_contexts_.pop_back();
}

std::vector<PerIsolateData::DictionaryContext>
PerIsolateData::dictionary_contexts_innermost_first() const {
  return {dictionary_contexts_.rbegin(), dictionary_contexts_.rend()};
}

bool PerIsolateData::HasInstance(v8::Isolate* isolate,
                                 const WrapperTypeInfo& info,
                                 v8::Local<v8::Value> value) const {
  const auto found = templates_.find(&info);
  return found != templates_.end() &&
         info.kind != WrapperTypeInfo::Kind::kNamespace &&
         found->second.Get(isolate).As<v8::FunctionTemplate>()->HasInstance(
             value);
}

}  // namespace mitto::bindings
