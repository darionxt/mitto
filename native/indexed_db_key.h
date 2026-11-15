// IndexedDB's keys, key paths and key ranges, as Blink's common code has them
// for both a renderer and the browser: a key of an array, a binary, a string,
// a date or a number, compared as the Indexed Database API compares two keys.
// The library and the Page program both build this file. A key travels and
// is kept in Chrome's sortable encoding, whose bytes compare as memcmp()
// compares them in the order the keys compare, so that an ordered store of
// bytes keeps records in the order of their keys.
//
// Upstream: third_party/blink/common/indexeddb/indexeddb_key.cc
// Upstream: third_party/blink/public/common/indexeddb/indexeddb_key_path.h
// Upstream: content/browser/indexed_db/indexed_db_leveldb_coding.cc

#ifndef MITTO_INDEXED_DB_KEY_H_
#define MITTO_INDEXED_DB_KEY_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mitto {

// blink.mojom.IDBKeyType, in its order, which the comparison of two keys of
// different types follows: an array is the greatest, a number the least.
enum class IDBKeyType : uint8_t {
  kInvalid = 0,
  kArray = 1,
  kBinary = 2,
  kString = 3,
  kDate = 4,
  kNumber = 5,
  kNone = 6,
  kMin = 7,
};

class IDBKey {
 public:
  using KeyArray = std::vector<IDBKey>;

  // How deep and how long an array key may be, as Blink converts one.
  static constexpr size_t kMaximumDepth = 2000;
  static constexpr size_t kMaximumArraySize = 1000000;

  // None, as a key that is not given.
  IDBKey();
  explicit IDBKey(IDBKeyType type);
  static IDBKey Number(double number);
  static IDBKey Date(double date);
  static IDBKey String(std::u16string string);
  static IDBKey Binary(std::string binary);
  static IDBKey Array(KeyArray array);

  IDBKey(const IDBKey&);
  IDBKey(IDBKey&&);
  IDBKey& operator=(const IDBKey&);
  IDBKey& operator=(IDBKey&&);
  ~IDBKey();

  IDBKeyType type() const { return type_; }
  const KeyArray& array() const { return array_; }
  const std::string& binary() const { return binary_; }
  const std::u16string& string() const { return string_; }
  double number() const { return number_; }

  // An array, binary, string, date or number, and an array only of such.
  bool IsValid() const;
  // -1, 0 or 1, as this sorts before, with or after `other`. Both are valid.
  int CompareTo(const IDBKey& other) const;
  bool IsLessThan(const IDBKey& other) const { return CompareTo(other) < 0; }
  bool Equals(const IDBKey& other) const { return CompareTo(other) == 0; }
  // Whether this is an array with a None among its members: a compound
  // index key that holds the primary key that a key generator has not given
  // yet.
  bool HasHoles() const;
  // This, with each hole filled with `primary_key`.
  IDBKey FillHoles(const IDBKey& primary_key) const;
  // What it adds to the size of a transaction, as Blink estimates it.
  size_t SizeEstimate() const;

 private:
  IDBKeyType type_ = IDBKeyType::kNone;
  KeyArray array_;
  std::string binary_;
  std::u16string string_;
  double number_ = 0;
};

// Chrome's EncodeSortableIDBKey() of a valid key, and DecodeSortableIDBKey(),
// which gives an invalid key for bytes that are not such an encoding.
std::string EncodeSortableIDBKey(const IDBKey& key);
IDBKey DecodeSortableIDBKey(std::string_view encoded);
// Decodes the key that `encoded` starts with, and returns it and how many
// bytes it took, or nullopt. No encoding is a prefix of another, so a key
// followed by another decodes as the first.
std::optional<std::pair<IDBKey, size_t>> DecodeSortableIDBKeyPrefix(
    std::string_view encoded);

// blink.mojom.IDBKeyPathType.
enum class IDBKeyPathType : uint8_t {
  kNull = 0,
  kString = 1,
  kArray = 2,
};

// A key path: none, a string, or an array of strings.
struct IDBKeyPath {
  IDBKeyPath();
  explicit IDBKeyPath(std::u16string string);
  explicit IDBKeyPath(std::vector<std::u16string> array);
  IDBKeyPath(const IDBKeyPath&);
  IDBKeyPath(IDBKeyPath&&);
  IDBKeyPath& operator=(const IDBKeyPath&);
  IDBKeyPath& operator=(IDBKeyPath&&);
  ~IDBKeyPath();

  bool IsNull() const { return type == IDBKeyPathType::kNull; }
  friend bool operator==(const IDBKeyPath&, const IDBKeyPath&) = default;

  IDBKeyPathType type = IDBKeyPathType::kNull;
  std::u16string string;
  std::vector<std::u16string> array;
};

// A range of keys, as blink.mojom.IDBKeyRange carries it: a bound that is
// None is open-ended.
struct IndexedDbKeyRange {
  IndexedDbKeyRange();
  IndexedDbKeyRange(IDBKey lower,
                    IDBKey upper,
                    bool lower_open,
                    bool upper_open);
  IndexedDbKeyRange(const IndexedDbKeyRange&);
  IndexedDbKeyRange(IndexedDbKeyRange&&);
  IndexedDbKeyRange& operator=(const IndexedDbKeyRange&);
  IndexedDbKeyRange& operator=(IndexedDbKeyRange&&);
  ~IndexedDbKeyRange();

  // A range of one key.
  static IndexedDbKeyRange Only(IDBKey key);
  bool IsOnlyKey() const;
  // Whether `key`, a valid one, lies in the range.
  bool Contains(const IDBKey& key) const;

  IDBKey lower;
  IDBKey upper;
  bool lower_open = false;
  bool upper_open = false;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_KEY_H_
