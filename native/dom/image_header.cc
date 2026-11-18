#include "mitto/dom/image_header.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_math_expression.h"
#include "mitto/css/css_token.h"
#include "mitto/dom/svg_length.h"

namespace mitto {
namespace {

// Reads integers of either byte order out of `data`, and nothing past it.
class ByteReader {
 public:
  explicit ByteReader(std::string_view data, bool big_endian = true)
      : data_(data), big_endian_(big_endian) {}

  size_t size() const { return data_.size(); }
  bool Has(size_t offset, size_t length) const {
    return offset <= data_.size() && length <= data_.size() - offset;
  }
  uint8_t U8(size_t offset) const {
    return Has(offset, 1) ? static_cast<uint8_t>(data_[offset]) : 0;
  }
  uint16_t U16(size_t offset) const {
    return big_endian_ ? (U8(offset) << 8) | U8(offset + 1)
                       : U8(offset) | (U8(offset + 1) << 8);
  }
  uint32_t U24LE(size_t offset) const {
    return U8(offset) | (U8(offset + 1) << 8) | (U8(offset + 2) << 16);
  }
  uint32_t U32(size_t offset) const {
    const uint32_t a = U16(offset);
    const uint32_t b = U16(offset + 2);
    return big_endian_ ? (a << 16) | b : a | (b << 16);
  }
  std::string_view Bytes(size_t offset, size_t length) const {
    return Has(offset, length) ? data_.substr(offset, length)
                               : std::string_view();
  }

