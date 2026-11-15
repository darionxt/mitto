#include "mitto/dom/storage.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_storage.h"
#include "mitto/dom/cached_storage_area.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/storage_event.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

namespace {

void ThrowAccessDenied(bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException("SecurityError",
                                    Storage::kAccessDeniedMessage);
}

}  // namespace

// static
Storage* Storage::Create(Window& window,
                         scoped_refptr<CachedStorageArea> cached_area) {
  return bindings::MakeGarbageCollected<Storage>(window,
                                                 std::move(cached_area));
}

Storage::Storage(Window& window, scoped_refptr<CachedStorageArea> cached_area)
    : window_(&window), cached_area_(std::move(cached_area)) {
  cached_area_->RegisterSource(this);
}

Storage::~Storage() = default;

const bindings::WrapperTypeInfo* Storage::GetWrapperTypeInfo() const {
  return &bindings::V8Storage::kWrapperTypeInfo;
}

void Storage::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

uint32_t Storage::Length(bindings::ExceptionState& exception_state) const {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return 0;
  }
  return cached_area_->GetLength();
}

std::optional<std::u16string> Storage::Key(
    uint32_t index,
    bindings::ExceptionState& exception_state) const {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return std::nullopt;
  }
  return cached_area_->GetKey(index);
}

std::optional<std::u16string> Storage::GetItem(
    const std::u16string& key,
    bindings::ExceptionState& exception_state) const {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return std::nullopt;
  }
  return cached_area_->GetItem(key);
}

void Storage::SetItem(const std::u16string& key,
                      const std::u16string& value,
                      bindings::ExceptionState& exception_state) {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return;
  }
  if (!cached_area_->SetItem(key, value, this)) {
    QuotaExceededError::Throw(
        exception_state,
        base::StrCat({"Setting the value of '", base::UTF16ToUTF8(key),
                      "' exceeded the quota."}));
  }
}

bool Storage::RemoveItem(const std::u16string& key,
                         bindings::ExceptionState& exception_state) {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return false;
  }
  cached_area_->RemoveItem(key, this);
  return true;
}

void Storage::Clear(bindings::ExceptionState& exception_state) {
  if (!CanAccessStorage()) {
    ThrowAccessDenied(exception_state);
    return;
  }
  cached_area_->Clear(this);
}

std::vector<std::u16string> Storage::NamedPropertyEnumerator() const {
  std::vector<std::u16string> names;
  if (!CanAccessStorage()) {
    return names;
  }
  const unsigned length = cached_area_->GetLength();
  names.reserve(length);
  for (unsigned i = 0; i < length; ++i) {
    names.push_back(*cached_area_->GetKey(i));
  }
  return names;
}

bool Storage::NamedPropertyQuery(const std::u16string& name) const {
  if (name == u"length" || !CanAccessStorage()) {
    return false;
  }
  return cached_area_->GetItem(name).has_value();
}

std::string Storage::PageUrl() const {
  Window* const window = window_.Get();
  if (!window) {
    return std::string();
  }
  return base::UTF16ToUTF8(window->GetDocument().UrlForBinding());
}

bool Storage::EnqueueStorageEvent(
    const std::optional<std::u16string>& key,
    const std::optional<std::u16string>& old_value,
    const std::optional<std::u16string>& new_value,
    const std::string& url) {
  Window* const window = window_.Get();
  WindowTasks* const tasks = window ? window->GetTasks() : nullptr;
  if (!tasks) {
    return false;
  }
  // LocalDOMWindow::EnqueueWindowEvent(), on the DOM manipulation task source.
  StorageEvent* const event = StorageEvent::CreateForChange(
      key, old_value, new_value, base::UTF8ToUTF16(url), this);
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::WeakPersistent<Window>& window,
                         const cppgc::Persistent<StorageEvent>& event) {
                        if (window) {
                          window->DispatchEvent(event.Get());
                        }
                      },
                      cppgc::WeakPersistent<Window>(window),
                      cppgc::Persistent<StorageEvent>(event)));
  return true;
}

bool Storage::CanAccessStorage() const {
  // StorageController::CanAccessStorageArea(): no content setting blocks a
  // Page's storage, so only a window that has gone loses it.
  return window_.Get() != nullptr;
}

}  // namespace mitto
