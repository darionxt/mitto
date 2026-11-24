#include "mitto/page/session_history.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/strings/utf_string_conversions.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

// ToNavigationApiHistoryEntry(), for a document of `document_sequence_number`,
// which sees no URL of another document's entry that protects it.
Navigation::ApiEntry ToApiEntry(const HistoryItem& item,
                                int64_t document_sequence_number) {
  Navigation::ApiEntry entry;
  entry.key = item.navigation_api_key;
  entry.id = item.navigation_api_id;
  if (!item.protect_url_in_navigation_api ||
      item.document_sequence_number == document_sequence_number) {
    entry.url = base::UTF8ToUTF16(item.url.spec());
  }
  entry.document_sequence = item.document_sequence_number;
  entry.state = item.navigation_api_state;
  return entry;
}

}  // namespace

FrameNavigationEntry::FrameNavigationEntry(std::string unique_name,
                                           HistoryItem item,
                                           uint64_t record,
                                           bool same_document_entry,
                                           PageReferrerPolicy referrer_policy)
    : unique_name_(std::move(unique_name)),
      item_(std::move(item)),
      record_(record),
      same_document_entry_(same_document_entry),
      referrer_policy_(referrer_policy) {}

FrameNavigationEntry::~FrameNavigationEntry() = default;

SessionHistory::Entry::Entry() = default;
SessionHistory::Entry::Entry(const Entry&) = default;
SessionHistory::Entry& SessionHistory::Entry::operator=(const Entry&) = default;
SessionHistory::Entry::~Entry() = default;

SessionHistory::SessionHistory() {
  // CreateInitialEntry(): the tab's about:blank, which counts as one entry.
  Entry initial;
  initial.root = base::MakeRefCounted<FrameNavigationEntry>(
      std::string(), HistoryItem::Create(GURL(url::kAboutBlankURL)),
      /*record=*/0, /*same_document_entry=*/false);
  initial.initial = true;
  entries_.push_back(std::move(initial));
}

SessionHistory::~SessionHistory() = default;

HistoryCounts SessionHistory::Counts() const {
  HistoryCounts counts;
  counts.index = static_cast<int>(current_);
  counts.length = static_cast<int>(entries_.size());
  return counts;
}

const FrameNavigationEntry& SessionHistory::RootAt(size_t index) const {
  CHECK_LT(index, entries_.size());
  return *entries_[index].root;
}

const FrameNavigationEntry& SessionHistory::CurrentRoot() const {
  return RootAt(current_);
}

std::optional<size_t> SessionHistory::IndexOfKey(const std::u16string& key,
                                                 size_t from) const {
  // NavigateToNavigationApiKey(): back from `from` first, then forward, as
  // far as the entries of the current entry's origin go, each as
  // ShouldNavigateToEntryForNavigationApiKey() says.
  const url::Origin origin =
      url::Origin::Create(entries_[current_].root->item().url);
  // Whether the entry at `index` has the key, or nullopt to stop looking.
  const auto matches = [this, &key, &origin](size_t index) {
    const Entry& entry = entries_[index];
    if (entry.initial ||
        !url::Origin::Create(entry.root->item().url).IsSameOriginWith(origin)) {
      return std::optional<bool>();
    }
    return std::optional<bool>(entry.root->item().navigation_api_key == key);
  };
  for (size_t index = from; index-- > 0;) {
    const std::optional<bool> found = matches(index);
    if (!found) {
      break;
    }
    if (*found) {
      return index;
    }
  }
  for (size_t index = from + 1; index < entries_.size(); ++index) {
    const std::optional<bool> found = matches(index);
    if (!found) {
      break;
    }
    if (*found) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<size_t> SessionHistory::IndexOfItem(
    int64_t item_sequence_number) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].root->item().item_sequence_number == item_sequence_number) {
      return i;
    }
  }
  return std::nullopt;
}