 private:
  std::string_view data_;
  bool big_endian_;
};

// What Skia's SkExif::Parse() reads of a TIFF structure of EXIF metadata.
struct ExifMetadata {
  // 1 to 8, as EXIF's Orientation; 1 for none.
  int origin = 1;
  std::optional<int> resolution_unit;
  std::optional<float> x_resolution;
  std::optional<float> y_resolution;
  std::optional<uint32_t> pixel_x_dimension;
  std::optional<uint32_t> pixel_y_dimension;
};

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong = 4;
constexpr uint16_t kTypeRational = 5;

void ReadIfd(const ByteReader& tiff,
             uint32_t offset,
             ExifMetadata& metadata,
             bool is_exif_ifd,
             int depth) {
  if (depth > 2 || !tiff.Has(offset, 2)) {
    return;
  }
  const uint16_t count = tiff.U16(offset);
  for (uint16_t i = 0; i < count; ++i) {
    const size_t entry = offset + 2 + i * 12u;
    if (!tiff.Has(entry, 12)) {
      return;
    }
    const uint16_t tag = tiff.U16(entry);
    const uint16_t type = tiff.U16(entry + 2);
    const uint32_t components = tiff.U32(entry + 4);
    if (components != 1) {
      continue;
    }
    auto integer = [&]() -> std::optional<uint32_t> {
      if (type == kTypeShort) {
        return tiff.U16(entry + 8);
      }
      if (type == kTypeLong) {
        return tiff.U32(entry + 8);
      }
      return std::nullopt;
    };
    auto rational = [&]() -> std::optional<float> {
      if (type != kTypeRational) {
        return std::nullopt;
      }
      const uint32_t at = tiff.U32(entry + 8);
      if (!tiff.Has(at, 8) || !tiff.U32(at + 4)) {
        return std::nullopt;
      }
      return static_cast<float>(tiff.U32(at)) /
             static_cast<float>(tiff.U32(at + 4));
    };
    if (is_exif_ifd) {
      if (tag == 0xA002) {
        metadata.pixel_x_dimension = integer();
      } else if (tag == 0xA003) {
        metadata.pixel_y_dimension = integer();
      }
      continue;
    }
    switch (tag) {
      case 0x0112:
        if (const std::optional<uint32_t> origin = integer();
            origin && *origin >= 1 && *origin <= 8) {
          metadata.origin = static_cast<int>(*origin);
        }
        break;
      case 0x011A:
        metadata.x_resolution = rational();
        break;
      case 0x011B:
        metadata.y_resolution = rational();
        break;
      case 0x0128:
        if (const std::optional<uint32_t> unit = integer()) {
          metadata.resolution_unit = static_cast<int>(*unit);
        }
        break;
      case 0x8769:
        if (const std::optional<uint32_t> exif = integer()) {
          ReadIfd(tiff, *exif, metadata, /*is_exif_ifd=*/true, depth + 1);
        }
        break;
      default:
        break;
    }
  }
}

ExifMetadata ParseExif(std::string_view tiff_data) {
  ExifMetadata metadata;
  if (tiff_data.size() < 8) {
    return metadata;
  }
  bool big_endian;
  if (tiff_data.starts_with("MM")) {
    big_endian = true;
  } else if (tiff_data.starts_with("II")) {
    big_endian = false;
  } else {
    return metadata;
  }
  const ByteReader tiff(tiff_data, big_endian);
  if (tiff.U16(2) != 42) {
    return metadata;
  }
  ReadIfd(tiff, tiff.U32(4), metadata, /*is_exif_ifd=*/false, 0);
  return metadata;
}

// The size of a bitmap of `physical` pixels as BitmapImage gives it: density
// corrected as ExtractDensityCorrectedSize() works it out, then turned for an
// orientation that swaps width and height.
std::optional<ImageHeader> BitmapHeader(int width,
                                        int height,
                                        const ExifMetadata& metadata) {
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  constexpr float kDefaultResolution = 72;
  constexpr int kResolutionUnitDpi = 2;
  int corrected_width = width;
  int corrected_height = height;
  const float x_resolution = metadata.x_resolution.value_or(0);
  const float y_resolution = metadata.y_resolution.value_or(0);
  const int pixel_width =
      static_cast<int>(metadata.pixel_x_dimension.value_or(0));
  const int pixel_height =
      static_cast<int>(metadata.pixel_y_dimension.value_or(0));
  if (metadata.resolution_unit == kResolutionUnitDpi && x_resolution > 0 &&
      y_resolution > 0 && pixel_width > 0 && pixel_height > 0) {
    const int from_resolution_width =
        static_cast<int>(std::round(width * kDefaultResolution / x_resolution));
    const int from_resolution_height = static_cast<int>(
        std::round(height * kDefaultResolution / y_resolution));
    if (from_resolution_width == pixel_width &&
        from_resolution_height == pixel_height) {
      corrected_width = pixel_width;
      corrected_height = pixel_height;
    }
  }
  // Orientations 5 to 8 turn the image a quarter.
  if (metadata.origin >= 5) {
    std::swap(corrected_width, corrected_height);
  }
  ImageHeader header;
  header.width = corrected_width;
  header.height = corrected_height;
  return header;
}

std::optional<ImageHeader> ReadJpeg(const ByteReader& reader) {
  ExifMetadata metadata;
  size_t offset = 2;
  while (reader.Has(offset, 4)) {
    if (reader.U8(offset) != 0xFF) {
      return std::nullopt;
    }
    const uint8_t marker = reader.U8(offset + 1);
    if (marker == 0xFF) {
      ++offset;
      continue;
    }
    // Markers with no length.
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      offset += 2;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) {
      return std::nullopt;
    }
    const uint16_t length = reader.U16(offset + 2);
    if (length < 2) {
      return std::nullopt;
    }
    const size_t segment = offset + 4;
    if (marker == 0xE1 && length >= 8 &&
        reader.Bytes(segment, 6) == std::string_view("Exif\0\0", 6) &&
        metadata.origin == 1 && !metadata.resolution_unit) {
      metadata = ParseExif(reader.Bytes(segment + 6, length - 2u - 6u));
    }
    // The start of a frame, but for DHT, JPG and DAC.
    const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                        marker != 0xC8 && marker != 0xCC;
    if (is_sof) {
      if (!reader.Has(segment, 5)) {
        return std::nullopt;
      }
      return BitmapHeader(reader.U16(segment + 3), reader.U16(segment + 1),
                          metadata);
    }
    offset = segment + length - 2;
  }
  return std::nullopt;
}

std::optional<ImageHeader> ReadPng(const ByteReader& reader) {
  if (reader.Bytes(12, 4) != "IHDR" || reader.U32(8) != 13) {
    return std::nullopt;
  }
  const uint32_t width = reader.U32(16);
  const uint32_t height = reader.U32(20);
  // libpng's limit.
  if (width > 0x7FFFFFFF || height > 0x7FFFFFFF) {
    return std::nullopt;
  }
  ExifMetadata metadata;
  size_t offset = 33;
  while (reader.Has(offset, 8)) {
    const uint32_t length = reader.U32(offset);
    const std::string_view type = reader.Bytes(offset + 4, 4);
    if (type == "IDAT") {
      break;
    }
    if (type == "eXIf") {
      metadata = ParseExif(reader.Bytes(offset + 8, length));
      break;
    }
    if (length > reader.size()) {
      break;
    }
    offset += 12u + length;
  }
  return BitmapHeader(static_cast<int>(width), static_cast<int>(height),
                      metadata);
}

std::optional<ImageHeader> ReadGif(const ByteReader& little) {
  return BitmapHeader(little.U16(6), little.U16(8), ExifMetadata());
}

std::optional<ImageHeader> ReadWebP(const ByteReader& little) {
  const std::string_view chunk = little.Bytes(12, 4);
  if (chunk == "VP8 ") {
    if (little.Bytes(23, 3) != "\x9d\x01\x2a") {
      return std::nullopt;
    }
    return BitmapHeader(little.U16(26) & 0x3FFF, little.U16(28) & 0x3FFF,
                        ExifMetadata());
  }
  if (chunk == "VP8L") {
    if (little.U8(20) != 0x2F) {
      return std::nullopt;
    }
    const uint32_t bits = little.U32(21);
    return BitmapHeader(static_cast<int>((bits & 0x3FFF) + 1),
                        static_cast<int>(((bits >> 14) & 0x3FFF) + 1),
                        ExifMetadata());
  }
  if (chunk == "VP8X") {
    const uint8_t flags = little.U8(20);
    const int width = static_cast<int>(little.U24LE(24) + 1);
    const int height = static_cast<int>(little.U24LE(27) + 1);
    ExifMetadata metadata;
    if (flags & 0x08) {
      size_t offset = 30;
      while (little.Has(offset, 8)) {
        const uint32_t length = little.U32(offset + 4);
        if (little.Bytes(offset, 4) == "EXIF") {
          std::string_view exif = little.Bytes(offset + 8, length);
          // Some writers keep JPEG's "Exif\0\0" in front of it.
          if (exif.starts_with(std::string_view("Exif\0\0", 6))) {
            exif.remove_prefix(6);
          }
          metadata = ParseExif(exif);
          break;
        }
        if (length > little.size()) {
          break;
        }
        offset += 8u + length + (length & 1);
      }
    }
    return BitmapHeader(width, height, metadata);
  }
  return std::nullopt;
}

// ICOImageDecoder: the largest image of the directory, and of two the same
// size, the one of more bits.
std::optional<ImageHeader> ReadIco(const ByteReader& little) {
  const uint16_t count = little.U16(4);
  int best_width = 0;
  int best_height = 0;
  int best_bits = -1;
  for (uint16_t i = 0; i < count; ++i) {
    const size_t entry = 6 + i * 16u;
    if (!little.Has(entry, 16)) {
      break;
    }
    const int width = little.U8(entry) ? little.U8(entry) : 256;
    const int height = little.U8(entry + 1) ? little.U8(entry + 1) : 256;
    const int bits = little.U16(entry + 6);
    const int64_t area = int64_t{width} * height;
    const int64_t best_area = int64_t{best_width} * best_height;
    if (area > best_area || (area == best_area && bits > best_bits)) {
      best_width = width;
      best_height = height;
      best_bits = bits;
    }
  }
  if (!best_width) {
    return std::nullopt;
  }
  return BitmapHeader(best_width, best_height, ExifMetadata());
}

std::optional<ImageHeader> ReadBmp(const ByteReader& little) {
  if (little.Bytes(0, 2) != "BM") {
    return std::nullopt;
  }
  const uint32_t header_size = little.U32(14);
  if (header_size == 12) {
    return BitmapHeader(little.U16(18), little.U16(20), ExifMetadata());
  }
  if (header_size < 16 || !little.Has(14, 12)) {
    return std::nullopt;
  }
  const int32_t width = static_cast<int32_t>(little.U32(18));
  const int32_t height = static_cast<int32_t>(little.U32(22));
  if (height == INT32_MIN) {
    return std::nullopt;
  }
  return BitmapHeader(width, std::abs(height), ExifMetadata());
}

// An ISO BMFF box: where its content starts and how long it is.
struct Box {
  std::string_view type;
  size_t content;
  size_t length;
};

std::optional<Box> ReadBox(const ByteReader& reader,
                           size_t offset,
                           size_t end) {
  if (offset >= end || !reader.Has(offset, 8)) {
    return std::nullopt;
  }
  uint64_t size = reader.U32(offset);
  size_t header = 8;
  if (size == 1) {
    if (!reader.Has(offset + 8, 8)) {
      return std::nullopt;
    }
    size = (uint64_t{reader.U32(offset + 8)} << 32) | reader.U32(offset + 12);
    header = 16;
  } else if (size == 0) {
    size = end - offset;
  }
  if (size < header || size > end - offset) {
    return std::nullopt;
  }
  return Box{reader.Bytes(offset + 4, 4), offset + header,
             static_cast<size_t>(size) - header};
}

// crabbyavif's avifPeekCompatibleFileType(): an ftyp box whose major brand
// or one of its first 32 compatible brands is avif or avis.
bool MatchesAvifSignature(const ByteReader& reader) {
  const std::optional<Box> ftyp =
      ReadBox(reader, 0, std::min<size_t>(reader.size(), 144));
  if (!ftyp || ftyp->type != "ftyp" || ftyp->length < 8) {
    return false;
  }
  auto is_avif = [](std::string_view brand) {
    return brand == "avif" || brand == "avis";
  };
  if (is_avif(reader.Bytes(ftyp->content, 4))) {
    return true;
  }
  const size_t brands = std::min<size_t>((ftyp->length - 8) / 4, 32);
  for (size_t i = 0; i < brands; ++i) {
    if (is_avif(reader.Bytes(ftyp->content + 8 + i * 4, 4))) {
      return true;
    }
  }
  return false;
}

// The primary item's ispe property, turned by its irot, as AVIFImageDecoder
// gives the size of a still image.
std::optional<ImageHeader> ReadAvif(const ByteReader& reader) {
  size_t offset = 0;
  std::optional<Box> meta;
  while (std::optional<Box> box = ReadBox(reader, offset, reader.size())) {
    if (box->type == "meta") {
      meta = box;
      break;
    }
    offset = box->content + box->length;
  }
  if (!meta || meta->length < 4) {
    return std::nullopt;
  }
  // meta is a full box: four bytes of version and flags.
  const size_t meta_end = meta->content + meta->length;
  uint32_t primary = 0;
  std::optional<Box> iprp;
  for (size_t at = meta->content + 4;
       std::optional<Box> box = ReadBox(reader, at, meta_end);
       at = box->content + box->length) {
    if (box->type == "pitm") {
      primary = reader.U8(box->content) == 0 ? reader.U16(box->content + 4)
                                             : reader.U32(box->content + 4);
    } else if (box->type == "iprp") {
      iprp = box;
    }
  }
  if (!iprp) {
    return std::nullopt;
  }
  const size_t iprp_end = iprp->content + iprp->length;
  std::vector<Box> properties;
  std::optional<Box> ipma;
  for (size_t at = iprp->content;
       std::optional<Box> box = ReadBox(reader, at, iprp_end);
       at = box->content + box->length) {
    if (box->type == "ipco") {
      const size_t ipco_end = box->content + box->length;
      for (size_t p = box->content;
           std::optional<Box> property = ReadBox(reader, p, ipco_end);
           p = property->content + property->length) {
        properties.push_back(*property);
      }
    } else if (box->type == "ipma") {
      ipma = box;
    }
  }
  if (!ipma) {
    return std::nullopt;
  }
  const uint8_t version = reader.U8(ipma->content);
  const bool large_index = reader.U8(ipma->content + 3) & 1;
  const uint32_t entries = reader.U32(ipma->content + 4);
  size_t at = ipma->content + 8;
  const size_t ipma_end = ipma->content + ipma->length;
  std::optional<std::pair<uint32_t, uint32_t>> size;
  int rotation = 0;
  for (uint32_t i = 0; i < entries && at < ipma_end; ++i) {
    uint32_t item;
    if (version < 1) {
      item = reader.U16(at);
      at += 2;
    } else {
      item = reader.U32(at);
      at += 4;
    }
    const uint8_t associations = reader.U8(at);
    ++at;
    for (uint8_t j = 0; j < associations && at < ipma_end; ++j) {
      uint32_t index;
      if (large_index) {
        index = reader.U16(at) & 0x7FFF;
        at += 2;
      } else {
        index = reader.U8(at) & 0x7F;
        at += 1;
      }
      if (item != primary || index == 0 || index > properties.size()) {
        continue;
      }
      const Box& property = properties[index - 1];
      if (property.type == "ispe") {
        size.emplace(reader.U32(property.content + 4),
                     reader.U32(property.content + 8));
      } else if (property.type == "irot") {
        rotation = reader.U8(property.content) & 3;
      }
    }
  }
  if (!size || size->first > 0x7FFFFFFF || size->second > 0x7FFFFFFF) {
    return std::nullopt;
  }
  ExifMetadata metadata;
  // A quarter or three quarters of a turn swaps width and height.
  if (rotation == 1 || rotation == 3) {
    metadata.origin = 6;
  }
  return BitmapHeader(static_cast<int>(size->first),
                      static_cast<int>(size->second), metadata);
}

// Reads JPEG XL's codestream header, least significant bit first.
class BitReader {
 public:
  explicit BitReader(std::string_view data) : data_(data) {}

