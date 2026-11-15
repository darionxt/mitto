#include "mitto/dom/cached_storage_area.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/storage.h"
#include "mitto/dom/storage_backend.h"

namespace mitto {

namespace {

// How a value of localStorage is written, which Chrome's files keep: never
// change or reorder these.
constexpr char kUtf16Format = 0;
constexpr char kLatin1Format = 1;

// kLocalStorageDetectFormat: a byte that says which follows, then Latin-1 if
// every character fits, and otherwise UTF-16.
std::string ToDetectedFormat(const std::u16string& input) {
  if (std::ranges::all_of(input, [](char16_t c) { return c <= 0xFF; })) {
    std::string result(input.size() + 1, kLatin1Format);
    std::ranges::transform(input, result.begin() + 1,
                           [](char16_t c) { return static_cast<char>(c); });
    return result;
  }
  std::string result(1, kUtf16Format);
  result.append(base::as_string_view(base::as_byte_span(input)));
  return result;
}

std::optional<std::u16string> FromUtf16Bytes(std::string_view bytes) {
  if (bytes.size() % sizeof(char16_t) != 0) {
    return std::nullopt;
  }
  std::u16string result(bytes.size() / sizeof(char16_t), u'\0');
  base::as_writable_byte_span(result).copy_from(base::as_byte_span(bytes));
  return result;
}

std::optional<std::u16string> FromDetectedFormat(std::string_view bytes) {
  switch (bytes[0]) {
    case kUtf16Format:
      return FromUtf16Bytes(bytes.substr(1));
    case kLatin1Format: {
      std::u16string result(bytes.size() - 1, u'\0');
      std::ranges::transform(bytes.substr(1), result.begin(), [](char c) {
        return static_cast<char16_t>(static_cast<uint8_t>(c));
      });
      return result;
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

CachedStorageArea::CachedStorageArea(PageStorageKind kind,
                                     uint64_t document,
                                     StorageBackend& backend)
    : kind_(kind), backend_(backend) {
  // BindStorageArea(): the area and its observer, at once, so that a window
  // that has its Storage object hears of changes before it reads.
  observer_ = backend_->NewObserver();
  area_ = backend_->Bind(kind_, document, observer_, this);
}

CachedStorageArea::~CachedStorageArea() {
  backend_->Unbind(area_);
}

unsigned CachedStorageArea::GetLength() {
  EnsureLoaded();
  return map_->GetLength();
}

std::optional<std::u16string> CachedStorageArea::GetKey(unsigned index) {
  EnsureLoaded();
  return map_->GetKey(index);
}

std::optional<std::u16string> CachedStorageArea::GetItem(
    const std::u16string& key) {
  EnsureLoaded();
  return map_->GetItem(key);
}

bool CachedStorageArea::SetItem(const std::u16string& key,
                                const std::u16string& value,
                                Storage* source) {
  // A quick check that refuses what is plainly too large before loading.
  if ((key.size() + value.size()) * 2 > kPerStorageAreaQuota) {
    return false;
  }
  EnsureLoaded();
  std::optional<std::u16string> old_value;
  if (!map_->SetItem(key, value, &old_value)) {
    return false;
  }
  const PageStorageSource page_source = SourceOf(source);
  PageStorageMutation mutation;
  mutation.area = area_;
  mutation.source = page_source;
  mutation.key = KeyToBytes(key);
  mutation.value = ValueToBytes(value);
  backend_->Mutate(PageMessage::kStoragePut, std::move(mutation));
  if (!IsSessionStorage()) {
    EnqueuePendingMutation(key, value, std::move(old_value), page_source.token);
  } else if (old_value != value) {
    EnqueueStorageEvent(key, old_value, value, page_source.page_url,
                        page_source.token);
  }
  return true;
}

void CachedStorageArea::RemoveItem(const std::u16string& key, Storage* source) {
  EnsureLoaded();
  std::optional<std::u16string> old_value;
  if (!map_->RemoveItem(key, &old_value)) {
    return;
  }
  const PageStorageSource page_source = SourceOf(source);
  PageStorageMutation mutation;
  mutation.area = area_;
  mutation.source = page_source;
  mutation.key = KeyToBytes(key);
  backend_->Mutate(PageMessage::kStorageDelete, std::move(mutation));
  if (!IsSessionStorage()) {
    EnqueuePendingMutation(key, std::nullopt, std::move(old_value),
                           page_source.token);
  } else {
    EnqueueStorageEvent(key, old_value, std::nullopt, page_source.page_url,
                        page_source.token);
  }
}

void CachedStorageArea::Clear(Storage* source) {
  uint64_t new_observer = 0;
  bool already_empty = false;
  if (IsSessionStorage()) {
    EnsureLoaded();
    already_empty = map_->GetLength() == 0;
  } else if (!map_) {
    // The first use of the area: a new observer, whose first event is the
    // clear's own AllDeleted.
    DCHECK(pending_mutations_by_source_.empty());
    DCHECK(pending_mutations_by_key_.empty());
    new_observer = backend_->NewObserver();
    observer_ = new_observer;
  }
  map_ = std::make_unique<StorageAreaMap>(kPerStorageAreaQuota);

  const PageStorageSource page_source = SourceOf(source);
  PageStorageMutation mutation;
  mutation.area = area_;
  mutation.source = page_source;
  mutation.observer = new_observer;
  backend_->Mutate(PageMessage::kStorageDeleteAll, std::move(mutation));
  if (!IsSessionStorage()) {
    EnqueuePendingMutation(std::nullopt, std::nullopt, std::nullopt,
                           page_source.token);
  } else if (!already_empty) {
    EnqueueStorageEvent(std::nullopt, std::nullopt, std::nullopt,
                        page_source.page_url, page_source.token);
  }
}

void CachedStorageArea::RegisterSource(Storage* source) {
  sources_.emplace_back(source, base::Token::CreateRandom());
}

void CachedStorageArea::OnEvent(PageMessage type,
                                const PageStorageEvent& event) {
  if (event.observer != observer_) {
    return;
  }
  switch (type) {
    case PageMessage::kStorageKeyChanged:
      // sessionStorage's changes come with no echo, and the Session sends
      // none of them.
      if (!IsSessionStorage()) {
        KeyChanged(event);
      }
      return;
    case PageMessage::kStorageKeyChangeFailed:
      if (!IsSessionStorage()) {
        KeyChangeFailed(event);
      }
      return;
    case PageMessage::kStorageKeyDeleted:
      if (!IsSessionStorage()) {
        KeyDeleted(event);
      }
      return;
    case PageMessage::kStorageAllDeleted:
      // Even for sessionStorage, as the browser may delete it.
      AllDeleted(event);
      return;
    default:
      NOTREACHED();
  }
}

void CachedStorageArea::KeyChanged(const PageStorageEvent& event) {
  const std::u16string key = BytesToKey(event.key);
  const std::u16string new_value = BytesToValue(event.new_value);
  std::optional<std::u16string> old_value;
  if (event.old_value) {
    old_value = BytesToValue(*event.old_value);
  }
  std::unique_ptr<PendingMutation> local_mutation =
      PopPendingMutation(event.source.token);
  if (map_ && !local_mutation) {
    MaybeApplyNonLocalMutationForKey(key, new_value);
  }
  // A mutation popped is for the key that the event names.
  DCHECK(!local_mutation || local_mutation->key == key);
  EnqueueStorageEvent(key, old_value, new_value, event.source.page_url,
                      event.source.token);
}

void CachedStorageArea::KeyChangeFailed(const PageStorageEvent& event) {
  const std::u16string key = BytesToKey(event.key);
  std::unique_ptr<PendingMutation> local_mutation =
      PopPendingMutation(event.source.token);
  // Another window's failed change does not matter here.
  if (!local_mutation) {
    return;
  }
  DCHECK(local_mutation->key == key);
  // The Session refused a change that this Page made.
  std::optional<std::u16string> old_value = local_mutation->old_value;
  const auto key_queue = pending_mutations_by_key_.find(key);
  if (key_queue == pending_mutations_by_key_.end()) {
    // The only one pending for the key: the key goes back to what it was, or
    // to what came from elsewhere meanwhile.
    DCHECK(map_);
    std::optional<std::u16string> invalid_cached_value = map_->GetItem(key);
    if (!old_value) {
      map_->RemoveItem(key, nullptr);
    } else {
      map_->SetItemIgnoringQuota(key, *old_value);
    }
    EnqueueStorageEvent(key, invalid_cached_value, old_value,
                        event.source.page_url, event.source.token);
    return;
  }
  // Something has already overwritten it, so the next pending change takes
  // the value to go back to, should it fail too.
  key_queue->second.front()->old_value = std::move(old_value);
}

void CachedStorageArea::KeyDeleted(const PageStorageEvent& event) {
  const std::u16string key = BytesToKey(event.key);
  std::unique_ptr<PendingMutation> local_mutation =
      PopPendingMutation(event.source.token);
  if (map_ && !local_mutation) {
    MaybeApplyNonLocalMutationForKey(key, std::nullopt);
  }
  DCHECK(!local_mutation || local_mutation->key == key);
  if (event.old_value) {
    EnqueueStorageEvent(key, BytesToValue(*event.old_value), std::nullopt,
                        event.source.page_url, event.source.token);
  }
}

void CachedStorageArea::AllDeleted(const PageStorageEvent& event) {
  std::unique_ptr<PendingMutation> local_mutation =
      PopPendingMutation(event.source.token);
  // The Page's own clear() emptied its map already, and only now does it know
  // whether the area held anything.
  if (!local_mutation) {
    map_ = std::make_unique<StorageAreaMap>(kPerStorageAreaQuota);
    // The latest change of each key that is still pending came after the
    // clear, so it comes back now rather than with its echo.
    for (auto& [key, mutations] : pending_mutations_by_key_) {
      DCHECK(!mutations.empty());
      PendingMutation* const most_recent = mutations.back();
      if (most_recent->new_value) {
        map_->SetItemIgnoringQuota(key, *most_recent->new_value);
      }
      // If the first fails, the key is empty in the Session.
      mutations.front()->old_value = std::nullopt;
    }
  }
  DCHECK(!local_mutation || !local_mutation->key);
  if (event.was_nonempty) {
    EnqueueStorageEvent(std::nullopt, std::nullopt, std::nullopt,
                        event.source.page_url, event.source.token);
  }
}

void CachedStorageArea::EnqueuePendingMutation(
    std::optional<std::u16string> key,
    std::optional<std::u16string> new_value,
    std::optional<std::u16string> old_value,
    const base::Token& source_id) {
  CHECK(!source_id.is_zero());
  auto mutation = std::make_unique<PendingMutation>();
  mutation->key = std::move(key);
  mutation->new_value = std::move(new_value);
  mutation->old_value = std::move(old_value);
  // A clear() has no key.
  if (mutation->key) {
    pending_mutations_by_key_[*mutation->key].push_back(mutation.get());
  }
  pending_mutations_by_source_[source_id].push_back(std::move(mutation));
}

std::unique_ptr<CachedStorageArea::PendingMutation>
CachedStorageArea::PopPendingMutation(const base::Token& source_id) {
  const auto source_queue = pending_mutations_by_source_.find(source_id);
  if (source_queue == pending_mutations_by_source_.end()) {
    return nullptr;
  }
  DCHECK(!source_queue->second.empty());
  std::unique_ptr<PendingMutation> mutation =
      std::move(source_queue->second.front());
  source_queue->second.pop_front();
  if (source_queue->second.empty()) {
    pending_mutations_by_source_.erase(source_queue);
  }
  // The oldest pending change of its key is this one.
  if (mutation->key) {
    const auto key_queue = pending_mutations_by_key_.find(*mutation->key);
    CHECK(key_queue != pending_mutations_by_key_.end());
    DCHECK_EQ(key_queue->second.front(), mutation.get());
    key_queue->second.pop_front();
    if (key_queue->second.empty()) {
      pending_mutations_by_key_.erase(key_queue);
    }
  }
  return mutation;
}

void CachedStorageArea::MaybeApplyNonLocalMutationForKey(
    const std::u16string& key,
    const std::optional<std::u16string>& new_value) {
  DCHECK(map_);
  const auto key_queue = pending_mutations_by_key_.find(key);
  if (key_queue == pending_mutations_by_key_.end()) {
    if (!new_value) {
      map_->RemoveItem(key, nullptr);
    } else {
      // The Session allows more than the quota, for races between windows.
      map_->SetItemIgnoringQuota(key, *new_value);
    }
    return;
  }
  // A change of this Page's waits for its echo, so this one waits as the value
  // to go back to, should that change fail.
  key_queue->second.front()->old_value = new_value;
}

void CachedStorageArea::EnsureLoaded() {
  if (map_) {
    return;
  }
  // A new observer, whose events start right after the snapshot.
  observer_ = backend_->NewObserver();
  const std::vector<std::pair<std::string, std::string>> data =
      backend_->GetAll(area_, observer_);
  map_ = std::make_unique<StorageAreaMap>(kPerStorageAreaQuota);
  for (const auto& [key, value] : data) {
    map_->SetItemIgnoringQuota(BytesToKey(key), BytesToValue(value));
  }
}

void CachedStorageArea::EnqueueStorageEvent(
    const std::optional<std::u16string>& key,
    const std::optional<std::u16string>& old_value,
    const std::optional<std::u16string>& new_value,
    const std::string& url,
    const base::Token& source_id) {
  // A change of a key's value to the same value changed nothing.
  if (key && new_value == old_value) {
    return;
  }
  std::erase_if(sources_, [&](const auto& source) {
    Storage* const area = source.first.Get();
    if (!area) {
      return true;
    }
    if (source.second == source_id) {
      return false;
    }
    return !area->EnqueueStorageEvent(key, old_value, new_value, url);
  });
}

PageStorageSource CachedStorageArea::SourceOf(Storage* source) const {
  PageStorageSource page_source;
  const auto found = std::ranges::find_if(
      sources_, [source](const auto& s) { return s.first.Get() == source; });
  CHECK(found != sources_.end());
  page_source.token = found->second;
  page_source.page_url = source->PageUrl();
  return page_source;
}

std::string CachedStorageArea::KeyToBytes(const std::u16string& key) const {
  if (!IsSessionStorage()) {
    return ToDetectedFormat(key);
  }
  // kSessionStorageForceUTF8, with each lone surrogate as U+FFFD.
  return base::UTF16ToUTF8(key);
}

std::string CachedStorageArea::ValueToBytes(const std::u16string& value) const {
  if (!IsSessionStorage()) {
    return ToDetectedFormat(value);
  }
  return std::string(base::as_string_view(base::as_byte_span(value)));
}

std::u16string CachedStorageArea::BytesToKey(const std::string& bytes) const {
  if (bytes.empty()) {
    return std::u16string();
  }
  std::optional<std::u16string> result;
  if (!IsSessionStorage()) {
    result = FromDetectedFormat(bytes);
  } else {
    std::u16string utf16;
    if (base::UTF8ToUTF16(bytes.data(), bytes.size(), &utf16)) {
      result = std::move(utf16);
    }
  }
  if (!result) {
    LOG(ERROR) << "Corrupt data in domstorage";
    return std::u16string();
  }
  return *std::move(result);
}

std::u16string CachedStorageArea::BytesToValue(const std::string& bytes) const {
  if (bytes.empty()) {
    return std::u16string();
  }
  std::optional<std::u16string> result =
      IsSessionStorage() ? FromUtf16Bytes(bytes) : FromDetectedFormat(bytes);
  if (!result) {
    LOG(ERROR) << "Corrupt data in domstorage";
    return std::u16string();
  }
  return *std::move(result);
}

}  // namespace mitto
