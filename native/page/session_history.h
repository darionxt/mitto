// The session history of a Page's tab, on the Page program's main thread, as
// Chrome's browser keeps it in its NavigationController: a list of entries,
// at most 50, each a tree of frame entries, one per frame, with the
// top-level frame's at the root, and the entry that the frames show now. The
// Page holds every frame of a tab, so the list is the Page's, beside its
// frames, and history.length is its own count, at once, as the renderer's
// copy of it is in Chrome.
//
// A frame entry holds what its document's HistoryItem holds, and the ID of
// the document whose navigation the Session made, which a traversal to it
// repeats, as the Session never takes the Page's word for an Initiator or a
// Referer. The root matches the top-level frame by its place, and a child
// matches a frame by its unique name, as Chrome's TreeNode::MatchesFrame()
// has it; until frames have entries of their own, each entry has a root only.
//
// Upstream: content/browser/renderer_host/navigation_controller_impl.cc
// Upstream: content/browser/renderer_host/navigation_entry_impl.cc
// Upstream: content/browser/renderer_host/frame_navigation_entry.cc

#ifndef MITTO_PAGE_SESSION_HISTORY_H_
#define MITTO_PAGE_SESSION_HISTORY_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigation_client.h"
#include "url/origin.h"

namespace mitto {

// What one frame shows at one entry, which entries that the frame did not
// change share, as Chrome's FrameNavigationEntry is shared.
class FrameNavigationEntry final
    : public base::RefCounted<FrameNavigationEntry> {
 public:
  FrameNavigationEntry(std::string unique_name,
                       HistoryItem item,
                       uint64_t record);

  FrameNavigationEntry(const FrameNavigationEntry&) = delete;
  FrameNavigationEntry& operator=(const FrameNavigationEntry&) = delete;

  // Blink's unique name of its frame, empty for the top-level frame.
  const std::string& unique_name() const { return unique_name_; }
  const HistoryItem& item() const { return item_; }
  void set_item(HistoryItem item) { item_ = std::move(item); }
  // The document whose navigation the Session made, which a traversal to the
  // entry repeats: its ID, 0 for none, as for the initial about:blank.
  uint64_t record() const { return record_; }

 private:
  friend class base::RefCounted<FrameNavigationEntry>;
  ~FrameNavigationEntry();

  const std::string unique_name_;
  HistoryItem item_;
  const uint64_t record_;
};

class SessionHistory final {
 public:
  // Chrome's kMaxSessionHistoryEntries.
  static constexpr size_t kMaxEntries = 50;

  // A history with the tab's initial entry alone, at about:blank, which the
  // first navigation replaces.
  SessionHistory();

  SessionHistory(const SessionHistory&) = delete;
  SessionHistory& operator=(const SessionHistory&) = delete;

  ~SessionHistory();

  // Where the history is: the current entry's index and how many there are.
  HistoryCounts Counts() const;
  // The top-level frame's entry at `index`, which must be one.
  const FrameNavigationEntry& RootAt(size_t index) const;
  // The current entry's top-level frame entry.
  const FrameNavigationEntry& CurrentRoot() const;
  // The index of the entry whose top-level frame entry has the Navigation
  // API key `key`, nearest the current one, as Chrome finds it.
  std::optional<size_t> IndexOfKey(const std::u16string& key) const;
  // The index of the entry whose top-level frame entry is the item of
  // sequence number `item_sequence_number`, if one still is.
  std::optional<size_t> IndexOfItem(int64_t item_sequence_number) const;

  // The top-level frame committed a new document of `item`, the response of
  // the navigation that made document `record`: a new entry, in place of
  // forward ones, for kStandard, unless the current one is the initial
  // entry, and otherwise the current one replaced. Returns the entries that
  // the new document's Navigation API starts with.
  Navigation::EntryArrays DidCommitNavigation(const HistoryItem& item,
                                              FrameLoadType type,
                                              uint64_t record);
  // The top-level frame's document committed a navigation within itself to
  // `item`: a new entry for kStandard, the current one replaced for
  // kReplaceCurrentItem and kReload, and for kBackForward the entry of its
  // item became the current one.
  void DidCommitSameDocumentNavigation(const HistoryItem& item,
                                       FrameLoadType type);
  // The current entry's item changed in place.
  void UpdateCurrentItem(const HistoryItem& item);
  // The Navigation API keys of the entries dropped past kMaxEntries since the
  // last call, which no entry has any more, for the documents that still show
  // them, as RemovedEntriesTracker collects them.
  std::vector<std::u16string> TakeRemovedKeys();

 private:
  struct Entry {
    Entry();
    Entry(const Entry&);
    Entry& operator=(const Entry&);
    ~Entry();

    scoped_refptr<FrameNavigationEntry> root;
    // The tab's first entry, which no navigation made.
    bool initial = false;
  };

  // InsertOrReplaceEntry(): a new entry after the current one, in place of
  // the forward ones, dropping the oldest past kMaxEntries.
  void PushEntry(Entry entry);
  // The entries of the top-level frame of `origin` right before and after
  // `index`, earliest first, skipping those of the same item as the one next
  // to them, as GetNavigationApiHistoryEntryVectors() gives them.
  void FillApiEntries(size_t index,
                      const url::Origin& origin,
                      Navigation::EntryArrays& arrays) const;

  std::vector<Entry> entries_;
  size_t current_ = 0;
  std::vector<std::u16string> removed_keys_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_HISTORY_H_