  uint32_t Read(int bits) {
    uint32_t value = 0;
    for (int i = 0; i < bits; ++i) {
      const size_t byte = position_ / 8;
      if (byte >= data_.size()) {
        overrun_ = true;
        return 0;
      }
      value |= ((static_cast<uint8_t>(data_[byte]) >> (position_ % 8)) & 1u)
               << i;
      ++position_;
    }
    return value;
  }
  bool overrun() const { return overrun_; }

 private:
  std::string_view data_;
  size_t position_ = 0;
  bool overrun_ = false;
};

// SizeHeader's U32 of Bits(9) + 1, Bits(13) + 1, Bits(18) + 1, Bits(30) + 1.
uint32_t ReadJxlDimension(BitReader& bits) {
  static constexpr std::array<int, 4> kBits = {9, 13, 18, 30};
  return bits.Read(kBits[bits.Read(2)]) + 1;
}

// JPEG XL's SizeHeader and the orientation of its ImageMetadata.
std::optional<ImageHeader> ReadJxlCodestream(std::string_view codestream) {
  if (!codestream.starts_with("\xFF\x0A")) {
    return std::nullopt;
  }
  BitReader bits(codestream.substr(2));
  uint64_t width;
  uint64_t height;
  const bool small = bits.Read(1);
  height = small ? (bits.Read(5) + 1) * 8 : ReadJxlDimension(bits);
  const uint32_t ratio = bits.Read(3);
  static constexpr std::array<std::array<uint32_t, 2>, 8> kRatios = {
      {{0, 0}, {1, 1}, {12, 10}, {4, 3}, {3, 2}, {16, 9}, {5, 4}, {2, 1}}};
  if (ratio) {
    width = height * kRatios[ratio][0] / kRatios[ratio][1];
  } else {
    width = small ? (bits.Read(5) + 1) * 8 : ReadJxlDimension(bits);
  }
  ExifMetadata metadata;
  // ImageMetadata: all_default, then extra_fields, then the orientation.
  if (!bits.Read(1) && bits.Read(1)) {
    metadata.origin = static_cast<int>(bits.Read(3)) + 1;
  }
  if (bits.overrun() || width > 0x3FFFFFFF || height > 0x3FFFFFFF) {
    return std::nullopt;
  }
  return BitmapHeader(static_cast<int>(width), static_cast<int>(height),
                      metadata);
}

std::optional<ImageHeader> ReadJxl(const ByteReader& reader,
                                   std::string_view body) {
  if (body.starts_with("\xFF\x0A")) {
    return ReadJxlCodestream(body);
  }
  if (!body.starts_with(std::string_view("\0\0\0\x0CJXL \r\n\x87\n", 12))) {
    return std::nullopt;
  }
  for (size_t at = 12;
       std::optional<Box> box = ReadBox(reader, at, body.size());
       at = box->content + box->length) {
    if (box->type == "jxlc") {
      return ReadJxlCodestream(body.substr(box->content, box->length));
    }
    if (box->type == "jxlp" && box->length > 4) {
      return ReadJxlCodestream(body.substr(box->content + 4, box->length - 4));
    }
  }
  return std::nullopt;
}

// An attribute of the root element, and its value with entities decoded.
struct XmlAttribute {
  std::string name;
  std::u16string value;
};

void SkipXmlSpace(std::string_view text, size_t& position) {
  while (position < text.size() &&
         (text[position] == ' ' || text[position] == '\t' ||
          text[position] == '\n' || text[position] == '\r')) {
    ++position;
  }
}

bool IsXmlNameChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' || c == '.' ||
         c == ':' || static_cast<unsigned char>(c) >= 0x80;
}

