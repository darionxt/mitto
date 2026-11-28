#include "mitto/dom/blob_data.h"

#include <utility>

#include "base/check_op.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"

namespace mitto {
namespace {

// Blink's NormalizeToCrLf(): a CR alone and an LF alone each become CRLF, and
// a CRLF stays.
void AppendWithCrLf(std::string_view text, std::string& out) {
  for (size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '\r') {
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        continue;
      }
      out.append("\r\n");
    } else if (c == '\n') {
      out.append("\r\n");
    } else {
      out.push_back(c);
    }
  }
}

}  // namespace

bool IsValidBlobType(std::u16string_view type) {
  for (const char16_t c : type) {
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

bool IsValidBlobType(std::string_view type) {
  for (const char c : type) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte > 0x7E) {
      return false;
    }
  }
  return true;
}

BlobData::Element::Element() = default;
BlobData::Element::Element(Element&&) = default;
BlobData::Element& BlobData::Element::operator=(Element&&) = default;
BlobData::Element::~Element() = default;

BlobData::BlobData() = default;
BlobData::BlobData(BlobData&&) = default;
BlobData& BlobData::operator=(BlobData&&) = default;
BlobData::~BlobData() = default;

void BlobData::SetContentType(std::string_view type) {
  if (IsValidBlobType(type)) {
    content_type_ = std::string(type);
  } else {
    content_type_.clear();
  }
}

void BlobData::AppendText(std::u16string_view text,
                          bool normalize_line_endings_to_native) {
  const std::string utf8 = base::UTF16ToUTF8(text);
  if (!normalize_line_endings_to_native) {
    AppendBytes(base::as_byte_span(utf8));
    return;
  }
  std::string normalized;
  normalized.reserve(utf8.size());
  AppendWithCrLf(utf8, normalized);
  AppendBytes(base::as_byte_span(normalized));
}

void BlobData::AppendBytes(base::span<const uint8_t> bytes) {
  // Zero bytes change nothing of the blob, as Blink skips them.
  if (bytes.empty()) {
    return;
  }
  if (elements_.empty() || elements_.back().blob) {
    elements_.emplace_back();
  }
  elements_.back().bytes.append(base::as_string_view(bytes));
}

void BlobData::AppendBlob(scoped_refptr<BlobDataHandle> handle,
                          uint64_t offset,
                          uint64_t length) {
  DCHECK_LE(offset + length, handle->size());
  if (length == 0) {
    return;
  }
  Element element;
  element.blob = std::move(handle);
  element.offset = offset;
  element.length = length;
  elements_.push_back(std::move(element));
}

uint64_t BlobData::length() const {
  uint64_t length = 0;
  for (const Element& element : elements_) {
    length += element.blob ? element.length : element.bytes.size();
  }
  return length;
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::Create(BlobData data) {
  if (data.elements_.size() == 1 && data.elements_[0].blob) {
    // A slice of one blob shares its bytes.
    const BlobData::Element& element = data.elements_[0];
    return Create(
        element.blob->storage_,
        element.blob->offset_ + base::checked_cast<size_t>(element.offset),
        base::checked_cast<size_t>(element.length),
        std::move(data.content_type_));
  }
  std::string bytes;
  bytes.reserve(base::checked_cast<size_t>(data.length()));
  for (const BlobData::Element& element : data.elements_) {
    if (element.blob) {
      bytes.append(base::as_string_view(element.blob->Bytes().subspan(
          base::checked_cast<size_t>(element.offset),
          base::checked_cast<size_t>(element.length))));
    } else {
      bytes.append(element.bytes);
    }
  }
  return Create(std::move(bytes), std::move(data.content_type_));
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::Create(std::string bytes,
                                                     std::string type) {
  const size_t size = bytes.size();
  return Create(base::MakeRefCounted<base::RefCountedString>(std::move(bytes)),
                0, size, std::move(type));
}

// static
scoped_refptr<BlobDataHandle> BlobDataHandle::Create(
    scoped_refptr<const base::RefCountedString> storage,
    size_t offset,
    size_t size,
    std::string type) {
  return base::WrapRefCounted(
      new BlobDataHandle(std::move(storage), offset, size, std::move(type)));
}

BlobDataHandle::BlobDataHandle(
    scoped_refptr<const base::RefCountedString> storage,
    size_t offset,
    size_t size,
    std::string type)
    : uuid_(base::Uuid::GenerateRandomV4().AsLowercaseString()),
      type_(std::move(type)),
      storage_(std::move(storage)),
      offset_(offset),
      size_(size) {
  DCHECK_LE(offset_ + size_, storage_->as_string().size());
}

BlobDataHandle::~BlobDataHandle() = default;

base::span<const uint8_t> BlobDataHandle::Bytes() const {
  return base::as_byte_span(storage_->as_string()).subspan(offset_, size_);
}

}  // namespace mitto
