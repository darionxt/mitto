#include "mitto/dom/worker_navigator.h"

#include "mitto/bindings/v8_worker_navigator.h"
#include "mitto/dom/navigator.h"

namespace mitto {

// static
WorkerNavigator* WorkerNavigator::Create(const PageStart& start,
                                         const PageConnection& connection) {
  return bindings::MakeGarbageCollected<WorkerNavigator>(start, connection);
}

WorkerNavigator::WorkerNavigator(const PageStart& start,
                                 const PageConnection& connection)
    : navigator_(Navigator::Create(start, connection)) {}

WorkerNavigator::~WorkerNavigator() = default;

const bindings::WrapperTypeInfo* WorkerNavigator::GetWrapperTypeInfo() const {
  return &bindings::V8WorkerNavigator::kWrapperTypeInfo;
}

void WorkerNavigator::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(navigator_);
}

uint64_t WorkerNavigator::HardwareConcurrency() const {
  return navigator_->HardwareConcurrency();
}

std::u16string WorkerNavigator::AppCodeName() const {
  return navigator_->AppCodeName();
}

std::u16string WorkerNavigator::AppName() const {
  return navigator_->AppName();
}

std::u16string WorkerNavigator::AppVersion() const {
  return navigator_->AppVersion();
}

std::u16string WorkerNavigator::Platform() const {
  return navigator_->Platform();
}

std::u16string WorkerNavigator::Product() const {
  return navigator_->Product();
}

std::u16string WorkerNavigator::UserAgent() const {
  return navigator_->UserAgent();
}

std::u16string WorkerNavigator::Language() const {
  return navigator_->Language();
}

v8::Local<v8::Value> WorkerNavigator::Languages() {
  return navigator_->Languages();
}

bool WorkerNavigator::OnLine() const {
  return navigator_->OnLine();
}

NetworkInformation* WorkerNavigator::Connection() {
  return navigator_->Connection();
}

float WorkerNavigator::DeviceMemory() const {
  return navigator_->DeviceMemory();
}

NavigatorUAData* WorkerNavigator::UserAgentData() const {
  return navigator_->UserAgentData();
}

}  // namespace mitto
