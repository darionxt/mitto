#include "mitto/dom/bytes_uploader.h"

#include <algorithm>
#include <tuple>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/strings/string_view_util.h"
#include "mitto/dom/fetcher.h"

namespace mitto {

BytesUploader::BytesUploader(BytesConsumer* consumer,
                             FetchController& controller)
    : consumer_(consumer), controller_(&controller) {
  DCHECK_EQ(consumer_->GetPublicState(),
            BytesConsumer::PublicState::kReadableOrWaiting);
}

void BytesUploader::OnRoom() {
  if (disposed_) {
    return;
  }
  if (!started_) {
    started_ = true;
    consumer_->SetClient(this);
    if (consumer_->GetPublicState() !=
        BytesConsumer::PublicState::kReadableOrWaiting) {
      return;
    }
  }
  WriteDataOnPipe();
}

void BytesUploader::OnRequestGone() {
  controller_ = nullptr;
  if (!started_) {
    disposed_ = true;
    return;
  }
  WriteDataOnPipe();
}

void BytesUploader::Detach() {
  controller_ = nullptr;
  disposed_ = true;
}

void BytesUploader::OnStateChange() {
  if (disposed_) {
    return;
  }
  switch (consumer_->GetPublicState()) {
    case BytesConsumer::PublicState::kReadableOrWaiting:
      WriteDataOnPipe();
      return;
    case BytesConsumer::PublicState::kClosed:
      Close();
      return;
    case BytesConsumer::PublicState::kErrored:
      CloseOnError();
      return;
  }
  NOTREACHED();
}

void BytesUploader::WriteDataOnPipe() {
  if (disposed_) {
    return;
  }
  while (true) {
    base::span<const char> buffer;
    BytesConsumer::Result result = consumer_->BeginRead(buffer);
    switch (result) {
      case BytesConsumer::Result::kError:
        CloseOnError();
        return;
      case BytesConsumer::Result::kShouldWait:
        return;
      case BytesConsumer::Result::kDone:
        Close();
        return;
      case BytesConsumer::Result::kOk:
        break;
    }
    // A write to a data pipe whose reader has gone fails.
    if (!controller_) {
      std::ignore = consumer_->EndRead(0);
      CloseOnError();
      return;
    }
    const size_t room = controller_->BodyRoom();
    if (room == 0 && !buffer.empty()) {
      // MOJO_RESULT_SHOULD_WAIT: OnRoom() says when there is room.
      std::ignore = consumer_->EndRead(0);
      return;
    }
    const size_t size = std::min(buffer.size(), room);
    if (size > 0) {
      controller_->SendBody(base::as_string_view(buffer.first(size)));
    }
    result = consumer_->EndRead(size);
    switch (result) {
      case BytesConsumer::Result::kError:
        CloseOnError();
        return;
      case BytesConsumer::Result::kShouldWait:
        NOTREACHED();
      case BytesConsumer::Result::kDone:
        Close();
        return;
      case BytesConsumer::Result::kOk:
        break;
    }
  }
}

void BytesUploader::Close() {
  if (controller_) {
    controller_->EndBody(/*errored=*/false);
  }
  consumer_->Cancel();
  Dispose();
}

void BytesUploader::CloseOnError() {
  if (controller_) {
    controller_->EndBody(/*errored=*/true);
  }
  consumer_->Cancel();
  Dispose();
}

void BytesUploader::Dispose() {
  controller_ = nullptr;
  disposed_ = true;
}

void BytesUploader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(consumer_);
}

}  // namespace mitto
