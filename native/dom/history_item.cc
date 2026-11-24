#include "mitto/dom/history_item.h"

#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "url/origin.h"

namespace mitto {

HistoryItem::HistoryItem() = default;
HistoryItem::HistoryItem(const HistoryItem&) = default;
HistoryItem& HistoryItem::operator=(const HistoryItem&) = default;
HistoryItem::HistoryItem(HistoryItem&&) = default;
HistoryItem& HistoryItem::operator=(HistoryItem&&) = default;
HistoryItem::~HistoryItem() = default;

// static
int64_t HistoryItem::GenerateSequenceNumber() {
  // Blink starts from the time, so that numbers of other sessions seldom
  // meet these.
  static int64_t next = base::Time::Now().InMillisecondsSinceUnixEpoch() * 1000;
  return ++next;
}

// static
std::u16string HistoryItem::NewNavigationApiKey() {
  return base::UTF8ToUTF16(base::Uuid::GenerateRandomV4().AsLowercaseString());
}

// static
HistoryItem HistoryItem::Create(const GURL& url) {
  HistoryItem item;
  item.url = url;
  item.item_sequence_number = GenerateSequenceNumber();
  item.document_sequence_number = GenerateSequenceNumber();
  item.navigation_api_key = NewNavigationApiKey();
  item.navigation_api_id = NewNavigationApiKey();
  return item;
}

// static
HistoryItem HistoryItem::ForCommit(const HistoryItem* old_item,
                                   const GURL& url,
                                   FrameLoadType type,
                                   HistoryNavigationType navigation_type) {
  HistoryItem item = Create(url);
  if (!old_item) {
    return item;
  }
  // LoadTypeToCommitType(): a replace and a reload make no new entry.
  const bool inert = type == FrameLoadType::kReplaceCurrentItem ||
                     type == FrameLoadType::kReload;
  const bool reload = type == FrameLoadType::kReload;
  // Nothing of the old entry's goes to another origin, as a reload that a
  // redirect takes there commits in another of Chrome's renderers.
  const bool same_origin = url::Origin::Create(old_item->url)
                               .IsSameOriginWith(url::Origin::Create(url));
  // The key stands for a slot of the list, which a replace within the origin
  // keeps.
  if (inert && same_origin) {
    item.navigation_api_key = old_item->navigation_api_key;
  }
  // The ID stands for the entry, which a reload keeps.
  if (reload && same_origin) {
    item.navigation_api_id = old_item->navigation_api_id;
  }
  // The Navigation API's state goes on to a fragment's entry and a reload.
  if (navigation_type == HistoryNavigationType::kFragment ||
      (reload && same_origin)) {
    item.navigation_api_state = old_item->navigation_api_state;
  }
  // A new document's entry takes nothing more, unless it reloads or replaces
  // the same URL but for the fragment.
  if (navigation_type == HistoryNavigationType::kDifferentDocument &&
      (!inert || old_item->url.GetWithoutRef() != url.GetWithoutRef())) {
    return item;
  }
  item.document_sequence_number = old_item->document_sequence_number;
  item.scroll_restoration = old_item->scroll_restoration;
  // The same entry, for a replace of the same URL or by replaceState().
  if (inert && (navigation_type == HistoryNavigationType::kHistoryApi ||
                old_item->url == url)) {
    item.state = old_item->state;
    item.item_sequence_number = old_item->item_sequence_number;
  }
  return item;
}

}  // namespace mitto