// XML's predefined entities and character references in an attribute
// value, which also turns its white space into spaces.
std::optional<std::u16string> DecodeAttributeValue(std::string_view raw) {
  std::string decoded;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '<') {
      return std::nullopt;
    }
    if (c == '\t' || c == '\n' || c == '\r') {
      decoded += ' ';
      continue;
    }
    if (c != '&') {
      decoded += c;
      continue;
    }
    const size_t end = raw.find(';', i);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view entity = raw.substr(i + 1, end - i - 1);
    if (entity == "amp") {
      decoded += '&';
    } else if (entity == "lt") {
      decoded += '<';
    } else if (entity == "gt") {
      decoded += '>';
    } else if (entity == "quot") {
      decoded += '"';
    } else if (entity == "apos") {
      decoded += '\'';
    } else if (entity.starts_with('#')) {
      uint32_t code_point;
      const bool hex = entity.size() > 1 && entity[1] == 'x';
      if (!(hex ? base::HexStringToUInt(entity.substr(2), &code_point)
                : base::StringToUint(entity.substr(1), &code_point))) {
        return std::nullopt;
      }
      if (!base::IsValidCodepoint(code_point)) {
        return std::nullopt;
      }
      base::WriteUnicodeCharacter(code_point, &decoded);
    } else {
      // Entities a DTD declares are a Known Gap.
      return std::nullopt;
    }
    i = end;
  }
  std::u16string value;
  if (!base::UTF8ToUTF16(decoded.data(), decoded.size(), &value)) {
    return std::nullopt;
  }
  return value;
}

