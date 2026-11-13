// The File and Directory Entries API's Entry: an entry of a file system that
// webkitRequestFileSystem() gave, with its path, name and URL. Moving,
// copying, removing it and reading its metadata are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/filesystem/entry.cc
// Upstream: third_party/blink/renderer/modules/filesystem/entry_base.cc
// Upstream: third_party/blink/renderer/modules/filesystem/dom_file_path.cc

#ifndef MITTO_DOM_ENTRY_H_
#define MITTO_DOM_ENTRY_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class DOMFileSystem;

class Entry : public bindings::ScriptWrappable {
 public:
  ~Entry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  virtual bool IsFile() const;
  virtual bool IsDirectory() const;
  std::u16string Name() const;
  std::u16string FullPath() const { return full_path_; }
  DOMFileSystem* Filesystem() const { return file_system_.Get(); }
  std::u16string ToURL() const;

 protected:
  Entry(DOMFileSystem* file_system, const std::u16string& full_path);

 private:
  cppgc::Member<DOMFileSystem> file_system_;
  const std::u16string full_path_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ENTRY_H_
