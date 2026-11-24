#include "mitto/dom/form_data_encoder.h"

#include <stdint.h>

#include <array>

#include "base/rand_util.h"
#include "base/strings/strcat.h"

namespace mitto::form_data_encoder {
namespace {

// FormDataEncoder's Mode.
enum class Mode {
  kNormalizeCRLF,
  kDoNotNormalizeCRLF,
};

// A string as a quoted value, its quotes and line breaks escaped.
void AppendQuotedString(std::string& buffer,
                        std::string_view string,
                        Mode mode) {
  const size_t length = string.length();
  for (size_t i = 0; i < length; ++i) {
    const char c = string[i];
    switch (c) {
      case 0x0a:
        buffer += mode == Mode::kNormalizeCRLF ? "%0D%0A" : "%0A";
        break;
      case 0x0d:
        if (mode == Mode::kNormalizeCRLF) {
          buffer += "%0D%0A";
          if (i + 1 < length && string[i + 1] == 0x0a) {
            ++i;
          }
        } else {
          buffer += "%0D";
        }
        break;
      case '"':
        buffer += "%22";
        break;
      default:
        buffer += c;
    }
  }
}

}  // namespace

std::string GenerateUniqueBoundaryString() {
  // Twice as likely an 'A' or a 'B', as Blink's map has them twice.
  static constexpr std::array<uint8_t, 64> kAlphaNumericEncodingMap = {
      0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
      0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
      0x57, 0x58, 0x59, 0x5A, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
      0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72,
      0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x30, 0x31, 0x32,
      0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x41, 0x42};
  std::array<uint8_t, 16> random_bytes;
  base::RandBytes(random_bytes);
  std::string boundary = "----WebKitFormBoundary";
  for (const uint8_t c : random_bytes) {
    boundary += static_cast<char>(kAlphaNumericEncodingMap[c & 0x3F]);
  }
  return boundary;
}

void AddBoundaryToMultiPartHeader(std::string& buffer,
                                  std::string_view boundary,
                                  bool is_last_boundary) {
  base::StrAppend(&buffer, {"--", boundary});
  if (is_last_boundary) {
    buffer += "--";
  }
  buffer += "\r\n";
}

void BeginMultiPartHeader(std::string& buffer,
                          std::string_view boundary,
                          std::string_view name) {
  AddBoundaryToMultiPartHeader(buffer, boundary);
  buffer += "Content-Disposition: form-data; name=\"";
  AppendQuotedString(buffer, name, Mode::kNormalizeCRLF);
  buffer += '"';
}

void AddFilenameToMultiPartHeader(std::string& buffer,
                                  std::string_view filename) {
  buffer += "; filename=\"";
  AppendQuotedString(buffer, filename, Mode::kDoNotNormalizeCRLF);
  buffer += '"';
}

void AddContentTypeToMultiPartHeader(std::string& buffer,
                                     std::string_view mime_type) {
  base::StrAppend(&buffer, {"\r\nContent-Type: ", mime_type});
}

void FinishMultiPartHeader(std::string& buffer) {
  buffer += "\r\n\r\n";
}

}  // namespace mitto::form_data_encoder
