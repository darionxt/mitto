// The order in which Blink's HashMap<String, T> and HashSet<String> iterate
// their keys: their buckets in order, each key in the bucket that its 24-bit
// hash and triangular probing find, in a table that grows as
// HashTable::insert() grows it, or that ReserveCapacityForSize() sized.
// A page sees that order where Blink hands it such a map's keys, as in
// document.featurePolicy.features() and a KeyboardLayoutMap.
//
// Upstream: third_party/blink/renderer/platform/wtf/hash_table.h
// Upstream: third_party/blink/renderer/platform/wtf/hash_traits.h

#ifndef MITTO_DOM_STRING_HASH_TABLE_ORDER_H_
#define MITTO_DOM_STRING_HASH_TABLE_ORDER_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

namespace mitto {

class StringHashTableOrder {
 public:
  // An empty table, as a new HashMap has before its first key.
  StringHashTableOrder();
  // An empty table that ReserveCapacityForSize(`size`) made room for.
  static StringHashTableOrder WithCapacityFor(size_t size);

  StringHashTableOrder(const StringHashTableOrder&);
  StringHashTableOrder& operator=(const StringHashTableOrder&);
  ~StringHashTableOrder();

  // HashTable::insert() of `key`, a string of 8-bit characters that is not in
  // the table yet.
  void Insert(std::string_view key);

  // The keys in the order that the table iterates them.
  std::vector<std::string> Keys() const;

 private:
  // Places `key` in the first empty bucket that probing finds.
  void Place(std::string key);
  // HashTable::Rehash(): a table of `size` buckets with the keys again, in
  // the order of the old buckets.
  void Rehash(size_t size);

  // Empty strings are empty buckets: Blink's tables hold no empty key here.
  std::vector<std::string> buckets_;
  size_t count_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_STRING_HASH_TABLE_ORDER_H_
