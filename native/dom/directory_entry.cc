#include "mitto/dom/directory_entry.h"

#include "mitto/bindings/v8_directory_entry.h"

namespace mitto {

// static
DirectoryEntry* DirectoryEntry::Create(DOMFileSystem* file_system,
                                       const std::u16string& full_path) {
  return bindings::MakeGarbageCollected<DirectoryEntry>(file_system, full_path);
}

DirectoryEntry::DirectoryEntry(DOMFileSystem* file_system,
                               const std::u16string& full_path)
    : Entry(file_system, full_path) {}

DirectoryEntry::~DirectoryEntry() = default;

const bindings::WrapperTypeInfo* DirectoryEntry::GetWrapperTypeInfo() const {
  return &bindings::V8DirectoryEntry::kWrapperTypeInfo;
}

bool DirectoryEntry::IsDirectory() const {
  return true;
}

}  // namespace mitto
