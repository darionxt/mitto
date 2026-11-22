#include "mitto/dom/form_data.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_form_data.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/file.h"
#include "mitto/dom/form_data_encoder.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The File an entry holds for `blob`: the File itself, a copy named
// `filename`, or for a Blob that is not a File a new one of its bytes, named
// `filename` or "blob", of now.
File* CreateFileFromBlob(Blob* blob,
                         const std::optional<std::u16string>& filename) {
  if (blob->IsFile()) {
    File* const file = static_cast<File*>(blob);
    return filename ? file->Clone(*filename) : file;
  }
  return File::Create(filename.value_or(u"blob"), std::u16string(),
                      base::Time::Now(), blob->GetBlobDataHandle());
}

// Each CR, LF and CRLF as CRLF, as Blink's NormalizeLineEndingsToCrLf().
std::u16string NormalizeLineEndingsToCrLf(std::u16string_view text) {
  std::u16string normalized;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      normalized += u"\r\n";
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    } else if (text[i] == '\n') {
      normalized += u"\r\n";
    } else {
      normalized += text[i];
    }
  }
  return normalized;
}

}  // namespace

class FormData::IterationSource final : public bindings::PairIterationSource {
 public:
  explicit IterationSource(FormData* form_data) : form_data_(form_data) {}

  void Trace(cppgc::Visitor* visitor) const override {
    bindings::PairIterationSource::Trace(visitor);
    visitor->Trace(form_data_);
  }

 protected:
  bool FetchNextItem(v8::Isolate* isolate,
                     v8::Local<v8::Value>* key,
                     v8::Local<v8::Value>* value) override {
    if (current_ >= form_data_->size()) {
      return false;
    }
    const Entry& entry = *form_data_->EntryAt(current_++);
    *key = bindings::ToV8String(isolate, entry.name());
    *value = ValueOf(entry);
    return true;
  }

 private:
  const cppgc::Member<FormData> form_data_;
  size_t current_ = 0;
};

FormData::Entry::Entry(std::u16string name, std::u16string value)
    : name_(std::move(name)), value_(std::move(value)) {}

FormData::Entry::Entry(std::u16string name,
                       Blob* blob,
                       std::optional<std::u16string> filename)
    : name_(std::move(name)),
      file_(CreateFileFromBlob(blob, filename)),
      filename_(std::move(filename)) {}

FormData::Entry::~Entry() = default;

void FormData::Entry::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(file_);
}

FormData::MultipartBody::MultipartBody() = default;
FormData::MultipartBody::MultipartBody(MultipartBody&&) = default;
FormData::MultipartBody& FormData::MultipartBody::operator=(MultipartBody&&) =
    default;
FormData::MultipartBody::~MultipartBody() = default;

std::string FormData::MultipartBody::ContentType() const {
  return base::StrCat({"multipart/form-data; boundary=", boundary});
}

// static
FormData* FormData::Create(const std::optional<HTMLFormElement*>& form,
                           HTMLElement* submitter,
                           bindings::ExceptionState& exception_state) {
  // A form's entry list is a Known Gap until #281.
  return Create();
}

// static
FormData* FormData::Create() {
  return bindings::MakeGarbageCollected<FormData>();
}

FormData::FormData() = default;
FormData::~FormData() = default;

const bindings::WrapperTypeInfo* FormData::GetWrapperTypeInfo() const {
  return &bindings::V8FormData::kWrapperTypeInfo;
}

void FormData::Trace(cppgc::Visitor* visitor) const {
  entries_.Trace(visitor);
  bindings::ScriptWrappable::Trace(visitor);
}

void FormData::Append(const std::u16string& name, const std::u16string& value) {
  entries_.push_back(bindings::MakeGarbageCollected<Entry>(name, value));
}

void FormData::Append(const std::u16string& name,
                      Blob* blob,
                      const std::optional<std::u16string>& filename) {
  entries_.push_back(
      bindings::MakeGarbageCollected<Entry>(name, blob, filename));
}

void FormData::DeleteEntry(const std::u16string& name) {
  entries_.EraseIf([&name](Entry* entry) { return entry->name() == name; });
}

v8::Local<v8::Value> FormData::Get(const std::u16string& name) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name() == name) {
      return ValueOf(*entries_[i]);
    }
  }
  return v8::Null(v8::Isolate::GetCurrent());
}

std::vector<v8::Local<v8::Value>> FormData::GetAll(
    const std::u16string& name) const {
  std::vector<v8::Local<v8::Value>> values;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name() == name) {
      values.push_back(ValueOf(*entries_[i]));
    }
  }
  return values;
}

bool FormData::Has(const std::u16string& name) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name() == name) {
      return true;
    }
  }
  return false;
}

void FormData::Set(const std::u16string& name, const std::u16string& value) {
  SetEntry(bindings::MakeGarbageCollected<Entry>(name, value));
}

void FormData::Set(const std::u16string& name,
                   Blob* blob,
                   const std::optional<std::u16string>& filename) {
  SetEntry(bindings::MakeGarbageCollected<Entry>(name, blob, filename));
}

bindings::PairIterationSource* FormData::CreateIterationSource() {
  return bindings::MakeGarbageCollected<IterationSource>(this);
}

void FormData::SetEntry(Entry* entry) {
  // The first entry of the name takes its place, and the others go.
  size_t first = entries_.size();
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name() == entry->name()) {
      first = i;
      break;
    }
  }
  if (first == entries_.size()) {
    entries_.push_back(entry);
    return;
  }
  entries_.set(first, entry);
  const std::u16string& name = entry->name();
  entries_.EraseIf([&name, entry](Entry* other) {
    return other != entry && other->name() == name;
  });
}

FormData::MultipartBody FormData::EncodeMultiPartFormData() const {
  MultipartBody body;
  body.boundary = form_data_encoder::GenerateUniqueBoundaryString();
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = *entries_[i];
    form_data_encoder::BeginMultiPartHeader(body.bytes, body.boundary,
                                            base::UTF16ToUTF8(entry.name()));
    if (File* const file = entry.file()) {
      // The relative path, if it has one, or else its name, unless append()
      // or set() took a file name.
      std::u16string name = file->WebkitRelativePath().empty()
                                ? file->Name()
                                : file->WebkitRelativePath();
      if (entry.filename()) {
        name = *entry.filename();
      }
      // The header has filename="..." even for an empty name.
      form_data_encoder::AddFilenameToMultiPartHeader(body.bytes,
                                                      base::UTF16ToUTF8(name));
      const std::string& type = file->GetBlobDataHandle()->GetType();
      form_data_encoder::AddContentTypeToMultiPartHeader(
          body.bytes, type.empty() ? "application/octet-stream" : type);
    }
    form_data_encoder::FinishMultiPartHeader(body.bytes);
    if (File* const file = entry.file()) {
      const base::span<const uint8_t> bytes =
          file->GetBlobDataHandle()->Bytes();
      body.bytes.append(bytes.begin(), bytes.end());
      body.has_files = true;
    } else {
      body.bytes +=
          base::UTF16ToUTF8(NormalizeLineEndingsToCrLf(entry.value()));
    }
    body.bytes += "\r\n";
  }
  form_data_encoder::AddBoundaryToMultiPartHeader(body.bytes, body.boundary,
                                                  /*is_last_boundary=*/true);
  return body;
}

// static
v8::Local<v8::Value> FormData::ValueOf(const Entry& entry) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (File* const file = entry.file()) {
    return bindings::ToV8(isolate, file);
  }
  return bindings::ToV8String(isolate, entry.value());
}

}  // namespace mitto