Navigation::EntryArrays SessionHistory::DidCommitNavigation(
    const HistoryItem& item,
    FrameLoadType type,
    uint64_t record,
    int64_t traversed_item_sequence_number) {
  const url::Origin origin = url::Origin::Create(item.url);
  Navigation::EntryArrays arrays;
  arrays.type = type;
  // The entry that the frame came from, if it is of the new origin.
  const Entry& old = entries_[current_];
  if (!old.initial &&
      url::Origin::Create(old.root->item().url).IsSameOriginWith(origin)) {
    arrays.previous =
        ToApiEntry(old.root->item(), item.document_sequence_number);
  }
  // A traversal goes to the entry it went to, whose frame entry the new
  // document's item and navigation take, as Chrome's controller updates the
  // FrameNavigationEntry in place, or, if a new entry pruned it meanwhile,
  // takes the current entry's place, as Chrome 157 ends.
  if (type == FrameLoadType::kBackForward) {
    if (const std::optional<size_t> index =
            IndexOfItem(traversed_item_sequence_number)) {
      current_ = *index;
    }
  }
  Entry entry;
  entry.root = base::MakeRefCounted<FrameNavigationEntry>(
      std::string(), item, record, /*same_document_entry=*/false);
  if (type == FrameLoadType::kStandard && !old.initial) {
    PushEntry(std::move(entry));
  } else {
    // A replace, a reload, a traversal, and the first navigation, which
    // replaces the initial entry.
    entries_[current_] = std::move(entry);
  }
  FillApiEntries(current_, origin, arrays);
  return arrays;
}

void SessionHistory::DidCommitSameDocumentNavigation(
    const HistoryItem& item,
    FrameLoadType type,
    uint64_t record,
    PageReferrerPolicy referrer_policy) {
  // An entry within the document repeats the navigation that made the
  // document, with the document's origin as its Initiator.
  switch (type) {
    case FrameLoadType::kStandard: {
      Entry entry;
      entry.root = base::MakeRefCounted<FrameNavigationEntry>(
          std::string(), item, record, /*same_document_entry=*/true,
          referrer_policy);
      PushEntry(std::move(entry));
      return;
    }
    case FrameLoadType::kReplaceCurrentItem:
    case FrameLoadType::kReload:
      entries_[current_].root = base::MakeRefCounted<FrameNavigationEntry>(
          std::string(), item, record, /*same_document_entry=*/true,
          referrer_policy);
      return;
    case FrameLoadType::kBackForward:
      if (const std::optional<size_t> index =
              IndexOfItem(item.item_sequence_number)) {
        current_ = *index;
      }
      return;
  }
}

void SessionHistory::UpdateCurrentItem(const HistoryItem& item) {
  entries_[current_].root->set_item(item);
}

std::set<uint64_t> SessionHistory::RecordsHeld() const {
  std::set<uint64_t> records;
  for (const Entry& entry : entries_) {
    if (entry.root->record() != 0) {
      records.insert(entry.root->record());
    }
  }
  return records;
}

std::vector<std::u16string> SessionHistory::TakeRemovedKeys() {
  return std::exchange(removed_keys_, {});
}

void SessionHistory::PushEntry(Entry entry) {
  entries_.resize(current_ + 1);
  // PruneOldestSkippableEntryIfFull(): the oldest goes, as each entry before
  // one that a Page's script added is skippable, the Page having no user
  // activation.
  if (entries_.size() >= kMaxEntries) {
    const std::u16string key = entries_.front().root->item().navigation_api_key;
    entries_.erase(entries_.begin());
    const bool kept =
        key == entry.root->item().navigation_api_key ||
        std::any_of(entries_.begin(), entries_.end(),
                    [&key](const Entry& other) {
                      return other.root->item().navigation_api_key == key;
                    });
    if (!kept) {
      removed_keys_.push_back(key);
    }
  }
  entries_.push_back(std::move(entry));
  current_ = entries_.size() - 1;
}

void SessionHistory::FillApiEntries(size_t index,
                                    const url::Origin& origin,
                                    Navigation::EntryArrays& arrays) const {
  // PopulateSingleNavigationApiHistoryEntryVector(): outwards while the
  // entries are of the origin, each item once.
  const int64_t document_sequence_number =
      entries_[index].root->item().document_sequence_number;
  const auto fill = [this, &origin, document_sequence_number](
                        size_t from, bool back,
                        std::vector<Navigation::ApiEntry>& out) {
    int64_t last = entries_[from].root->item().item_sequence_number;
    size_t i = from;
    while (back ? i > 0 : i + 1 < entries_.size()) {
      i = back ? i - 1 : i + 1;
      const Entry& entry = entries_[i];
      const HistoryItem& item = entry.root->item();
      if (entry.initial ||
          !url::Origin::Create(item.url).IsSameOriginWith(origin)) {
        break;
      }
      if (item.item_sequence_number == last) {
        continue;
      }
      last = item.item_sequence_number;
      out.push_back(ToApiEntry(item, document_sequence_number));
    }
  };
  fill(index, /*back=*/true, arrays.back);
  std::reverse(arrays.back.begin(), arrays.back.end());
  fill(index, /*back=*/false, arrays.forward);
}

}  // namespace mitto
