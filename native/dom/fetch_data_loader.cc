#include "mitto/dom/fetch_data_loader.h"

#include <stdint.h>

#include <limits>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/bytes_consumer.h"
#include "v8/include/cppgc/member.h"

namespace mitto {
namespace {

// Blink's FetchDataLoaderAsArrayBuffer and FetchDataLoaderAsString, which
// differ only in what they give at the end: the decoder of the latter turns
// the same bytes into the same string whether it decodes them in pieces or
// whole.
class FetchDataLoaderAsBytes final : public FetchDataLoader,
                                     public BytesConsumer::Client {
 public:
  explicit FetchDataLoaderAsBytes(bool as_string) : as_string_(as_string) {}

  void Start(BytesConsumer* consumer,
             FetchDataLoader::Client* client) override {
    DCHECK(!client_);
    DCHECK(!consumer_);
    client_ = client;
    consumer_ = consumer;
    consumer_->SetClient(this);
    OnStateChange();
  }

  void Cancel() override { consumer_->Cancel(); }

  void OnStateChange() override {
    // A consumer that errs while it is read tells its client again, as
    // DataPipeBytesConsumer's EndRead() does, after the client has heard.
    if (finished_) {
      return;
    }
    while (true) {
      base::span<const char> buffer;
      auto result = consumer_->BeginRead(buffer);
      if (result == BytesConsumer::Result::kShouldWait) {
        return;
      }
      if (result == BytesConsumer::Result::kOk) {
        if (!buffer.empty()) {
          if (!Append(buffer)) {
            std::ignore = consumer_->EndRead(0);
            Cancel();
            Finish()->DidFetchDataLoadFailed();
            return;
          }
        }
        result = consumer_->EndRead(buffer.size());
        // EndRead() may have told this already.
        if (finished_) {
          return;
        }
      }
      switch (result) {
        case BytesConsumer::Result::kOk:
          break;
        case BytesConsumer::Result::kShouldWait:
          NOTREACHED();
        case BytesConsumer::Result::kDone: {
          std::string bytes;
          bytes.swap(bytes_);
          if (as_string_) {
            Finish()->DidFetchDataLoadedString(DecodeUtf8Body(bytes));
          } else {
            Finish()->DidFetchDataLoadedArrayBuffer(std::move(bytes));
          }
          return;
        }
        case BytesConsumer::Result::kError:
          Finish()->DidFetchDataLoadFailed();
          return;
      }
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(consumer_);
    visitor->Trace(client_);
    FetchDataLoader::Trace(visitor);
    BytesConsumer::Client::Trace(visitor);
  }

 private:
  // The client, which hears once.
  FetchDataLoader::Client* Finish() {
    finished_ = true;
    return client_.Get();
  }

  // Returns false upon overflow: an ArrayBuffer of Blink's holds at most
  // 2^32 - 1 bytes here.
  [[nodiscard]] bool Append(base::span<const char> data) {
    DCHECK(!data.empty());
    bytes_.append(data.begin(), data.end());
    return as_string_ ||
           bytes_.size() <=
               static_cast<size_t>(std::numeric_limits<uint32_t>::max());
  }

  const bool as_string_;
  bool finished_ = false;
  cppgc::Member<BytesConsumer> consumer_;
  cppgc::Member<FetchDataLoader::Client> client_;
  std::string bytes_;
};

}  // namespace

void FetchDataLoader::Client::DidFetchDataLoadedArrayBuffer(std::string bytes) {
  NOTREACHED();
}

void FetchDataLoader::Client::DidFetchDataLoadedString(std::u16string string) {
  NOTREACHED();
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsArrayBuffer() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsBytes>(
      /*as_string=*/false);
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsString() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsBytes>(
      /*as_string=*/true);
}

std::u16string DecodeUtf8Body(std::string_view bytes) {
  if (bytes.starts_with("\xEF\xBB\xBF")) {
    bytes.remove_prefix(3);
  }
  std::u16string text;
  base::UTF8ToUTF16(bytes.data(), bytes.size(), &text);
  return text;
}

}  // namespace mitto
