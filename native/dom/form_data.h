// The XMLHttpRequest Standard's FormData: a list of entries, each a name and
// a string or a File, as Blink's FormData keeps them. A Blob that goes in
// becomes a File named "blob", or the name given, of the same bytes. Its
// body is multipart/form-data, as Blink's EncodeMultiPartFormData() writes
// it. `new FormData(form)` is a Known Gap until forms build entry lists
// (#281): it gives an empty FormData.
//
// Upstream: third_party/blink/renderer/core/html/forms/form_data.h
// Upstream: third_party/blink/renderer/core/html/forms/form_data.cc

#ifndef MITTO_DOM_FORM_DATA_H_
#define MITTO_DOM_FORM_DATA_H_

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/pair_iterable.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Blob;
class File;
class HTMLElement;
class HTMLFormElement;

class FormData final : public bindings::ScriptWrappable {
 public:
  // One entry: a name and a string, or a name and a File, with the file name
  // that append() or set() took, if any.
  class Entry final : public cppgc::GarbageCollected<Entry> {
   public:
    Entry(std::u16string name, std::u16string value);
    Entry(std::u16string name,
          Blob* blob,
          std::optional<std::u16string> filename);
    ~Entry();

    const std::u16string& name() const { return name_; }
    const std::u16string& value() const { return value_; }
    File* file() const { return file_.Get(); }
    const std::optional<std::u16string>& filename() const { return filename_; }
    bool IsFile() const { return file_; }

    void Trace(cppgc::Visitor* visitor) const;

   private:
    const std::u16string name_;
    const std::u16string value_;
    const cppgc::Member<File> file_;
    const std::optional<std::u16string> filename_;
  };

  // A body of multipart/form-data, and whether it holds a file's bytes, which
  // Blink reads as a blob.
  struct MultipartBody {
    MultipartBody();
    MultipartBody(MultipartBody&&);
    MultipartBody& operator=(MultipartBody&&);
    ~MultipartBody();

    std::string boundary;
    std::string bytes;
    bool has_files = false;

    // "multipart/form-data; boundary=" and the boundary.
    std::string ContentType() const;
  };

  // `new FormData(form, submitter)`.
  static FormData* Create(const std::optional<HTMLFormElement*>& form,
                          HTMLElement* submitter,
                          bindings::ExceptionState& exception_state);
  static FormData* Create();
  // The one a Known Gap gives, such as a FormDataEvent's: empty.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  FormData();
  ~FormData() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void Append(const std::u16string& name, const std::u16string& value);
  void Append(const std::u16string& name,
              Blob* blob,
              const std::optional<std::u16string>& filename);
  void DeleteEntry(const std::u16string& name);
  v8::Local<v8::Value> Get(const std::u16string& name) const;
  std::vector<v8::Local<v8::Value>> GetAll(const std::u16string& name) const;
  bool Has(const std::u16string& name) const;
  void Set(const std::u16string& name, const std::u16string& value);
  void Set(const std::u16string& name,
           Blob* blob,
           const std::optional<std::u16string>& filename);
  bindings::PairIterationSource* CreateIterationSource();

  size_t size() const { return entries_.size(); }
  const Entry* EntryAt(size_t index) const { return entries_[index]; }

  // The entries as a multipart/form-data body, with a new boundary.
  MultipartBody EncodeMultiPartFormData() const;

 private:
  class IterationSource;

  // The entry's value as a page sees it: its string or its File.
  static v8::Local<v8::Value> ValueOf(const Entry& entry);
  void SetEntry(Entry* entry);

  HeapVector<Entry> entries_;
};

}  // namespace mitto

#endif  // MITTO_DOM_FORM_DATA_H_
