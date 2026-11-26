// The File API's Blob, as Blink's: an immutable run of bytes with a type, made
// from strings, buffers and other blobs, that slices, streams and reads as
// text, an ArrayBuffer or bytes. Its bytes are a BlobDataHandle, which every
// Blob made from it, and every copy that a message or a worker gets, shares.
//
// Upstream: third_party/blink/renderer/core/fileapi/blob.h
// Upstream: third_party/blink/renderer/core/fileapi/blob.cc

#ifndef MITTO_DOM_BLOB_H_
#define MITTO_DOM_BLOB_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/blob_data.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadableStream;

class Blob : public bindings::ScriptWrappable {
 public:
  // `new Blob(blobParts, options)`.
  static Blob* Create(
      const std::optional<std::vector<
          bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>>& blob_parts,
      const bindings::BlobPropertyBag& options);
  // One of the bytes of `handle`.
  static Blob* Create(scoped_refptr<BlobDataHandle> handle);
  // One of `bytes`, of `content_type`, as Blink's Blob::Create(data, type).
  static Blob* Create(base::span<const uint8_t> bytes,
                      std::string_view content_type);
  // The one a Known Gap gives: empty, of no type.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Blob(scoped_refptr<BlobDataHandle> handle);
  ~Blob() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint64_t Size() const;
  std::u16string Type() const;
  Blob* Slice(int64_t start,
              int64_t end,
              const std::u16string& content_type,
              bindings::ExceptionState& exception_state) const;
  ReadableStream* Stream() const;
  ReadableStream* TextStream(bindings::ExceptionState& exception_state) const;
  v8::Local<v8::Promise> Text();
  v8::Local<v8::Promise> ArrayBuffer();
  v8::Local<v8::Promise> Bytes();

  const scoped_refptr<BlobDataHandle>& GetBlobDataHandle() const {
    return blob_data_handle_;
  }
  virtual bool IsFile() const;
  // Adds all of its bytes to `blob_data`.
  void AppendTo(BlobData& blob_data) const;

  // A type as Blink's NormalizeType() keeps it: in lowercase, or empty if it
  // is longer than 65,535 characters or has any that are not printable ASCII.
  static std::string NormalizeType(std::u16string_view type);
  // Fills `blob_data` with `parts`, as Blink's PopulateBlobData() does.
  static void PopulateBlobData(
      BlobData& blob_data,
      const std::vector<
          bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>& parts,
      bool normalize_line_endings_to_native);
  // Where a slice of a blob of `size` starts and ends, as Blink's
  // ClampSliceOffsets() works them out: a negative offset counts from the
  // end, and both are clamped to the blob.
  static void ClampSliceOffsets(uint64_t size, int64_t& start, int64_t& end);

 private:
  const scoped_refptr<BlobDataHandle> blob_data_handle_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BLOB_H_