// The root element of an XML document, as far as libxml reads it before
// anything after it: its qualified name and attributes.
std::optional<std::pair<std::string, std::vector<XmlAttribute>>>
ReadXmlRootElement(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  size_t position = 0;
  while (true) {
    SkipXmlSpace(text, position);
    const std::string_view rest = text.substr(position);
    if (rest.starts_with("<?")) {
      const size_t end = text.find("?>", position + 2);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      position = end + 2;
    } else if (rest.starts_with("<!--")) {
      const size_t end = text.find("-->", position + 4);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      position = end + 3;
    } else if (rest.starts_with("<!DOCTYPE")) {
      // To its end, past an internal subset in brackets.
      size_t depth = 0;
      size_t at = position + 9;
      for (; at < text.size(); ++at) {
        if (text[at] == '[') {
          ++depth;
        } else if (text[at] == ']' && depth) {
          --depth;
        } else if (text[at] == '>' && !depth) {
          break;
        }
      }
      if (at >= text.size()) {
        return std::nullopt;
      }
      position = at + 1;
    } else {
      break;
    }
  }
  if (position >= text.size() || text[position] != '<') {
    return std::nullopt;
  }
  ++position;
  const size_t name_start = position;
  while (position < text.size() && IsXmlNameChar(text[position])) {
    ++position;
  }
  std::string name(text.substr(name_start, position - name_start));
  if (name.empty()) {
    return std::nullopt;
  }
  std::vector<XmlAttribute> attributes;
  while (true) {
    const size_t before_space = position;
    SkipXmlSpace(text, position);
    if (position >= text.size()) {
      return std::nullopt;
    }
    if (text[position] == '>' || text.substr(position).starts_with("/>")) {
      break;
    }
    if (position == before_space) {
      return std::nullopt;
    }
    const size_t attribute_start = position;
    while (position < text.size() && IsXmlNameChar(text[position])) {
      ++position;
    }
    XmlAttribute attribute;
    attribute.name =
        std::string(text.substr(attribute_start, position - attribute_start));
    SkipXmlSpace(text, position);
    if (attribute.name.empty() || position >= text.size() ||
        text[position] != '=') {
      return std::nullopt;
    }
    ++position;
    SkipXmlSpace(text, position);
    if (position >= text.size() ||
        (text[position] != '"' && text[position] != '\'')) {
      return std::nullopt;
    }
    const char quote = text[position];
    const size_t value_end = text.find(quote, position + 1);
    if (value_end == std::string_view::npos) {
      return std::nullopt;
    }
    std::optional<std::u16string> value = DecodeAttributeValue(
        text.substr(position + 1, value_end - position - 1));
    if (!value) {
      return std::nullopt;
    }
    attribute.value = std::move(*value);
    for (const XmlAttribute& other : attributes) {
      if (other.name == attribute.name) {
        return std::nullopt;
      }
    }
    attributes.push_back(std::move(attribute));
    position = value_end + 1;
  }
  return std::make_pair(std::move(name), std::move(attributes));
}

