// A double-ended queue of objects on the C++ heap whose buffer is on that heap
// too, as Blink's HeapDeque keeps one, for the same reason as HeapVector
// (heap_vector.h): V8's collector traces objects on threads of its own while
// scripts run. It is a ring in a buffer that a change that needs more room
// replaces with a larger one, leaving the old one to the collector.
//
// It lives in an object on the heap, which traces it.

#ifndef MITTO_DOM_HEAP_DEQUE_H_
#define MITTO_DOM_HEAP_DEQUE_H_

#include <stddef.h>

#include <algorithm>
#include <new>
#include <utility>

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

template <typename T>
class HeapDeque {
 public:
  HeapDeque() = default;
  HeapDeque(const HeapDeque&) = delete;
  HeapDeque& operator=(const HeapDeque&) = delete;
  ~HeapDeque() = default;

  void Trace(cppgc::Visitor* visitor) const { visitor->Trace(buffer_); }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // The item at `index` from the front.
  T* operator[](size_t index) const {
    CHECK_LT(index, size_);
    return buffer_->at(Slot(index));
  }
  T* front() const { return (*this)[0]; }
  T* back() const { return (*this)[size_ - 1]; }

  void push_back(T* item) {
    Reserve(size_ + 1);
    buffer_->set(Slot(size_), item);
    ++size_;
  }

  void push_front(T* item) {
    Reserve(size_ + 1);
    start_ = (start_ + buffer_->capacity() - 1) % buffer_->capacity();
    buffer_->set(start_, item);
    ++size_;
  }

  void pop_front() {
    CHECK_GT(size_, 0u);
    buffer_->set(start_, nullptr);
    start_ = (start_ + 1) % buffer_->capacity();
    --size_;
  }

  void pop_back() {
    CHECK_GT(size_, 0u);
    buffer_->set(Slot(size_ - 1), nullptr);
    --size_;
  }

  // Removes the front item and returns it, as Blink's TakeFirst() does.
  T* TakeFirst() {
    T* const item = front();
    pop_front();
    return item;
  }

  void clear() {
    buffer_ = nullptr;
    start_ = 0;
    size_ = 0;
  }

  void swap(HeapDeque& other) {
    std::swap(buffer_, other.buffer_);
    std::swap(start_, other.start_);
    std::swap(size_, other.size_);
  }

 private:
  // A buffer of Members after the object, as AdditionalBytes gives room for.
  class Buffer final : public cppgc::GarbageCollected<Buffer> {
   public:
    explicit Buffer(size_t capacity) : capacity_(capacity) {
      for (size_t i = 0; i < capacity_; ++i) {
        // SAFETY: AdditionalBytes made room for `capacity_` Members.
        new (UNSAFE_BUFFERS(items() + i)) cppgc::Member<T>();
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
    cppgc::Member<T>* items() const {
      // SAFETY: the Members follow the object.
      return reinterpret_cast<cppgc::Member<T>*>(UNSAFE_BUFFERS(
          reinterpret_cast<char*>(const_cast<Buffer*>(this)) + sizeof(Buffer)));
    }

    const size_t capacity_;
  };

  size_t Slot(size_t index) const {
    return (start_ + index) % buffer_->capacity();
  }

  void Reserve(size_t needed) {
    const size_t capacity = buffer_ ? buffer_->capacity() : 0;
    if (needed <= capacity) {
      return;
    }
    const size_t new_capacity = std::max<size_t>(needed, capacity * 2 + 4);
    Buffer* const larger = cppgc::MakeGarbageCollected<Buffer>(
        v8::Isolate::GetCurrent()->GetCppHeap()->GetAllocationHandle(),
        cppgc::AdditionalBytes(new_capacity * sizeof(cppgc::Member<T>)),
        new_capacity);
    for (size_t i = 0; i < size_; ++i) {
      larger->set(i, buffer_->at(Slot(i)));
    }
    buffer_ = larger;
    start_ = 0;
  }

  cppgc::Member<Buffer> buffer_;
  size_t start_ = 0;
  size_t size_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HEAP_DEQUE_H_
