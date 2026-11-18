// The HTML Standard's WorkerNavigator, a worker's `navigator`: what its
// creator's navigator says of the browser and the machine, from the Session's
// Persona and Machine, as Blink's NavigatorBase gives a worker the values of
// its creator's process; its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/workers/worker_navigator.h
// Upstream: third_party/blink/renderer/core/execution_context/navigator_base.cc

#ifndef MITTO_DOM_WORKER_NAVIGATOR_H_
#define MITTO_DOM_WORKER_NAVIGATOR_H_

#include <stdint.h>

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class Navigator;
class NavigatorUAData;
class NetworkInformation;
class Permissions;
class StorageManager;

class WorkerNavigator final : public bindings::ScriptWrappable {
 public:
  // What `start` says of the browser and the machine, with `connection` for
  // the worker.
  static WorkerNavigator* Create(const PageStart& start,
                                 const PageConnection& connection);

  WorkerNavigator(const PageStart& start, const PageConnection& connection);
  ~WorkerNavigator() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface, which give what a window's navigator does.
  uint64_t HardwareConcurrency() const;
  std::u16string AppCodeName() const;
  std::u16string AppName() const;
  std::u16string AppVersion() const;
  std::u16string Platform() const;
  std::u16string Product() const;
  std::u16string UserAgent() const;
  std::u16string Language() const;
  v8::Local<v8::Value> Languages();
  bool OnLine() const;
  NetworkInformation* Connection();
  float DeviceMemory() const;
  NavigatorUAData* UserAgentData() const;
  mitto::Permissions* Permissions();
  StorageManager* Storage();

 private:
  // A window's navigator of the same values, which no page reaches, as Blink
  // shares NavigatorBase between both.
  const cppgc::Member<Navigator> navigator_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WORKER_NAVIGATOR_H_
