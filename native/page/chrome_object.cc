#include "mitto/page/chrome_object.h"

#include <stdint.h>

#include <initializer_list>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/performance.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// The navigation of a Page is the Session's, as a typed URL is: Blink's
// kWebNavigationTypeOther, which loadTimes() calls "Other" and csi() gives as
// the transition 15.
constexpr std::string_view kNavigationType = "Other";
constexpr int kTransitionOther = 15;

// What the extensions system answers for a page that no installed app covers,
// as extension_misc names the states.
// Upstream: chrome/common/extensions/extension_constants.h
constexpr std::string_view kRunningState = "cannot_run";
constexpr std::string_view kInstallState = "not_installed";

v8::Local<v8::String> String(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

v8::Local<v8::String> Name(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kInternalized,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

v8::Local<v8::External> External(v8::Isolate* isolate, ChromeObjectData& data) {
  return v8::External::New(isolate, &data, v8::kExternalPointerTypeTagDefault);
}

ChromeObjectData& DataOf(v8::Local<v8::Value> external) {
  return *static_cast<ChromeObjectData*>(
      external.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
}

// A native data property's getter, which gives what it was made with. Its
// empty setter keeps it so when a page assigns to it.
void NativeDataGetter(v8::Local<v8::Name> name,
                      const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(info.Data());
}
void EmptySetter(v8::Local<v8::Name> name,
                 v8::Local<v8::Value> value,
                 const v8::PropertyCallbackInfo<v8::Boolean>& info) {}

// An object whose properties are native data properties, as loadTimes() and
// csi() make them, or null if one of them cannot be defined.
class NativeDataObject {
 public:
  NativeDataObject(v8::Isolate* isolate, v8::Local<v8::Context> context)
      : isolate_(isolate),
        context_(context),
        object_(v8::Object::New(isolate)) {}

  void Define(std::string_view name, v8::Local<v8::Value> value) {
    defined_ = defined_ && object_
                               ->SetNativeDataProperty(
                                   context_, Name(isolate_, name),
                                   &NativeDataGetter, &EmptySetter, value)
                               .FromMaybe(false);
  }
  void Number(std::string_view name, double value) {
    Define(name, v8::Number::New(isolate_, value));
  }
  void Boolean(std::string_view name, bool value) {
    Define(name, v8::Boolean::New(isolate_, value));
  }
  void Text(std::string_view name, std::string_view value) {
    Define(name, String(isolate_, value));
  }

  v8::Local<v8::Value> Result() const {
    if (!defined_) {
      return v8::Null(isolate_);
    }
    return object_;
  }

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const v8::Local<v8::Context> context_;
  const v8::Local<v8::Object> object_;
  bool defined_ = true;
};

double InSeconds(int64_t milliseconds) {
  return base::Milliseconds(milliseconds).InSecondsF();
}

// LoadTimesBindings::GetLoadTimes().
void LoadTimes(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const ChromeObjectData& data = DataOf(info.Data());
  // Request time and start load time are the navigation's start, for
  // compatibility; commit is the response's start.
  const double navigation_start =
      InSeconds(data.ToIntegerMilliseconds(data.navigation_start));
  NativeDataObject load_times(isolate, isolate->GetCurrentContext());
  load_times.Number("requestTime", navigation_start);
  load_times.Number("startLoadTime", navigation_start);
  load_times.Number("commitLoadTime",
                    InSeconds(data.ToIntegerMilliseconds(data.response_start)));
  load_times.Number(
      "finishDocumentLoadTime",
      InSeconds(data.ToIntegerMilliseconds(data.dom_content_loaded_end)));
  load_times.Number("finishLoadTime",
                    InSeconds(data.ToIntegerMilliseconds(data.load_end)));
  // A Page paints nothing, so its first paint has not come.
  load_times.Number("firstPaintTime", 0);
  load_times.Number("firstPaintAfterLoadTime", 0);
  load_times.Text("navigationType", kNavigationType);
  load_times.Boolean("wasFetchedViaSpdy", data.was_fetched_via_spdy);
  load_times.Boolean("wasNpnNegotiated", data.was_alpn_negotiated);
  load_times.Text("npnNegotiatedProtocol", data.alpn_negotiated_protocol);
  // The network service never sets it.
  load_times.Boolean("wasAlternateProtocolAvailable", false);
  load_times.Text("connectionInfo", data.connection_info);
  info.GetReturnValue().Set(load_times.Result());
}

// LoadTimesBindings::GetCSI().
void Csi(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const ChromeObjectData& data = DataOf(info.Data());
  const base::Time now = base::Time::Now();
  const base::Time start = base::Time::FromSecondsSinceUnixEpoch(
      InSeconds(data.ToIntegerMilliseconds(data.navigation_start)));
  const base::Time dom_content_loaded_end =
      base::Time::FromSecondsSinceUnixEpoch(
          InSeconds(data.ToIntegerMilliseconds(data.dom_content_loaded_end)));
  NativeDataObject csi(isolate, isolate->GetCurrentContext());
  csi.Number("startE",
             static_cast<double>(start.InMillisecondsSinceUnixEpoch()));
  // The time the document finished parsing, under the name of onload.
  csi.Number("onloadT",
             static_cast<double>(
                 dom_content_loaded_end.InMillisecondsSinceUnixEpoch()));
  csi.Number("pageT", (now - start).InMillisecondsF());
  csi.Number("tran", kTransitionOther);
  info.GetReturnValue().Set(csi.Result());
}

// The error of an app method called with arguments that its signature does
// not take: APIBinding::HandleCall()'s, with the empty error of
// AppHooksDelegate's invalid invocation.
void ThrowInvocationError(v8::Isolate* isolate,
                          std::string_view method,
                          std::string_view signature) {
  isolate->ThrowException(v8::Exception::TypeError(
      String(isolate, base::StrCat({"Error in invocation of app.", method, "(",
                                    signature, "): "}))));
}

// The app methods that take nothing: any argument fails their signature.
template <void (*kAnswer)(const v8::FunctionCallbackInfo<v8::Value>&)>
void NoArguments(const v8::FunctionCallbackInfo<v8::Value>& info,
                 std::string_view method) {
  if (info.Length() > 0) {
    ThrowInvocationError(info.GetIsolate(), method, "");
    return;
  }
  kAnswer(info);
}

void AnswerFalse(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(false);
}
void AnswerNull(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().SetNull();
}
void AnswerRunningState(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(Name(info.GetIsolate(), kRunningState));
}

void GetDetails(const v8::FunctionCallbackInfo<v8::Value>& info) {
  NoArguments<&AnswerNull>(info, "getDetails");
}
void GetIsInstalled(const v8::FunctionCallbackInfo<v8::Value>& info) {
  NoArguments<&AnswerFalse>(info, "getIsInstalled");
}
void RunningState(const v8::FunctionCallbackInfo<v8::Value>& info) {
  NoArguments<&AnswerRunningState>(info, "runningState");
}

// Calls `callback` with the install state, as the browser's answer to
// GetAppInstallState() completes the request, with the global object as this.
void CompleteInstallState(ChromeObjectData* data,
                          v8::Isolate* isolate,
                          v8::Global<v8::Context> context_handle,
                          v8::Global<v8::Function> callback) {
  if (data->detached) {
    return;
  }
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = context_handle.Get(isolate);
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> state = Name(isolate, kInstallState);
  std::ignore =
      callback.Get(isolate)->Call(context, context->Global(), 1, &state);
  if (try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception());
  }
}

// installState(optional function callback): the callback may be left out,
// or be null or undefined, and nothing else is taken.
void InstallState(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const int length = info.Length();
  const bool takes =
      length == 0 ||
      (length == 1 && (info[0]->IsFunction() || info[0]->IsNullOrUndefined()));
  if (!takes) {
    ThrowInvocationError(isolate, "installState", "optional function callback");
    return;
  }
  if (length == 0 || !info[0]->IsFunction()) {
    return;
  }
  ChromeObjectData& data = DataOf(info.Data());
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &CompleteInstallState, base::Unretained(&data), isolate,
          v8::Global<v8::Context>(isolate, isolate->GetCurrentContext()),
          v8::Global<v8::Function>(isolate, info[0].As<v8::Function>())));
}

