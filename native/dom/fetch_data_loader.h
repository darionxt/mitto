// Reads all of a body from a BytesConsumer, as Blink's FetchDataLoader does
// for text(), json(), arrayBuffer() and formData(), and tells its client once
// it has.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_data_loader.h
// Upstream: third_party/blink/renderer/core/fetch/fetch_data_loader.cc

#ifndef MITTO_DOM_FETCH_DATA_LOADER_H_
#define MITTO_DOM_FETCH_DATA_LOADER_H_

#include <string>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

class BytesConsumer;
class FormData;

class FetchDataLoader : public cppgc::GarbageCollected<FetchDataLoader> {
 public:
  class Client : public cppgc::GarbageCollectedMixin {
   public:
    virtual ~Client() = default;

    // The whole body, from a loader as an ArrayBuffer.
    virtual void DidFetchDataLoadedArrayBuffer(std::string bytes);
    // The whole body decoded as UTF-8, from a loader as a string.
    virtual void DidFetchDataLoadedString(std::u16string string);
    // The parts of a multipart/form-data body, from a loader as FormData.
    virtual void DidFetchDataLoadedFormData(FormData* form_data);
    virtual void DidFetchDataLoadFailed() = 0;
    // The body's signal aborted before the loader started, with `reason`.
    virtual void Abort(v8::Local<v8::Value> reason) = 0;

    void Trace(cppgc::Visitor* visitor) const override {}
  };

  // A loader that gives the bytes, and one that gives them decoded as UTF-8,
  // with a byte order mark dropped, as Blink's TextResourceDecoder does with
  // CreateUTF8Decode().
  static FetchDataLoader* CreateLoaderAsArrayBuffer();
  static FetchDataLoader* CreateLoaderAsString();
  // One that gives the bytes decoded as UTF-8 with a byte order mark kept as
  // U+FEFF, as CreateUTF8DecodeWithoutBOM() has them, as the URL Standard's
  // application/x-www-form-urlencoded parser takes them.
  static FetchDataLoader* CreateLoaderAsStringKeepingByteOrderMark();
  // One that parses a multipart/form-data body of `boundary` into a FormData,
  // as Blink's FetchDataLoaderAsFormData: a part with a file name gives a
  // File of its bytes, of its Content-Type or text/plain, and any other its
  // bytes decoded as UTF-8, and a part that is not UTF-8, has no name or is
  // not of type form-data fails it.
  static FetchDataLoader* CreateLoaderAsFormData(std::string boundary);
  // One that reads the whole body and then fails.
  static FetchDataLoader* CreateLoaderAsFailure();

  FetchDataLoader() = default;
  FetchDataLoader(const FetchDataLoader&) = delete;
  FetchDataLoader& operator=(const FetchDataLoader&) = delete;
  virtual ~FetchDataLoader() = default;

  // Reads `consumer` to its end, and then tells `client`. Called once.
  virtual void Start(BytesConsumer* consumer, Client* client) = 0;
  virtual void Cancel() = 0;

  virtual void Trace(cppgc::Visitor* visitor) const {}
};

// The Encoding Standard's UTF-8 decode of a whole body: a byte order mark
// goes, and each sequence that is not UTF-8 becomes U+FFFD.
std::u16string DecodeUtf8Body(std::string_view bytes);

}  // namespace mitto

#endif  // MITTO_DOM_FETCH_DATA_LOADER_H_
