// The File and Directory Entries API's DOMFileSystem, which
// webkitRequestFileSystem() gives its success callback: the file system of the
// document's origin, by Chrome's name for it, and its root directory.
//
// Upstream: third_party/blink/renderer/modules/filesystem/dom_file_system.cc
// Upstream: third_party/blink/renderer/modules/filesystem/dom_file_system_base.cc
// Upstream: storage/common/file_system/file_system_util.cc
// Upstream: storage/common/database/database_identifier.cc

#ifndef MITTO_DOM_DOM_FILE_SYSTEM_H_
#define MITTO_DOM_DOM_FILE_SYSTEM_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "url/origin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class DirectoryEntry;

class DOMFileSystem final : public bindings::ScriptWrappable {
 public:
  // The two kinds of file system that a page can ask for, as
  // mojom::FileSystemType numbers them.
  enum class Type { kTemporary = 0, kPersistent = 1 };

  // The file system of `type` of the non-opaque `origin`.
  static DOMFileSystem* Create(const url::Origin& origin, Type type);

  DOMFileSystem(const url::Origin& origin, Type type);
  ~DOMFileSystem() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return name_; }
  DirectoryEntry* Root() const { return root_.Get(); }

  // The URL of the entry at `full_path`, which starts with "/", as
  // DOMFileSystemBase::CreateFileSystemURL() writes it.
  std::u16string UrlOf(const std::u16string& full_path) const;

 private:
  const std::u16string name_;
  // The browser's root URL of the file system, such as
  // "filesystem:https://example.com/temporary/".
  const std::u16string root_url_;
  cppgc::Member<DirectoryEntry> root_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_FILE_SYSTEM_H_
