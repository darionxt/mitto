// The bytes of a Blob, as Blink's BlobData builds them and its BlobDataHandle
// names them. Chrome keeps a blob's bytes in the browser's
// BlobStorageContext, and a renderer holds a handle with the blob's UUID,
// type and size; a Page has no browser, so its handle holds the bytes
// themselves. They never change once made, and every Blob, slice, message and
// worker that has the handle shares them, on any thread.
//
// Upstream: third_party/blink/renderer/platform/blob/blob_data.h
// Upstream: third_party/blink/renderer/platform/blob/blob_data.cc
// Upstream: third_party/blink/renderer/platform/wtf/text/line_ending.cc

#ifndef MITTO_DOM_BLOB_DATA_H_
#define MITTO_DOM_BLOB_DATA_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"

namespace mitto {

class BlobDataHandle;

// What a Blob is made of, before it is made: bytes, text and parts of other
// blobs, in order.
class BlobData {
 public:
  BlobData();
  BlobData(BlobData&&);
  BlobData& operator=(BlobData&&);
  ~BlobData();

  // The blob's type: `type` if each of its characters is printable ASCII, and
  // otherwise empty, as Blink's SetContentType() takes it.
  void SetContentType(std::string_view type);
  const std::string& content_type() const { return content_type_; }

  // `text` as UTF-8, with its line endings as Windows Chrome writes native
  // ones, CRLF, if `normalize_line_endings_to_native`: every Persona is of
  // Windows (docs/persona.md).
  void AppendText(std::u16string_view text,
                  bool normalize_line_endings_to_native);
  void AppendBytes(base::span<const uint8_t> bytes);
  // `length` bytes of `handle` from `offset`, which hold at least as many.
  void AppendBlob(scoped_refptr<BlobDataHandle> handle,
                  uint64_t offset,
                  uint64_t length);

  // How many bytes it holds.
  uint64_t length() const;

 private:
  friend class BlobDataHandle;

  // A part: bytes of its own, or a range of another blob's.
  struct Element {
    Element();
    Element(Element&&);
    Element& operator=(Element&&);
    ~Element();

    std::string bytes;
    scoped_refptr<BlobDataHandle> blob;
    uint64_t offset = 0;
    uint64_t length = 0;
  };

  std::string content_type_;
  std::vector<Element> elements_;
};

class BlobDataHandle final : public base::RefCountedThreadSafe<BlobDataHandle> {
 public:
  // A blob of what `data` holds, which takes its type. A blob made of one
  // range of another shares that one's bytes.
  static scoped_refptr<BlobDataHandle> Create(BlobData data);
  // A blob of `bytes`, of `type`, which must be valid.
  static scoped_refptr<BlobDataHandle> Create(std::string bytes,
                                              std::string type);
  // A blob of `size` bytes of `storage` from `offset`, of `type`, which must
  // be valid.
  static scoped_refptr<BlobDataHandle> Create(
      scoped_refptr<const base::RefCountedString> storage,
      size_t offset,
      size_t size,
      std::string type);

  BlobDataHandle(const BlobDataHandle&) = delete;
  BlobDataHandle& operator=(const BlobDataHandle&) = delete;

  // A new random UUID, lowercase, as Chrome's browser names each blob.
  const std::string& Uuid() const { return uuid_; }
  const std::string& GetType() const { return type_; }
  uint64_t size() const { return size_; }
  // Its bytes.
  base::span<const uint8_t> Bytes() const;
  // Its bytes as a part of what may hold other blobs' too.
  const scoped_refptr<const base::RefCountedString>& storage() const {
    return storage_;
  }
  size_t offset() const { return offset_; }

 private:
  friend class base::RefCountedThreadSafe<BlobDataHandle>;

  BlobDataHandle(scoped_refptr<const base::RefCountedString> storage,
                 size_t offset,
                 size_t size,
                 std::string type);
  ~BlobDataHandle();

  const std::string uuid_;
  const std::string type_;
  // The bytes, which other handles may share, and where its own are in them.
  const scoped_refptr<const base::RefCountedString> storage_;
  const size_t offset_;
  const size_t size_;
};

// Whether `type` is a type that a Blob keeps: each of its characters printable
// ASCII, from U+0020 to U+007E, as Blink's IsValidBlobType() says.
bool IsValidBlobType(std::u16string_view type);
bool IsValidBlobType(std::string_view type);

}  // namespace mitto

#endif  // MITTO_DOM_BLOB_DATA_H_
