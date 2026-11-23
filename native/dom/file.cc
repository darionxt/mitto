#include "mitto/dom/file.h"

#include <utility>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_file.h"
#include "v8/include/v8-date.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
File* File::Create(
    const std::vector<bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>&
        file_bits,
    const std::u16string& file_name,
    const bindings::FilePropertyBag& options) {
  // Milliseconds since the epoch, added as an integer, since a double would
  // lose precision, as Blink adds them.
  const base::Time last_modified =
      options.last_modified
          ? base::Time::UnixEpoch() + base::Milliseconds(*options.last_modified)
          : base::Time::Now();
  BlobData blob_data;
  blob_data.SetContentType(NormalizeType(options.type));
  PopulateBlobData(
      blob_data, file_bits,
      /*normalize_line_endings_to_native=*/options.endings == u"native");
  return Create(file_name, std::u16string(), last_modified,
                BlobDataHandle::Create(std::move(blob_data)));
}

// static
File* File::Create(std::u16string name,
                   std::u16string relative_path,
                   std::optional<base::Time> last_modified,
                   scoped_refptr<BlobDataHandle> handle) {
  return bindings::MakeGarbageCollected<File>(std::move(name),
                                              std::move(relative_path),
                                              last_modified, std::move(handle));
}

// static
bindings::ScriptWrappable* File::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<File>(
      info, bindings::V8File::kWrapperTypeInfo, std::u16string(),
      std::u16string(), std::nullopt,
      BlobDataHandle::Create(std::string(), std::string()));
}

File::File(std::u16string name,
           std::u16string relative_path,
           std::optional<base::Time> last_modified,
           scoped_refptr<BlobDataHandle> handle)
    : Blob(std::move(handle)),
      name_(std::move(name)),
      relative_path_(std::move(relative_path)),
      last_modified_(last_modified) {}

File::~File() = default;

const bindings::WrapperTypeInfo* File::GetWrapperTypeInfo() const {
  return &bindings::V8File::kWrapperTypeInfo;
}

base::Time File::LastModifiedTime() const {
  // A file whose time is not known gives the time of each read, as Blink's.
  return last_modified_.value_or(base::Time::Now());
}

int64_t File::LastModified() const {
  return (LastModifiedTime() - base::Time::UnixEpoch()).InMilliseconds();
}

v8::Local<v8::Value> File::LastModifiedDate() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return v8::Date::New(isolate->GetCurrentContext(),
                       LastModifiedTime().InMillisecondsFSinceUnixEpoch())
      .FromMaybe(v8::Local<v8::Value>());
}

}  // namespace mitto
