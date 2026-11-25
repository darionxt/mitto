#include "mitto/dom/fetch_data_loader.h"

#include <stdint.h>

#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/bytes_consumer.h"
#include "mitto/dom/file.h"
#include "mitto/dom/form_data.h"
#include "mitto/dom/multipart_parser.h"
#include "mitto/dom/parsed_content_type.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_codec_utf8.h"
#include "v8/include/cppgc/member.h"

namespace mitto {
namespace {

// What a FetchDataLoaderAsBytes gives at the end.
enum class LoadedAs {
  kArrayBuffer,
  kString,
  kStringKeepingByteOrderMark,
};

// Blink's FetchDataLoaderAsArrayBuffer and FetchDataLoaderAsString, which
// differ only in what they give at the end: the decoder of the latter turns
// the same bytes into the same string whether it decodes them in pieces or
// whole.
class FetchDataLoaderAsBytes final : public FetchDataLoader,
                                     public BytesConsumer::Client {
 public:
  explicit FetchDataLoaderAsBytes(LoadedAs loaded_as) : loaded_as_(loaded_as) {}

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
          switch (loaded_as_) {
            case LoadedAs::kArrayBuffer:
              Finish()->DidFetchDataLoadedArrayBuffer(std::move(bytes));
              return;
            case LoadedAs::kString:
              Finish()->DidFetchDataLoadedString(DecodeUtf8Body(bytes));
              return;
            case LoadedAs::kStringKeepingByteOrderMark: {
              std::u16string text;
              base::UTF8ToUTF16(bytes.data(), bytes.size(), &text);
              Finish()->DidFetchDataLoadedString(std::move(text));
              return;
            }
          }
          NOTREACHED();
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
    return loaded_as_ != LoadedAs::kArrayBuffer ||
           bytes_.size() <=
               static_cast<size_t>(std::numeric_limits<uint32_t>::max());
  }

  const LoadedAs loaded_as_;
  bool finished_ = false;
  cppgc::Member<BytesConsumer> consumer_;
  cppgc::Member<FetchDataLoader::Client> client_;
  std::string bytes_;
};

// Blink's FetchDataLoaderAsFailure: it reads the whole body, which goes, and
// then fails.
class FetchDataLoaderAsFailure final : public FetchDataLoader,
                                       public BytesConsumer::Client {
 public:
  void Start(BytesConsumer* consumer,
             FetchDataLoader::Client* client) override {
    DCHECK(!client_);
    DCHECK(!consumer_);
    client_ = client;
    consumer_ = consumer;
    consumer_->SetClient(this);
    OnStateChange();
  }

  void OnStateChange() override {
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
        result = consumer_->EndRead(buffer.size());
        if (finished_) {
          return;
        }
      }
      switch (result) {
        case BytesConsumer::Result::kOk:
          break;
        case BytesConsumer::Result::kShouldWait:
          NOTREACHED();
        case BytesConsumer::Result::kDone:
        case BytesConsumer::Result::kError:
          finished_ = true;
          client_->DidFetchDataLoadFailed();
          return;
      }
    }
  }

  void Cancel() override { consumer_->Cancel(); }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(consumer_);
    visitor->Trace(client_);
    FetchDataLoader::Trace(visitor);
    BytesConsumer::Client::Trace(visitor);
  }

 private:
  bool finished_ = false;
  cppgc::Member<BytesConsumer> consumer_;
  cppgc::Member<FetchDataLoader::Client> client_;
};

