#include "mitto/page_messages.h"

#include <array>
#include <optional>
#include <utility>

#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/notreached.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/values.h"

namespace mitto {
namespace {

constexpr std::string_view kTimeZoneKey = "time_zone";
constexpr std::string_view kLocaleKey = "locale";

constexpr uint8_t kHasResult = 0;
constexpr uint8_t kFailed = 1;

std::string EncodeUint32(uint32_t value) {
  const std::array<uint8_t, 4> bytes = base::U32ToLittleEndian(value);
  return std::string(base::as_string_view(base::span(bytes)));
}

std::string EncodeUint64(uint64_t value) {
  const std::array<uint8_t, 8> bytes = base::U64ToLittleEndian(value);
  return std::string(base::as_string_view(base::span(bytes)));
}

uint32_t DecodeUint32(std::string_view bytes) {
  return base::U32FromLittleEndian(base::as_byte_span(bytes).first<4>());
}

uint64_t DecodeUint64(std::string_view bytes) {
  return base::U64FromLittleEndian(base::as_byte_span(bytes).first<8>());
}

bool IsKnown(uint32_t type) {
  return type >= static_cast<uint32_t>(PageMessage::kHello) &&
         type <= static_cast<uint32_t>(PageMessage::kUncaughtError);
}

bool IsConsoleLevel(uint8_t level) {
  return level >= static_cast<uint8_t>(ConsoleLevel::kDebug) &&
         level <= static_cast<uint8_t>(ConsoleLevel::kError);
}

// Text in a message can hold any character that a script can, U+FFFF too.
bool IsText(std::string_view text) {
  return base::IsStringUTF8AllowingNoncharacters(text);
}

std::string UnknownType(uint32_t type) {
  return base::StrCat(
      {"message type ", base::NumberToString(type), " is unknown"});
}

std::string WrongWay(uint32_t type, std::string_view goes_to) {
  return base::StrCat({"message type ", base::NumberToString(type),
                       " only goes to ", goes_to});
}

std::string WrongSize(std::string_view message, size_t size) {
  return base::StrCat(
      {"a ", message, " of ", base::NumberToString(size), " bytes"});
}

base::expected<uint32_t, std::string> ReadHello(std::string_view payload) {
  if (payload.size() != 4) {
    return base::unexpected(WrongSize("hello", payload.size()));
  }
  return DecodeUint32(payload);
}

base::expected<std::string, std::string> ReadRefused(std::string payload) {
  if (!IsText(payload)) {
    return base::unexpected("a refusal that is not UTF-8");
  }
  return base::ok(std::move(payload));
}

base::expected<PageStart, std::string> ReadStart(std::string_view payload) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
  if (!dict) {
    return base::unexpected("a start that is not a JSON object");
  }
  PageStart start;
  bool has_time_zone = false;
  bool has_locale = false;
  for (const auto [key, value] : *dict) {
    if (!value.is_string()) {
      return base::unexpected(
          base::StrCat({"a start whose \"", key, "\" is not a string"}));
    }
    if (key == kTimeZoneKey) {
      start.time_zone = value.GetString();
      has_time_zone = true;
    } else if (key == kLocaleKey) {
      start.locale = value.GetString();
      has_locale = true;
    } else {
      return base::unexpected(
          base::StrCat({"a start with the unknown key \"", key, "\""}));
    }
  }
  if (!has_time_zone || !has_locale) {
    return base::unexpected("a start without a time zone and a locale");
  }
  return start;
}

}  // namespace

PageStart::PageStart() = default;
PageStart::PageStart(const PageStart&) = default;
PageStart& PageStart::operator=(const PageStart&) = default;
PageStart::~PageStart() = default;

PageEvaluated::PageEvaluated() = default;
PageEvaluated::PageEvaluated(PageEvaluated&&) = default;
PageEvaluated& PageEvaluated::operator=(PageEvaluated&&) = default;
PageEvaluated::~PageEvaluated() = default;