// Sets a function on `object`, as v8::Function::New() makes them: one of
// LoadTimesBindings', with no name and a prototype, or, `named`, one of an
// extension API's methods, which has a name and is not a constructor.
void SetFunction(v8::Local<v8::Context> context,
                 v8::Local<v8::Object> object,
                 std::string_view key,
                 v8::FunctionCallback callback,
                 ChromeObjectData& data,
                 bool named) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Function> function;
  if (!v8::Function::New(context, callback, External(isolate, data), 0,
                         named ? v8::ConstructorBehavior::kThrow
                               : v8::ConstructorBehavior::kAllow)
           .ToLocal(&function)) {
    return;
  }
  if (named) {
    function->SetName(Name(isolate, key));
  }
  object->Set(context, Name(isolate, key), function).Check();
}

// An enumeration of the API's schema, as the extensions system makes one:
// each value under its name in upper case, in the order of the names.
v8::Local<v8::Object> Enumeration(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        entries) {
  const v8::Local<v8::Object> object = v8::Object::New(isolate);
  for (const auto& [name, value] : entries) {
    object->Set(context, Name(isolate, name), Name(isolate, value)).Check();
  }
  return object;
}

// chrome.app, as the extensions system makes it for a web page: its
// isInstalled from AppHooksDelegate's template, its methods and its two
// enumerations, in the order of their names.
v8::Local<v8::Object> AppObject(v8::Local<v8::Context> context,
                                ChromeObjectData& data) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Object> app = v8::Object::New(isolate);
  app->SetNativeDataProperty(context, Name(isolate, "isInstalled"),
                             &NativeDataGetter, &EmptySetter,
                             v8::False(isolate))
      .Check();
  SetFunction(context, app, "getDetails", &GetDetails, data, /*named=*/true);
  SetFunction(context, app, "getIsInstalled", &GetIsInstalled, data,
              /*named=*/true);
  SetFunction(context, app, "installState", &InstallState, data,
              /*named=*/true);
  SetFunction(context, app, "runningState", &RunningState, data,
              /*named=*/true);
  app->Set(context, Name(isolate, "InstallState"),
           Enumeration(isolate, context,
                       {{"DISABLED", "disabled"},
                        {"INSTALLED", "installed"},
                        {"NOT_INSTALLED", "not_installed"}}))
      .Check();
  app->Set(context, Name(isolate, "RunningState"),
           Enumeration(isolate, context,
                       {{"CANNOT_RUN", "cannot_run"},
                        {"READY_TO_RUN", "ready_to_run"},
                        {"RUNNING", "running"}}))
      .Check();
  return app;
}

}  // namespace

