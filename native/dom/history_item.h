// What an entry of the session history holds for one frame, as Blink's
// HistoryItem and the PageState that the browser keeps of it: its URL, the
// sequence numbers that tell its document and the entry itself apart, the
// state that history.pushState() and replaceState() give it, the Navigation
// API's key, ID and state, and how its scroll position comes back. A window
// holds the one of its document's current entry, and the Page's session
// history a copy of each.
//
// Upstream: third_party/blink/renderer/core/loader/history_item.h

#ifndef MITTO_DOM_HISTORY_ITEM_H_
#define MITTO_DOM_HISTORY_ITEM_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "mitto/dom/frame_loader.h"
#include "mitto/dom/serialized_script_value.h"
#include "url/gurl.h"

namespace mitto {

// Blink's HistoryNavigationType: what kind of commit made an entry.
enum class HistoryNavigationType : uint8_t {
  kDifferentDocument,
  kFragment,
  kHistoryApi,
};

// mojom::ScrollRestorationType.
enum class ScrollRestorationType : uint8_t {
  kAuto,
  kManual,
};

struct HistoryItem {
  HistoryItem();
  HistoryItem(const HistoryItem&);
  HistoryItem& operator=(const HistoryItem&);
  HistoryItem(HistoryItem&&);
  HistoryItem& operator=(HistoryItem&&);
  ~HistoryItem();

  // A new number for an entry or a document, larger than each before it, as
  // Blink's HistoryItem::GenerateSequenceNumber() gives one.
  static int64_t GenerateSequenceNumber();
  // A new random key or ID of the Navigation API, as Blink's
  // CreateCanonicalUuidString() makes one.
  static std::u16string NewNavigationApiKey();
  // A new item at `url`, with new sequence numbers and a new key and ID.
  static HistoryItem Create(const GURL& url);
  // DocumentLoader::SetHistoryItemStateForCommit() for a commit of `type`
  // that is not a traversal: a new item at `url` that takes from `old_item`,
  // if any, what Blink carries over: the key for a replace or a reload within
  // the origin, the ID for a reload, the Navigation API's state for a
  // fragment or a reload, and, within the document or for a replace or a
  // reload of its URL but the fragment, the document's sequence number and
  // scrollRestoration, and for a replace that keeps the URL, or
  // replaceState(), the entry's own sequence number and state too.
  static HistoryItem ForCommit(const HistoryItem* old_item,
                               const GURL& url,
                               FrameLoadType type,
                               HistoryNavigationType navigation_type);

  GURL url;
  int64_t item_sequence_number = 0;
  int64_t document_sequence_number = 0;
  // What pushState() or replaceState() gave, none otherwise. A pointer, so
  // that history.state gives the same object while the entry keeps it, as
  // Blink compares SerializedScriptValue pointers.
  scoped_refptr<const SharedSerializedScriptValue> state;
  std::u16string navigation_api_key;
  std::u16string navigation_api_id;
  scoped_refptr<const SharedSerializedScriptValue> navigation_api_state;
  ScrollRestorationType scroll_restoration = ScrollRestorationType::kAuto;
  // Whether another document's Navigation API entries leave out its URL, as
  // Chrome's FrameNavigationEntry::protect_url_in_navigation_api() does for an
  // entry made under a referrer policy of no-referrer or origin.
  bool protect_url_in_navigation_api = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_HISTORY_ITEM_H_
