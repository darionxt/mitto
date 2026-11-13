// The File and Directory Entries API's DirectoryEntry: the root of a file
// system that webkitRequestFileSystem() gave. Reading it and making entries in
// it are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/filesystem/directory_entry.cc

#ifndef MITTO_DOM_DIRECTORY_ENTRY_H_
#define MITTO_DOM_DIRECTORY_ENTRY_H_

#include <string>

#include "mitto/dom/entry.h"

namespace mitto {

class DirectoryEntry final : public Entry {
 public:
  static DirectoryEntry* Create(DOMFileSystem* file_system,
                                const std::u16string& full_path);

  DirectoryEntry(DOMFileSystem* file_system, const std::u16string& full_path);
  ~DirectoryEntry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  bool IsDirectory() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_DIRECTORY_ENTRY_H_