PageConsoleMessage::PageConsoleMessage() = default;
PageConsoleMessage::PageConsoleMessage(PageConsoleMessage&&) = default;
PageConsoleMessage& PageConsoleMessage::operator=(PageConsoleMessage&&) =
    default;
PageConsoleMessage::~PageConsoleMessage() = default;

FrameFromPage::FrameFromPage() = default;
FrameFromPage::FrameFromPage(FrameFromPage&&) = default;
FrameFromPage& FrameFromPage::operator=(FrameFromPage&&) = default;
FrameFromPage::~FrameFromPage() = default;

FrameFromSession::FrameFromSession() = default;
FrameFromSession::FrameFromSession(FrameFromSession&&) = default;
FrameFromSession& FrameFromSession::operator=(FrameFromSession&&) = default;
FrameFromSession::~FrameFromSession() = default;

std::string EncodeFrame(PageMessage type, std::string_view payload) {
  CHECK_LE(payload.size(), kMaxPayloadSize);
  return base::StrCat({EncodeUint32(static_cast<uint32_t>(payload.size())),
                       EncodeUint32(static_cast<uint32_t>(type)), payload});
}

std::string EncodeHello() {
  return EncodeFrame(PageMessage::kHello, EncodeUint32(kPageProtocolVersion));
}

std::string EncodeStart(const PageStart& start) {
  base::DictValue dict;
  dict.Set(kTimeZoneKey, start.time_zone);
  dict.Set(kLocaleKey, start.locale);
  return EncodeFrame(PageMessage::kStart, *base::WriteJson(dict));
}

std::string EncodeEvaluate(uint64_t call, std::string_view script) {
  CHECK_LE(script.size(), kMaxScriptSize);
  return EncodeFrame(PageMessage::kEvaluate,
                     base::StrCat({EncodeUint64(call), script}));
}

std::string EncodeEvaluated(const PageEvaluated& evaluated) {
  CHECK_LE(evaluated.text.size(), kMaxEvaluatedTextSize);
  const char outcome = static_cast<char>(evaluated.failed ? kFailed : kHasResult);
  return EncodeFrame(
      PageMessage::kEvaluated,
      base::StrCat({EncodeUint64(evaluated.call), std::string_view(&outcome, 1),
                    evaluated.text}));
}

std::string EncodeConsole(const PageConsoleMessage& message) {
  CHECK_LE(message.text.size(), kMaxConsoleTextSize);
  const char level = static_cast<char>(message.level);
  return EncodeFrame(PageMessage::kConsole,
                     base::StrCat({std::string_view(&level, 1), message.text}));
}

std::string EncodeUncaughtError(std::string_view text) {
  return EncodeFrame(PageMessage::kUncaughtError, text);
}

base::expected<FrameFromPage, std::string> ReadFrameFromPage(
    uint32_t type,
    std::string payload) {
  if (!IsKnown(type)) {
    return base::unexpected(UnknownType(type));
  }
  FrameFromPage frame;
  frame.type = static_cast<PageMessage>(type);
  switch (frame.type) {
    case PageMessage::kHello: {
      base::expected<uint32_t, std::string> version = ReadHello(payload);
      if (!version.has_value()) {
        return base::unexpected(std::move(version).error());
      }
      frame.version = *version;
      return frame;
    }
    case PageMessage::kRefused: {
      base::expected<std::string, std::string> reason =
          ReadRefused(std::move(payload));
      if (!reason.has_value()) {
        return base::unexpected(std::move(reason).error());
      }
      frame.reason = *std::move(reason);
      return frame;
    }
    case PageMessage::kEvaluated: {
      if (payload.size() < 9) {
        return base::unexpected(WrongSize("result", payload.size()));
      }
      const std::string_view bytes = payload;
      const uint8_t outcome = static_cast<uint8_t>(bytes[8]);
      if (outcome != kHasResult && outcome != kFailed) {
        return base::unexpected(base::StrCat(
            {"a result with the outcome ", base::NumberToString(outcome)}));
      }
      if (!IsText(bytes.substr(9))) {
        return base::unexpected("a result that is not UTF-8");
      }
      frame.evaluated.call = DecodeUint64(bytes.substr(0, 8));
      frame.evaluated.failed = outcome == kFailed;
      frame.evaluated.text = payload.substr(9);
      return frame;
    }
    case PageMessage::kConsole: {
      if (payload.empty()) {
        return base::unexpected(WrongSize("console message", payload.size()));
      }
      const std::string_view bytes = payload;
      const uint8_t level = static_cast<uint8_t>(bytes[0]);
      if (!IsConsoleLevel(level)) {
        return base::unexpected(base::StrCat(
            {"a console message of level ", base::NumberToString(level)}));
      }
      if (!IsText(bytes.substr(1))) {
        return base::unexpected("a console message that is not UTF-8");
      }
      frame.console.level = static_cast<ConsoleLevel>(level);
      frame.console.text = payload.substr(1);
      return frame;
    }
    case PageMessage::kUncaughtError:
      if (!IsText(payload)) {
        return base::unexpected("an uncaught error that is not UTF-8");
      }
      frame.uncaught_error = std::move(payload);
      return frame;
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
      return base::unexpected(WrongWay(type, "a Page"));
  }
  NOTREACHED();
}

