#include "mitto/ready_requests.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace mitto {

ReadyRequests::ReadyRequests() = default;
ReadyRequests::~ReadyRequests() = default;

#if BUILDFLAG(IS_POSIX)
std::optional<int> ReadyRequests::GetFileDescriptor() {
  base::AutoLock lock(lock_);
  if (!read_end_.is_valid() &&
      !base::CreatePipe(&read_end_, &write_end_, /*non_blocking=*/true)) {
    return std::nullopt;
  }
  return read_end_.get();
}
#endif

std::optional<uintptr_t> ReadyRequests::Take() {
  base::AutoLock lock(lock_);
  if (requests_.empty()) {
    return std::nullopt;
  }
  const uintptr_t request = requests_.front();
  requests_.pop_front();
#if BUILDFLAG(IS_POSIX)
  if (requests_.empty()) {
    char byte;
    PCHECK(HANDLE_EINTR(read(read_end_.get(), &byte, 1)) == 1);
  }
#endif
  return request;
}

void ReadyRequests::Add(uintptr_t request) {
#if BUILDFLAG(IS_POSIX)
  base::AutoLock lock(lock_);
  if (!write_end_.is_valid()) {
    return;
  }
  requests_.push_back(request);
  if (requests_.size() == 1) {
    const char byte = 0;
    PCHECK(HANDLE_EINTR(write(write_end_.get(), &byte, 1)) == 1);
  }
#endif
}

}  // namespace mitto
