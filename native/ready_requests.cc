#include "mitto/ready_requests.h"

#include "net/base/net_errors.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace mitto {

#if BUILDFLAG(IS_POSIX)
namespace {

// The pipe holds one byte while anything is listed.
void WriteByte(const base::ScopedFD& write_end) {
  const char byte = 0;
  PCHECK(HANDLE_EINTR(write(write_end.get(), &byte, 1)) == 1);
}

void ReadByte(const base::ScopedFD& read_end) {
  char byte;
  PCHECK(HANDLE_EINTR(read(read_end.get(), &byte, 1)) == 1);
}

}  // namespace
#endif

ReadyRequests::ReadyRequests() : changed_(&lock_) {}

ReadyRequests::~ReadyRequests() = default;

#if BUILDFLAG(IS_POSIX)
std::optional<int> ReadyRequests::GetFileDescriptor() {
  base::AutoLock lock(lock_);
  if (!read_end_.is_valid()) {
    if (!base::CreatePipe(&read_end_, &write_end_, /*non_blocking=*/true)) {
      return std::nullopt;
    }
    // Wait() may have started the list already.
    if (!handles_.empty()) {
      WriteByte(write_end_);
    }
  }
  listing_ = true;
  return read_end_.get();
}
#endif

int ReadyRequests::Wait(std::optional<base::TimeDelta> timeout) {
  base::AutoLock lock(lock_);
  listing_ = true;
  const base::TimeTicks deadline =
      timeout ? base::TimeTicks::Now() + *timeout : base::TimeTicks::Max();
  ++running_waits_;
  int result = net::OK;
  while (true) {
    if (waits_ended_) {
      result = net::ERR_ABORTED;
      break;
    }
    if (!handles_.empty() && !returned_for_list_) {
      returned_for_list_ = true;
      break;
    }
    if (!timeout) {
      changed_.Wait();
      continue;
    }
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (!remaining.is_positive()) {
      result = net::ERR_IO_PENDING;
      break;
    }
    changed_.TimedWait(remaining);
  }
  --running_waits_;
  if (waits_ended_ && running_waits_ == 0) {
    changed_.Broadcast();
  }
  return result;
}

void ReadyRequests::EndWaits() {
  base::AutoLock lock(lock_);
  waits_ended_ = true;
  changed_.Broadcast();
}

void ReadyRequests::EndWaitsAndWaitForThem() {
  base::AutoLock lock(lock_);
  waits_ended_ = true;
  changed_.Broadcast();
  while (running_waits_ > 0) {
    changed_.Wait();
  }
}

std::optional<ReadyHandle> ReadyRequests::Take() {
  base::AutoLock lock(lock_);
  if (handles_.empty()) {
    return std::nullopt;
  }
  const ReadyHandle handle = handles_.front();
  handles_.pop_front();
  if (handles_.empty()) {
    // The next handle listed is news to Wait() again.
    returned_for_list_ = false;
#if BUILDFLAG(IS_POSIX)
    if (read_end_.is_valid()) {
      ReadByte(read_end_);
    }
#endif
  }
  return handle;
}

void ReadyRequests::Add(uintptr_t address, int32_t kind) {
  base::AutoLock lock(lock_);
  if (!listing_) {
    return;
  }
  handles_.push_back({.kind = kind, .address = address});
  if (handles_.size() == 1) {
#if BUILDFLAG(IS_POSIX)
    if (write_end_.is_valid()) {
      WriteByte(write_end_);
    }
#endif
    changed_.Broadcast();
  }
}

}  // namespace mitto
