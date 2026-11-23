#include "mitto/dom/file_reader_loader.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_view_util.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/window_task.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// Blink's kBlobMinDataPipeCapacity and kBlobDefaultDataPipeCapacity.
constexpr size_t kMinDataPipeCapacity = 1024;
constexpr size_t kDefaultDataPipeCapacity = 2 * 1024 * 1024;

std::u16string ToBinaryString(std::string_view bytes) {
  std::u16string result;
  result.reserve(bytes.size());
  for (const char c : bytes) {
    result.push_back(static_cast<unsigned char>(c));
  }
  return result;
}

std::u16string ToTextString(std::string_view bytes,
                            const std::u16string& label) {
  if (bytes.empty()) {
    return std::u16string();
  }
  // The File API says to use the encoding given, but Blink lets a byte order
  // mark win, as it does for web content.
  std::string encoding = CanonicalTextEncodingName(label);
  if (encoding.empty()) {
    encoding = "UTF-8";
  }
  TextResourceDecoder decoder(TextResourceDecoderOptions(
      TextResourceDecoderOptions::ContentType::kPlainText, encoding));
  std::u16string text = decoder.Decode(base::as_byte_span(bytes));
  text += decoder.Flush();
  return text;
}

std::u16string ToDataUrl(std::string_view bytes, std::string_view type) {
  // Firefox's default for an unknown type, which Blink matches
  // (https://crbug.com/48368).
  const std::string url =
      base::StrCat({"data:", type.empty() ? "application/octet-stream" : type,
                    ";base64,", base::Base64Encode(bytes)});
  return std::u16string(url.begin(), url.end());
}

}  // namespace

FileErrorCode FileReaderAccumulator::DidStartLoading(uint64_t total_bytes) {
  bytes_.clear();
  if (!base::IsValueInRangeForNumericType<size_t>(total_bytes)) {
    return FileErrorCode::kNotReadableErr;
  }
  total_bytes_ = total_bytes;
  bytes_.reserve(static_cast<size_t>(total_bytes));
  return DidStartLoading();
}

FileErrorCode FileReaderAccumulator::DidReceiveData(
    base::span<const uint8_t> data) {
  if (bytes_.size() + data.size() > total_bytes_) {
    bytes_.clear();
    return FileErrorCode::kNotReadableErr;
  }
  bytes_.append(base::as_string_view(data));
  return DidReceiveData();
}

FileErrorCode FileReaderAccumulator::DidStartLoading() {
  return FileErrorCode::kOK;
}

FileErrorCode FileReaderAccumulator::DidReceiveData() {
  return FileErrorCode::kOK;
}

void FileReaderAccumulator::DidFinishLoading() {
  DCHECK_EQ(bytes_.size(), total_bytes_);
  DidFinishLoading(std::move(bytes_));
}

void FileReaderAccumulator::DidFail(FileErrorCode code) {
  bytes_.clear();
}

FileReaderLoader::FileReaderLoader(FileReaderClient* client,
                                   ExecutionContext* context)
    : client_(client), context_(context) {
  CHECK(client);
}

FileReaderLoader::~FileReaderLoader() = default;

void FileReaderLoader::Start(scoped_refptr<BlobDataHandle> blob) {
  DCHECK(!blob_);
  pipe_capacity_ = std::max(kMinDataPipeCapacity,
                            static_cast<size_t>(std::min<uint64_t>(
                                blob->size(), kDefaultDataPipeCapacity)));
  blob_ = std::move(blob);
  PostTaskInContext(context_.Get(), TaskType::kFileReading,
                    base::BindOnce(
                        [](cppgc::Persistent<FileReaderLoader> self) {
                          self->OnCalculatedSize();
                        },
                        cppgc::Persistent<FileReaderLoader>(this)));
}

void FileReaderLoader::Cancel() {
  cancelled_ = true;
  blob_ = nullptr;
}

void FileReaderLoader::OnCalculatedSize() {
  if (cancelled_) {
    return;
  }
  total_bytes_ = blob_->size();
  if (const FileErrorCode error = client_->DidStartLoading(*total_bytes_);
      error != FileErrorCode::kOK) {
    Failed(error);
    return;
  }
  // The bytes come once the pipe has them, and an empty blob's end comes
  // after its size, as the browser's OnComplete() does.
  PostTaskInContext(context_.Get(), TaskType::kFileReading,
                    base::BindOnce(
                        [](cppgc::Persistent<FileReaderLoader> self) {
                          self->OnDataPipeReadable();
                        },
                        cppgc::Persistent<FileReaderLoader>(this)));
}

void FileReaderLoader::OnDataPipeReadable() {
  if (cancelled_) {
    return;
  }
  const base::span<const uint8_t> rest =
      blob_->Bytes().subspan(static_cast<size_t>(bytes_loaded_));
  if (!rest.empty()) {
    const base::span<const uint8_t> piece =
        rest.first(std::min(rest.size(), pipe_capacity_));
    bytes_loaded_ += piece.size();
    if (const FileErrorCode error = client_->DidReceiveData(piece);
        error != FileErrorCode::kOK) {
      Failed(error);
      return;
    }
    if (cancelled_) {
      return;
    }
    if (bytes_loaded_ < *total_bytes_) {
      PostTaskInContext(context_.Get(), TaskType::kFileReading,
                        base::BindOnce(
                            [](cppgc::Persistent<FileReaderLoader> self) {
                              self->OnDataPipeReadable();
                            },
                            cppgc::Persistent<FileReaderLoader>(this)));
      return;
    }
  }
  finished_loading_ = true;
  blob_ = nullptr;
  client_->DidFinishLoading();
}

void FileReaderLoader::Failed(FileErrorCode code) {
  if (cancelled_) {
    return;
  }
  cancelled_ = true;
  blob_ = nullptr;
  client_->DidFail(code);
}

void FileReaderLoader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(client_);
  visitor->Trace(context_);
}

std::u16string FileReaderResultAsString(std::string_view bytes,
                                        FileReadType read_type,
                                        const std::u16string& encoding,
                                        std::string_view type) {
  switch (read_type) {
    case FileReadType::kReadAsBinaryString:
      return ToBinaryString(bytes);
    case FileReadType::kReadAsText:
      return ToTextString(bytes, encoding);
    case FileReadType::kReadAsDataURL:
      return ToDataUrl(bytes, type);
    case FileReadType::kReadAsArrayBuffer:
    case FileReadType::kReadAsBytes:
      break;
  }
  NOTREACHED();
}

}  // namespace mitto
