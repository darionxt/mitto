#include "mitto/dom/entry.h"

#include "mitto/bindings/v8_entry.h"
#include "mitto/dom/dom_file_system.h"

namespace mitto {

Entry::Entry(DOMFileSystem* file_system, const std::u16string& full_path)
    : file_system_(file_system), full_path_(full_path) {}

Entry::~Entry() = default;

const bindings::WrapperTypeInfo* Entry::GetWrapperTypeInfo() const {
  return &bindings::V8Entry::kWrapperTypeInfo;
}

void Entry::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(file_system_);
}

bool Entry::IsFile() const {
  return false;
}

bool Entry::IsDirectory() const {
  return false;
}

// DOMFilePath::GetName(): what follows the last slash, so "" for the root.
std::u16string Entry::Name() const {
  const size_t slash = full_path_.rfind(u'/');
  return slash == std::u16string::npos ? full_path_
                                       : full_path_.substr(slash + 1);
}

std::u16string Entry::ToURL() const {
  return file_system_->UrlOf(full_path_);
}

}  // namespace mitto