base::expected<FrameFromSession, std::string> ReadFrameFromSession(
    uint32_t type,
    std::string payload) {
  if (!IsKnown(type)) {
    return base::unexpected(UnknownType(type));
  }
  FrameFromSession frame;
  frame.type = static_cast<PageMessage>(type);
  switch (frame.type) {
    case PageMessage::kHello: {
      base::expected<uint32_t, std::string> version = ReadHello(payload);
      if (!version.has_value()) {
        return base::unexpected(std::move(version).error());
      }
      frame.version = *version;
      return frame;
    }
    case PageMessage::kRefused: {
      base::expected<std::string, std::string> reason =
          ReadRefused(std::move(payload));
      if (!reason.has_value()) {
        return base::unexpected(std::move(reason).error());
      }
      frame.reason = *std::move(reason);
      return frame;
    }
    case PageMessage::kStart: {
      base::expected<PageStart, std::string> start = ReadStart(payload);
      if (!start.has_value()) {
        return base::unexpected(std::move(start).error());
      }
      frame.start = *std::move(start);
      return frame;
    }
    case PageMessage::kEvaluate: {
      if (payload.size() < 8) {
        return base::unexpected(WrongSize("script", payload.size()));
      }
      const std::string_view bytes = payload;
      if (!IsText(bytes.substr(8))) {
        return base::unexpected("a script that is not UTF-8");
      }
      frame.call = DecodeUint64(bytes.substr(0, 8));
      frame.script = payload.substr(8);
      return frame;
    }
    case PageMessage::kEvaluated:
    case PageMessage::kConsole:
    case PageMessage::kUncaughtError:
      return base::unexpected(WrongWay(type, "a Session"));
  }
  NOTREACHED();
}

FrameReader::FrameReader() = default;
FrameReader::~FrameReader() = default;

void FrameReader::Append(base::span<const uint8_t> bytes) {
  // What was taken already goes, so that the buffer holds one frame at most and
  // what has arrived of the next.
  if (offset_ > 0) {
    buffer_.erase(0, offset_);
    offset_ = 0;
  }
  buffer_.append(base::as_string_view(bytes));
}

FrameReader::Result FrameReader::Next(uint32_t& type,
                                      std::string& payload,
                                      size_t& size) {
  const std::string_view rest = std::string_view(buffer_).substr(offset_);
  if (rest.size() < kFrameHeaderSize) {
    return Result::kNeedMore;
  }
  size = DecodeUint32(rest.substr(0, 4));
  if (size > kMaxPayloadSize) {
    return Result::kTooLarge;
  }
  if (rest.size() - kFrameHeaderSize < size) {
    return Result::kNeedMore;
  }
  type = DecodeUint32(rest.substr(4, 4));
  payload.assign(rest.substr(kFrameHeaderSize, size));
  offset_ += kFrameHeaderSize + size;
  return Result::kFrame;
}

}  // namespace mitto
