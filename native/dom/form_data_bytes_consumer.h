// The bytes of a body that a script gave whole, such as a string or an
// ArrayBuffer, as Blink's FormDataBytesConsumer holds a body of data only.
//
// Upstream: third_party/blink/renderer/core/fetch/form_data_bytes_consumer.h
// Upstream: third_party/blink/renderer/core/fetch/form_data_bytes_consumer.cc

#ifndef MITTO_DOM_FORM_DATA_BYTES_CONSUMER_H_
#define MITTO_DOM_FORM_DATA_BYTES_CONSUMER_H_

#include <stddef.h>

#include <optional>
#include <string>

#include "mitto/dom/bytes_consumer.h"

namespace mitto {

class FormDataBytesConsumer final : public BytesConsumer {
 public:
  explicit FormDataBytesConsumer(std::string bytes);
  ~FormDataBytesConsumer() override;

  // BytesConsumer:
  Result BeginRead(base::span<const char>& buffer) override;
  Result EndRead(size_t read_size) override;
  std::optional<std::string> DrainAsBytes() override;
  void SetClient(BytesConsumer::Client* client) override;
  void ClearClient() override;
  void Cancel() override;
  PublicState GetPublicState() const override;

 private:
  std::string bytes_;
  // Whether a read has begun, after which the bytes cannot be drained.
  bool flattened_ = false;
  size_t offset_ = 0;
  PublicState state_ = PublicState::kReadableOrWaiting;
};

}  // namespace mitto

#endif  // MITTO_DOM_FORM_DATA_BYTES_CONSUMER_H_
