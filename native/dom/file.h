// The File API's File, as Blink's: a Blob with a name and a time of last
// modification. A page makes one with its constructor; a file of the user's,
// which an input or a drop gives, waits for the forms and input that give
// them (#281, M9).
//
// Upstream: third_party/blink/renderer/core/fileapi/file.h
// Upstream: third_party/blink/renderer/core/fileapi/file.cc

#ifndef MITTO_DOM_FILE_H_
#define MITTO_DOM_FILE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/blob.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

class File final : public Blob {
 public:
  // `new File(fileBits, fileName, options)`: its time is `lastModified`, or
  // now.
  static File* Create(
      const std::vector<
          bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>& file_bits,
      const std::u16string& file_name,
      const bindings::FilePropertyBag& options);
  // One of `handle`, as a deserialized one is.
  static File* Create(std::u16string name,
                      std::u16string relative_path,
                      std::optional<base::Time> last_modified,
                      scoped_refptr<BlobDataHandle> handle);
  // The one a Known Gap gives: empty, of no name, no type and no known time.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  File(std::u16string name,
       std::u16string relative_path,
       std::optional<base::Time> last_modified,
       scoped_refptr<BlobDataHandle> handle);
  ~File() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }
  int64_t LastModified() const;
  v8::Local<v8::Value> LastModifiedDate() const;
  const std::u16string& WebkitRelativePath() const { return relative_path_; }

  // Its time as a serialization writes it: none when it is not known.
  std::optional<base::Time> LastModifiedTimeForSerialization() const {
    return last_modified_;
  }

 private:
  // Its time, or now when it is not known, as Blink's LastModifiedTime().
  base::Time LastModifiedTime() const;

  const std::u16string name_;
  const std::u16string relative_path_;
  const std::optional<base::Time> last_modified_;
};

}  // namespace mitto

#endif  // MITTO_DOM_FILE_H_
