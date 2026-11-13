// A list of objects on the C++ heap whose buffer is on that heap too, as
// Blink's HeapVector keeps one. V8's collector traces objects on threads of its
// own while scripts run, which is safe for a Member in an object on the heap,
// but not for one in a std::vector's buffer, which the main thread can
// reallocate meanwhile (event_target.h). Each change that needs more room makes
// a larger buffer and leaves the old one to the collector.
//
// It lives in an object on the heap, which traces it. A WeakHeapVector holds
// its items weakly, as Blink's HeapVector of WeakMembers: an item that only it
// holds reads as null once the collector has taken it.

#ifndef MITTO_DOM_HEAP_VECTOR_H_
#define MITTO_DOM_HEAP_VECTOR_H_

#include <stddef.h>

#include <algorithm>
#include <new>
#include <vector>

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

template <typename T, typename Item = cppgc::Member<T>>
class HeapVector {
 public:
  HeapVector() = default;
  HeapVector(const HeapVector&) = delete;
  HeapVector& operator=(const HeapVector&) = delete;
  ~HeapVector() = default;

  void Trace(cppgc::Visitor* visitor) const { visitor->Trace(buffer_); }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  T* operator[](size_t index) const {
    CHECK_LT(index, size_);
    return buffer_->at(index);
  }

  void push_back(T* item) { insert(size_, item); }

  void insert(size_t index, T* item) {
    CHECK_LE(index, size_);
    Reserve(size_ + 1);
    for (size_t i = size_; i > index; --i) {
      buffer_->set(i, buffer_->at(i - 1));
    }
    buffer_->set(index, item);
    ++size_;
  }

  // Removes those for which `remove` is true, keeping the order of the rest.
  template <typename Predicate>
  void EraseIf(Predicate remove) {
    size_t kept = 0;
    for (size_t i = 0; i < size_; ++i) {
      T* const item = buffer_->at(i);
      if (!remove(item)) {
        buffer_->set(kept++, item);
      }
    }
    for (size_t i = kept; i < size_; ++i) {
      buffer_->set(i, nullptr);
    }
    size_ = kept;
  }

  void clear() {
    buffer_ = nullptr;
    size_ = 0;
  }

  // The items, on the stack, where the collector finds them.
  std::vector<T*> ToVector() const {
    std::vector<T*> items;
    items.reserve(size_);
    for (size_t i = 0; i < size_; ++i) {
      items.push_back(buffer_->at(i));
    }
    return items;
  }

 private:
  // A buffer of Members after the object, as AdditionalBytes gives room for.
  class Buffer final : public cppgc::GarbageCollected<Buffer> {
   public:
    explicit Buffer(size_t capacity) : capacity_(capacity) {
      for (size_t i = 0; i < capacity_; ++i) {
        // SAFETY: AdditionalBytes made room for `capacity_` Members.
        new (UNSAFE_BUFFERS(items() + i)) Item();
      }
    }

    void Trace(cppgc::Visitor* visitor) const {
      for (size_t i = 0; i < capacity_; ++i) {
        // SAFETY: there are `capacity_` Members.
        visitor->Trace(UNSAFE_BUFFERS(items()[i]));
      }
    }

    size_t capacity() const { return capacity_; }
    T* at(size_t index) const {
      CHECK_LT(index, capacity_);
      // SAFETY: checked against the capacity.
      return UNSAFE_BUFFERS(items()[index]).Get();
    }
    void set(size_t index, T* item) {
      CHECK_LT(index, capacity_);
      // SAFETY: checked against the capacity.
      UNSAFE_BUFFERS(items()[index]) = item;
    }

   private:
    Item* items() const {
      // SAFETY: the Members follow the object.
      return reinterpret_cast<Item*>(UNSAFE_BUFFERS(
          reinterpret_cast<char*>(const_cast<Buffer*>(this)) + sizeof(Buffer)));
    }

    const size_t capacity_;
  };

  void Reserve(size_t needed) {
    const size_t capacity = buffer_ ? buffer_->capacity() : 0;
    if (needed <= capacity) {
      return;
    }
    const size_t new_capacity = std::max<size_t>(needed, capacity * 2 + 4);
    Buffer* const larger = cppgc::MakeGarbageCollected<Buffer>(
        v8::Isolate::GetCurrent()->GetCppHeap()->GetAllocationHandle(),
        cppgc::AdditionalBytes(new_capacity * sizeof(Item)), new_capacity);
    for (size_t i = 0; i < size_; ++i) {
      larger->set(i, buffer_->at(i));
    }
    buffer_ = larger;
  }

  cppgc::Member<Buffer> buffer_;
  size_t size_ = 0;
};

template <typename T>
using WeakHeapVector = HeapVector<T, cppgc::WeakMember<T>>;

}  // namespace mitto

#endif  // MITTO_DOM_HEAP_VECTOR_H_