ChromeObjectData::ChromeObjectData() = default;
ChromeObjectData::~ChromeObjectData() = default;

void ChromeObjectData::StartAt(base::TimeTicks start) {
  // Blink's GetUnixAtZeroMonotonic(), as Performance takes it.
  unix_at_zero_monotonic =
      base::Time::Now() - (base::TimeTicks::Now() - base::TimeTicks());
  navigation_start = start;
}

int64_t ChromeObjectData::ToIntegerMilliseconds(base::TimeTicks time) const {
  // PerformanceTiming::MonotonicTimeToIntegerMilliseconds(): coarsened as
  // performance.timeOrigin is, with the same clamper, and then cut to whole
  // milliseconds, so that requestTime agrees with timeOrigin.
  // Upstream: third_party/blink/renderer/core/timing/performance_timing.cc
  if (time.is_null()) {
    return 0;
  }
  const base::Time wall = unix_at_zero_monotonic + (time - base::TimeTicks());
  return static_cast<int64_t>(
      Performance::ClampTimeResolution(wall - base::Time::UnixEpoch()));
}

void InstallChromeObject(v8::Local<v8::Context> context,
                         ChromeObjectData& data) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  // GetOrCreateChromeObject(): a plain object, set on the global object as a
  // page would set it.
  const v8::Local<v8::Object> chrome = v8::Object::New(isolate);
  context->Global()->Set(context, Name(isolate, "chrome"), chrome).Check();
  // ChromeRenderFrameObserver::DidCreateScriptContext(), then the extensions
  // system's.
  SetFunction(context, chrome, "loadTimes", &LoadTimes, data,
              /*named=*/false);
  SetFunction(context, chrome, "csi", &Csi, data, /*named=*/false);
  chrome->Set(context, Name(isolate, "app"), AppObject(context, data)).Check();
}

}  // namespace mitto
