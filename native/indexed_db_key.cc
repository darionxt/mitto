#include "mitto/indexed_db_key.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

#include "base/check.h"
#include "base/notreached.h"

namespace mitto {
namespace {

// What Blink adds to each key's size estimate.
constexpr size_t kOverheadSize = 16;

// The type bytes of the sortable encoding, in the order of the comparison.
constexpr char kSentinel = 0;
constexpr unsigned char kOrderedNumberTypeByte = 0x10;
constexpr unsigned char kOrderedDateTypeByte = 0x20;
constexpr unsigned char kOrderedStringTypeByte = 0x30;
constexpr unsigned char kOrderedBinaryTypeByte = 0x40;
constexpr unsigned char kOrderedArrayTypeByte = 0x50;

constexpr unsigned char kTwoByteEncodingIndicator = 0x80;
constexpr unsigned char kThreeByteEncodingIndicator = 0xff;
constexpr char16_t kOneByteMaxChar = 0x80 - 2;
constexpr char16_t kTwoByteMaxChar = 0xffff >> 2;

// EncodeBinaryWithSentinel()'s chunks.
constexpr size_t kChunkSize = 8;
constexpr unsigned char kMarkerByte = kChunkSize + 1;
constexpr unsigned char kEmptyBinarySentinel = 0;
constexpr size_t kChunkSizeWithMarker = kChunkSize + 1;
constexpr char kPaddingByte = 0;

template <typename T>
int Compare(const T& a, const T& b) {
  // '<' both ways, so that NaNs compare equal.
  if (a < b) {
    return -1;
  }
  return (b < a) ? 1 : 0;
}

void EncodeSortableVarChar16(std::optional<char16_t> source,
                             std::string& target) {
  if (!source) {
    target.push_back(kSentinel);
    return;
  }
  if (*source <= kOneByteMaxChar) {
    target.push_back(static_cast<char>(*source + 1));
    return;
  }
  if (*source <= kTwoByteMaxChar) {
    target.push_back(
        static_cast<char>(((*source >> 8) & 0xff) | kTwoByteEncodingIndicator));
    target.push_back(static_cast<char>(*source & 0xff));
    return;
  }
  target.push_back(static_cast<char>(kThreeByteEncodingIndicator));
  target.push_back(static_cast<char>((*source >> 8) & 0xff));
  target.push_back(static_cast<char>(*source & 0xff));
}

bool DecodeSortableVarChar16(std::string_view& from,
                             std::optional<char16_t>& target) {
  if (from.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(from.front());
  if (first == static_cast<unsigned char>(kSentinel)) {
    from.remove_prefix(1);
    target.reset();
    return true;
  }
  if ((first & 0x80) == 0) {
    from.remove_prefix(1);
    target = static_cast<char16_t>((first & 0x7f) - 1);
    return true;
  }
  if (from.size() < 2) {
    return false;
  }
  const unsigned char second = static_cast<unsigned char>(from[1]);
  if ((first & 0b11000000) == kTwoByteEncodingIndicator) {
    from.remove_prefix(2);
    target = static_cast<char16_t>(second | ((first & 0b00111111) << 8));
    return *target > kOneByteMaxChar;
  }
  if (from.size() < 3 || first != kThreeByteEncodingIndicator) {
    return false;
  }
  const unsigned char third = static_cast<unsigned char>(from[2]);
  from.remove_prefix(3);
  target = static_cast<char16_t>(third | (second << 8));
  return *target > kTwoByteMaxChar;
}

void EncodeBinaryWithSentinel(const std::string& value, std::string& into) {
  if (value.empty()) {
    into.push_back(static_cast<char>(kEmptyBinarySentinel));
    return;
  }
  for (size_t i = 0; i < value.size(); i += kChunkSize) {
    into.push_back(static_cast<char>(kMarkerByte));
    for (size_t j = 0; j < kChunkSize; ++j) {
      into.push_back(i + j < value.size() ? value[i + j] : kPaddingByte);
    }
  }
  const size_t non_padding = value.size() % kChunkSize;
  into.push_back(
      static_cast<char>(non_padding == 0 ? kChunkSize : non_padding));
}

std::optional<std::string> DecodeBinaryWithSentinel(std::string_view& encoded) {
  if (!encoded.empty() &&
      static_cast<unsigned char>(encoded.front()) == kEmptyBinarySentinel) {
    encoded.remove_prefix(1);
    return std::string();
  }
  std::string output;
  while (!encoded.empty()) {
    const unsigned char marker = static_cast<unsigned char>(encoded.front());
    if (marker == kMarkerByte) {
      if (encoded.size() < kChunkSizeWithMarker) {
        return std::nullopt;
      }
      output.append(encoded.substr(1, kChunkSize));
      encoded.remove_prefix(kChunkSizeWithMarker);
    } else if (marker >= 1 && marker <= kChunkSize) {
      if (marker > output.size()) {
        return std::nullopt;
      }
      for (size_t i = 0; i < kChunkSize - marker; ++i) {
        if (output.back() != kPaddingByte) {
          return std::nullopt;
        }
        output.pop_back();
      }
      encoded.remove_prefix(1);
      return output;
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void EncodeSortableDouble(double value, std::string& into) {
  CHECK(!std::isnan(value));
  uint64_t bits = std::bit_cast<uint64_t>(value);
  // Negative doubles sort in reverse as bits, so all their bits flip; a
  // positive one, and -0 as +0, gets its sign bit set to sort above them.
  if (std::signbit(value) && value != 0.0) {
    bits = ~bits;
  } else {
    bits = std::bit_cast<uint64_t>(std::abs(value)) | (uint64_t{1} << 63);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    into.push_back(static_cast<char>((bits >> shift) & 0xff));
  }
}

bool DecodeSortableDouble(std::string_view& data, double& output) {
  if (data.size() < 8) {
    return false;
  }
  uint64_t bits = 0;
  for (size_t i = 0; i < 8; ++i) {
    bits = (bits << 8) | static_cast<unsigned char>(data[i]);
  }
  data.remove_prefix(8);
  constexpr uint64_t kSignBit = uint64_t{1} << 63;
  bits = (bits & kSignBit) ? bits ^ kSignBit : ~bits;
  output = std::bit_cast<double>(bits);
  return !std::isnan(output) && !(std::signbit(output) && output == 0.0);
}

IDBKey DecodeSortableKeyNonArray(unsigned char type, std::string_view& data) {
  switch (type) {
    case kOrderedBinaryTypeByte:
      if (std::optional<std::string> binary = DecodeBinaryWithSentinel(data)) {
        return IDBKey::Binary(*std::move(binary));
      }
      break;
    case kOrderedStringTypeByte: {
      std::u16string string;
      while (true) {
        std::optional<char16_t> c;
        if (!DecodeSortableVarChar16(data, c)) {
          return IDBKey(IDBKeyType::kInvalid);
        }
        if (!c) {
          return IDBKey::String(std::move(string));
        }
        string.push_back(*c);
      }
    }
    case kOrderedDateTypeByte: {
      double date = 0;
      if (DecodeSortableDouble(data, date)) {
        return IDBKey::Date(date);
      }
      break;
    }
    case kOrderedNumberTypeByte: {
      double number = 0;
      if (DecodeSortableDouble(data, number)) {
        return IDBKey::Number(number);
      }
      break;
    }
    default:
      break;
  }
  return IDBKey(IDBKeyType::kInvalid);
}

}  // namespace

IDBKey::IDBKey() = default;
IDBKey::IDBKey(IDBKeyType type) : type_(type) {}
IDBKey::IDBKey(const IDBKey&) = default;
IDBKey::IDBKey(IDBKey&&) = default;
IDBKey& IDBKey::operator=(const IDBKey&) = default;
IDBKey& IDBKey::operator=(IDBKey&&) = default;
IDBKey::~IDBKey() = default;

// static
IDBKey IDBKey::Number(double number) {
  IDBKey key(IDBKeyType::kNumber);
  key.number_ = number;
  return key;
}

// static
IDBKey IDBKey::Date(double date) {
  IDBKey key(IDBKeyType::kDate);
  key.number_ = date;
  return key;
}

// static
IDBKey IDBKey::String(std::u16string string) {
  IDBKey key(IDBKeyType::kString);
  key.string_ = std::move(string);
  return key;
}

// static
IDBKey IDBKey::Binary(std::string binary) {
  IDBKey key(IDBKeyType::kBinary);
  key.binary_ = std::move(binary);
  return key;
}

// static
IDBKey IDBKey::Array(KeyArray array) {
  IDBKey key(IDBKeyType::kArray);
  key.array_ = std::move(array);
  return key;
}

bool IDBKey::IsValid() const {
  switch (type_) {
    case IDBKeyType::kArray:
      return std::ranges::all_of(array_, &IDBKey::IsValid);
    case IDBKeyType::kBinary:
    case IDBKeyType::kString:
    case IDBKeyType::kDate:
    case IDBKeyType::kNumber:
      return true;
    case IDBKeyType::kInvalid:
    case IDBKeyType::kNone:
    case IDBKeyType::kMin:
      return false;
  }
  NOTREACHED();
}

int IDBKey::CompareTo(const IDBKey& other) const {
  if (type_ != other.type_) {
    return type_ > other.type_ ? -1 : 1;
  }
  switch (type_) {
    case IDBKeyType::kArray:
      for (size_t i = 0; i < array_.size() && i < other.array_.size(); ++i) {
        if (const int result = array_[i].CompareTo(other.array_[i])) {
          return result;
        }
      }
      return Compare(array_.size(), other.array_.size());
    case IDBKeyType::kBinary:
      return Compare(binary_.compare(other.binary_), 0);
    case IDBKeyType::kString:
      return Compare(string_.compare(other.string_), 0);
    case IDBKeyType::kDate:
    case IDBKeyType::kNumber:
      return Compare(number_, other.number_);
    case IDBKeyType::kInvalid:
    case IDBKeyType::kNone:
    case IDBKeyType::kMin:
      return 0;
  }
  NOTREACHED();
}

bool IDBKey::HasHoles() const {
  return type_ == IDBKeyType::kArray &&
         std::ranges::any_of(array_, [](const IDBKey& subkey) {
           return subkey.type() == IDBKeyType::kNone;
         });
}

IDBKey IDBKey::FillHoles(const IDBKey& primary_key) const {
  if (type_ != IDBKeyType::kArray) {
    return *this;
  }
  KeyArray subkeys;
  subkeys.reserve(array_.size());
  for (const IDBKey& subkey : array_) {
    subkeys.push_back(subkey.type() == IDBKeyType::kNone ? primary_key
                                                         : subkey);
  }
  return Array(std::move(subkeys));
}

size_t IDBKey::SizeEstimate() const {
  switch (type_) {
    case IDBKeyType::kArray: {
      size_t size = kOverheadSize;
      for (const IDBKey& subkey : array_) {
        size += subkey.SizeEstimate();
      }
      return size;
    }
    case IDBKeyType::kBinary:
      return kOverheadSize + binary_.size();
    case IDBKeyType::kString:
      return kOverheadSize + string_.size() * sizeof(char16_t);
    case IDBKeyType::kDate:
    case IDBKeyType::kNumber:
      return kOverheadSize + sizeof(double);
    case IDBKeyType::kInvalid:
    case IDBKeyType::kNone:
    case IDBKeyType::kMin:
      return kOverheadSize;
  }
  NOTREACHED();
}

std::string EncodeSortableIDBKey(const IDBKey& key) {
  CHECK(key.IsValid());
  std::string into;
  // Depth first, without recursion: null marks the end of an array.
  std::vector<const IDBKey*> keys{&key};
  while (!keys.empty()) {
    const IDBKey* const next = keys.back();
    keys.pop_back();
    if (!next) {
      into.push_back(kSentinel);
      continue;
    }
    switch (next->type()) {
      case IDBKeyType::kArray:
        into.push_back(static_cast<char>(kOrderedArrayTypeByte));
        keys.push_back(nullptr);
        for (const IDBKey& subkey : std::views::reverse(next->array())) {
          keys.push_back(&subkey);
        }
        break;
      case IDBKeyType::kBinary:
        into.push_back(static_cast<char>(kOrderedBinaryTypeByte));
        EncodeBinaryWithSentinel(next->binary(), into);
        break;
      case IDBKeyType::kString:
        into.push_back(static_cast<char>(kOrderedStringTypeByte));
        for (char16_t c : next->string()) {
          EncodeSortableVarChar16(c, into);
        }
        EncodeSortableVarChar16(std::nullopt, into);
        break;
      case IDBKeyType::kDate:
        into.push_back(static_cast<char>(kOrderedDateTypeByte));
        EncodeSortableDouble(next->number(), into);
        break;
      case IDBKeyType::kNumber:
        into.push_back(static_cast<char>(kOrderedNumberTypeByte));
        EncodeSortableDouble(next->number(), into);
        break;
      case IDBKeyType::kInvalid:
      case IDBKeyType::kNone:
      case IDBKeyType::kMin:
        NOTREACHED();
    }
  }
  return into;
}

std::optional<std::pair<IDBKey, size_t>> DecodeSortableIDBKeyPrefix(
    std::string_view encoded) {
  std::string_view data = encoded;
  std::vector<IDBKey::KeyArray> arrays;
  while (!data.empty()) {
    const unsigned char type = static_cast<unsigned char>(data.front());
    data.remove_prefix(1);
    switch (type) {
      case kOrderedArrayTypeByte:
        if (arrays.size() >= IDBKey::kMaximumDepth) {
          return std::nullopt;
        }
        arrays.emplace_back();
        break;
      case kOrderedBinaryTypeByte:
      case kOrderedStringTypeByte:
      case kOrderedDateTypeByte:
      case kOrderedNumberTypeByte: {
        IDBKey key = DecodeSortableKeyNonArray(type, data);
        if (!key.IsValid()) {
          return std::nullopt;
        }
        if (arrays.empty()) {
          return std::make_pair(std::move(key), encoded.size() - data.size());
        }
        arrays.back().push_back(std::move(key));
        break;
      }
      case static_cast<unsigned char>(kSentinel): {
        if (arrays.empty()) {
          return std::nullopt;
        }
        IDBKey array = IDBKey::Array(std::move(arrays.back()));
        arrays.pop_back();
        if (arrays.empty()) {
          return std::make_pair(std::move(array), encoded.size() - data.size());
        }
        arrays.back().push_back(std::move(array));
        break;
      }
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

IDBKey DecodeSortableIDBKey(std::string_view encoded) {
  std::optional<std::pair<IDBKey, size_t>> decoded =
      DecodeSortableIDBKeyPrefix(encoded);
  if (!decoded || decoded->second != encoded.size()) {
    return IDBKey(IDBKeyType::kInvalid);
  }
  return std::move(decoded->first);
}

IDBKeyPath::IDBKeyPath() = default;
IDBKeyPath::IDBKeyPath(std::u16string string)
    : type(IDBKeyPathType::kString), string(std::move(string)) {}
IDBKeyPath::IDBKeyPath(std::vector<std::u16string> array)
    : type(IDBKeyPathType::kArray), array(std::move(array)) {}
IDBKeyPath::IDBKeyPath(const IDBKeyPath&) = default;
IDBKeyPath::IDBKeyPath(IDBKeyPath&&) = default;
IDBKeyPath& IDBKeyPath::operator=(const IDBKeyPath&) = default;
IDBKeyPath& IDBKeyPath::operator=(IDBKeyPath&&) = default;
IDBKeyPath::~IDBKeyPath() = default;

IndexedDbKeyRange::IndexedDbKeyRange() = default;
IndexedDbKeyRange::IndexedDbKeyRange(IDBKey lower,
                                     IDBKey upper,
                                     bool lower_open,
                                     bool upper_open)
    : lower(std::move(lower)),
      upper(std::move(upper)),
      lower_open(lower_open),
      upper_open(upper_open) {}
IndexedDbKeyRange::IndexedDbKeyRange(const IndexedDbKeyRange&) = default;
IndexedDbKeyRange::IndexedDbKeyRange(IndexedDbKeyRange&&) = default;
IndexedDbKeyRange& IndexedDbKeyRange::operator=(const IndexedDbKeyRange&) =
    default;
IndexedDbKeyRange& IndexedDbKeyRange::operator=(IndexedDbKeyRange&&) = default;
IndexedDbKeyRange::~IndexedDbKeyRange() = default;

// static
IndexedDbKeyRange IndexedDbKeyRange::Only(IDBKey key) {
  IDBKey upper = key;
  return IndexedDbKeyRange(std::move(key), std::move(upper), false, false);
}

bool IndexedDbKeyRange::IsOnlyKey() const {
  return !lower_open && !upper_open && lower.IsValid() && upper.IsValid() &&
         lower.Equals(upper);
}

bool IndexedDbKeyRange::Contains(const IDBKey& key) const {
  if (lower.IsValid()) {
    const int compared = key.CompareTo(lower);
    if (compared < 0 || (compared == 0 && lower_open)) {
      return false;
    }
  }
  if (upper.IsValid()) {
    const int compared = key.CompareTo(upper);
    if (compared > 0 || (compared == 0 && upper_open)) {
      return false;
    }
  }
  return true;
}

}  // namespace mitto