// Blink's FetchDataLoaderAsFormData, which feeds a MultipartParser each piece
// of the body as it comes, and stops at the first that does not parse.
class FetchDataLoaderAsFormData final : public FetchDataLoader,
                                        public BytesConsumer::Client,
                                        public MultipartParser::Client {
 public:
  explicit FetchDataLoaderAsFormData(std::string boundary)
      : boundary_(std::move(boundary)) {}

  void Start(BytesConsumer* consumer,
             FetchDataLoader::Client* client) override {
    DCHECK(!client_);
    DCHECK(!consumer_);
    client_ = client;
    form_data_ = FormData::Create();
    multipart_parser_ =
        bindings::MakeGarbageCollected<MultipartParser>(boundary_, this);
    consumer_ = consumer;
    consumer_->SetClient(this);
    OnStateChange();
  }

  void OnStateChange() override {
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
        const bool buffer_appended = multipart_parser_->AppendData(buffer);
        const bool multipart_receive_failed = multipart_parser_->IsCancelled();
        result = consumer_->EndRead(buffer.size());
        if (finished_) {
          return;
        }
        if (!buffer_appended || multipart_receive_failed) {
          // No point in reading any more of a body that does not parse.
          consumer_->Cancel();
          Fail();
          return;
        }
      }
      switch (result) {
        case BytesConsumer::Result::kOk:
          break;
        case BytesConsumer::Result::kShouldWait:
          NOTREACHED();
        case BytesConsumer::Result::kDone:
          if (multipart_parser_->Finish()) {
            DCHECK(!multipart_parser_->IsCancelled());
            finished_ = true;
            client_->DidFetchDataLoadedFormData(form_data_);
          } else {
            Fail();
          }
          return;
        case BytesConsumer::Result::kError:
          Fail();
          return;
      }
    }
  }

  void Cancel() override {
    consumer_->Cancel();
    multipart_parser_->Cancel();
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(consumer_);
    visitor->Trace(client_);
    visitor->Trace(form_data_);
    visitor->Trace(multipart_parser_);
    FetchDataLoader::Trace(visitor);
    BytesConsumer::Client::Trace(visitor);
    MultipartParser::Client::Trace(visitor);
  }

 private:
  // MultipartParser::Client:
  void PartHeaderFieldsInMultipartReceived(
      const MultipartPartHeaders& header_fields) override {
    if (!current_entry_.Initialize(header_fields)) {
      multipart_parser_->Cancel();
    }
  }

  void PartDataInMultipartReceived(base::span<const char> bytes) override {
    if (!current_entry_.AppendBytes(bytes)) {
      multipart_parser_->Cancel();
    }
  }

  void PartDataInMultipartFullyReceived() override {
    if (!current_entry_.Finish(*form_data_)) {
      multipart_parser_->Cancel();
    }
  }

  void Fail() {
    finished_ = true;
    client_->DidFetchDataLoadFailed();
  }

  // The part that the parser is in.
  class Entry {
   public:
    // False for a part that is not of type form-data or has no name.
    bool Initialize(const MultipartPartHeaders& header_fields) {
      const ParsedContentDisposition disposition(
          header_fields.content_disposition().value_or(u""));
      filename_ = disposition.Filename();
      name_ = disposition.ParameterValueForName(u"name");
      blob_data_.reset();
      text_.reset();
      if (disposition.Type() != u"form-data" || !name_) {
        return false;
      }
      if (filename_) {
        blob_data_.emplace();
        const std::u16string content_type =
            header_fields.content_type().value_or(u"text/plain");
        blob_data_->SetContentType(IsValidBlobType(content_type)
                                       ? base::UTF16ToASCII(content_type)
                                       : std::string());
      } else {
        if (!decoder_) {
          decoder_ = TextCodecUtf8::Create("UTF-8");
        }
        text_.emplace();
      }
      return true;
    }

    // False once the bytes of a part without a file name are not UTF-8.
    bool AppendBytes(base::span<const char> bytes) {
      if (blob_data_) {
        blob_data_->AppendBytes(base::as_bytes(bytes));
      }
      if (text_) {
        bool saw_error = false;
        *text_ +=
            decoder_->Decode(base::as_bytes(bytes), FlushBehavior::kDoNotFlush,
                             /*stop_on_error=*/false, saw_error);
        if (saw_error) {
          return false;
        }
      }
      return true;
    }

    bool Finish(FormData& form_data) {
      if (blob_data_) {
        DCHECK(!text_);
        // Of no known time, as Blink makes it.
        File* const file =
            File::Create(*filename_, std::u16string(), std::nullopt,
                         BlobDataHandle::Create(std::move(*blob_data_)));
        blob_data_.reset();
        form_data.Append(*name_, file, filename_);
        return true;
      }
      DCHECK(text_);
      bool saw_error = false;
      *text_ += decoder_->Decode({}, FlushBehavior::kFetchEof,
                                 /*stop_on_error=*/false, saw_error);
      if (saw_error) {
        return false;
      }
      form_data.Append(*name_, *text_);
      text_.reset();
      return true;
    }

   private:
    std::optional<BlobData> blob_data_;
    std::optional<std::u16string> filename_;
    std::optional<std::u16string> name_;
    std::optional<std::u16string> text_;
    std::unique_ptr<TextCodec> decoder_;
  };

  const std::string boundary_;
  bool finished_ = false;
  cppgc::Member<BytesConsumer> consumer_;
  cppgc::Member<FetchDataLoader::Client> client_;
  cppgc::Member<FormData> form_data_;
  cppgc::Member<MultipartParser> multipart_parser_;
  Entry current_entry_;
};

}  // namespace

void FetchDataLoader::Client::DidFetchDataLoadedArrayBuffer(std::string bytes) {
  NOTREACHED();
}

void FetchDataLoader::Client::DidFetchDataLoadedString(std::u16string string) {
  NOTREACHED();
}

void FetchDataLoader::Client::DidFetchDataLoadedFormData(FormData* form_data) {
  NOTREACHED();
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsArrayBuffer() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsBytes>(
      LoadedAs::kArrayBuffer);
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsString() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsBytes>(
      LoadedAs::kString);
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsStringKeepingByteOrderMark() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsBytes>(
      LoadedAs::kStringKeepingByteOrderMark);
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsFormData(std::string boundary) {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsFormData>(
      std::move(boundary));
}

// static
FetchDataLoader* FetchDataLoader::CreateLoaderAsFailure() {
  return bindings::MakeGarbageCollected<FetchDataLoaderAsFailure>();
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