constexpr char16_t kSvgNamespace[] = u"http://www.w3.org/2000/svg";

const std::u16string* FindAttribute(const std::vector<XmlAttribute>& attributes,
                                    std::string_view name) {
  for (const XmlAttribute& attribute : attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

// SVGSVGElement::IntrinsicWidth(): the length in user units, none for a
// percentage, or for a value that does not parse or is negative, which
// leaves the initial 100%. Relative units resolve against the initial font
// and the default object size, as an image's document has them.
std::optional<float> IntrinsicLength(const std::u16string* attribute) {
  if (!attribute) {
    return std::nullopt;
  }
  const std::optional<SVGLengthValue> length =
      SVGLengthValue::Parse(*attribute);
  if (!length || length->IsUnparsed()) {
    return std::nullopt;
  }
  CssLengthResolver resolver;
  resolver.viewport_width = 300;
  resolver.viewport_height = 150;
  if (!length->IsNumeric()) {
    return std::max(0.0f, static_cast<float>(length->ComputeMath(resolver)));
  }
  if (length->Unit() == CssUnit::kPercentage || length->Number() < 0) {
    return std::nullopt;
  }
  const CssUnit unit =
      length->Unit() == CssUnit::kNumber ? CssUnit::kPixels : length->Unit();
  return std::max(0.0f, static_cast<float>(
                            resolver.ComputedPixels(length->Number(), unit)));
}

// The text of an SVG document in UTF-8: decoded from UTF-16 that a byte order
// mark, or the "<?" of an XML declaration, gives away, as libxml's
// xmlDetectCharEncoding() finds it, and otherwise as it came, since the other
// encodings that libxml takes write the root element's ASCII as ASCII.
std::string SvgTextAsUtf8(std::string_view body) {
  bool big_endian = false;
  size_t start = 0;
  if (body.starts_with("\xFF\xFE")) {
    start = 2;
  } else if (body.starts_with("\xFE\xFF")) {
    big_endian = true;
    start = 2;
  } else if (body.starts_with(std::string_view("<\0?\0", 4))) {
  } else if (body.starts_with(std::string_view("\0<\0?", 4))) {
    big_endian = true;
  } else {
    return std::string(body);
  }
  std::u16string utf16;
  utf16.reserve((body.size() - start) / 2);
  for (size_t i = start; i + 1 < body.size(); i += 2) {
    const auto first = static_cast<uint8_t>(body[i]);
    const auto second = static_cast<uint8_t>(body[i + 1]);
    utf16.push_back(static_cast<char16_t>(big_endian ? (first << 8) | second
                                                     : (second << 8) | first));
  }
  return base::UTF16ToUTF8(utf16);
}

std::optional<ImageHeader> ReadSvg(std::string_view body) {
  const std::string text = SvgTextAsUtf8(body);
  const std::optional<std::pair<std::string, std::vector<XmlAttribute>>> root =
      ReadXmlRootElement(text);
  if (!root) {
    return std::nullopt;
  }
  const auto& [name, attributes] = *root;
  // The root must be SVG's svg element.
  const size_t colon = name.find(':');
  const std::string prefix =
      colon == std::string::npos ? "" : name.substr(0, colon);
  const std::string local =
      colon == std::string::npos ? name : name.substr(colon + 1);
  const std::u16string* namespace_uri =
      FindAttribute(attributes, prefix.empty() ? "xmlns" : "xmlns:" + prefix);
  if (local != "svg" || !namespace_uri || *namespace_uri != kSvgNamespace) {
    return std::nullopt;
  }
  ImageHeader header;
  header.kind = ImageHeader::Kind::kSvg;
  NaturalSizingInfo& sizing = header.sizing;
  const std::optional<float> width =
      IntrinsicLength(FindAttribute(attributes, "width"));
  const std::optional<float> height =
      IntrinsicLength(FindAttribute(attributes, "height"));
  sizing.width = width.value_or(0);
  sizing.height = height.value_or(0);
  sizing.has_width = width.has_value();
  sizing.has_height = height.has_value();
  if (sizing.width > 0 && sizing.height > 0) {
    sizing.aspect_width = sizing.width;
    sizing.aspect_height = sizing.height;
  } else if (const std::u16string* view_box =
                 FindAttribute(attributes, "viewBox")) {
    // Four numbers, apart by white space or a comma; a negative width or
    // height makes it invalid, and an empty one gives no ratio.
    const std::vector<std::u16string_view> numbers =
        base::SplitStringPiece(*view_box, u" \t\n\r,", base::TRIM_WHITESPACE,
                               base::SPLIT_WANT_NONEMPTY);
    std::array<double, 4> values = {};
    bool valid = numbers.size() == 4;
    for (size_t i = 0; valid && i < 4; ++i) {
      valid = base::StringToDouble(base::UTF16ToUTF8(numbers[i]), &values[i]) &&
              std::isfinite(values[i]);
    }
    if (valid && values[2] > 0 && values[3] > 0) {
      sizing.aspect_width = static_cast<float>(values[2]);
      sizing.aspect_height = static_cast<float>(values[3]);
    }
  }
  // SvgSizingWithPreserveAspectRatioNone is off in Chrome 157: with none,
  // a missing width or height leaves no natural dimensions at all.
  if (!sizing.has_width || !sizing.has_height) {
    if (const std::u16string* align =
            FindAttribute(attributes, "preserveAspectRatio")) {
      std::u16string_view value = base::TrimWhitespace(*align, base::TRIM_ALL);
      if (value.starts_with(u"defer")) {
        value = base::TrimWhitespace(value.substr(5), base::TRIM_LEADING);
      }
      if (value == u"none" || value.starts_with(u"none ")) {
        sizing = NaturalSizingInfo();
      }
    }
  }
  return header;
}

}  // namespace

std::optional<ImageHeader> ReadImageHeader(std::string_view body,
                                           std::string_view mime_type) {
  if (mime_type == "image/svg+xml") {
    return ReadSvg(body);
  }
  const ByteReader big(body, /*big_endian=*/true);
  const ByteReader little(body, /*big_endian=*/false);
  if (body.starts_with("\xFF\xD8\xFF")) {
    return ReadJpeg(big);
  }
  if (body.starts_with("\x89PNG\r\n\x1A\n")) {
    return ReadPng(big);
  }
  if (body.starts_with("GIF87a") || body.starts_with("GIF89a")) {
    return ReadGif(little);
  }
  if (body.starts_with("RIFF") && big.Bytes(8, 6) == "WEBPVP") {
    return ReadWebP(little);
  }
  if (body.starts_with(std::string_view("\0\0\1\0", 4)) ||
      body.starts_with(std::string_view("\0\0\2\0", 4))) {
    return ReadIco(little);
  }
  if (body.starts_with("BM") || body.starts_with("BA")) {
    return ReadBmp(little);
  }
  if (MatchesAvifSignature(big)) {
    return ReadAvif(big);
  }
  if (body.size() >= 12) {
    return ReadJxl(big, body);
  }
  return std::nullopt;
}

void ConcreteObjectSize(const NaturalSizingInfo& sizing,
                        float default_width,
                        float default_height,
                        float& width,
                        float& height) {
  const bool has_ratio = sizing.aspect_width > 0 && sizing.aspect_height > 0;
  if (sizing.has_width && sizing.has_height) {
    width = sizing.width;
    height = sizing.height;
  } else if (sizing.has_width) {
    width = sizing.width;
    height = has_ratio ? width * sizing.aspect_height / sizing.aspect_width
                       : default_height;
  } else if (sizing.has_height) {
    height = sizing.height;
    width = has_ratio ? height * sizing.aspect_width / sizing.aspect_height
                      : default_width;
  } else if (has_ratio) {
    // Contain: the largest box of the ratio that fits the default.
    const float solution_width =
        default_height * sizing.aspect_width / sizing.aspect_height;
    if (solution_width <= default_width) {
      width = solution_width;
      height = default_height;
    } else {
      width = default_width;
      height = default_width * sizing.aspect_height / sizing.aspect_width;
    }
  } else {
    width = default_width;
    height = default_height;
  }
}

}  // namespace mitto
