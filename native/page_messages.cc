#include "mitto/page_messages.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "base/bit_cast.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/values.h"
#include "mitto/header_rules.h"

namespace mitto {
namespace {

constexpr std::string_view kTimeZoneKey = "time_zone";
constexpr std::string_view kLocaleKey = "locale";
constexpr std::string_view kWindowStateKey = "window_state";
constexpr std::string_view kContrastThemeKey = "contrast_theme";
constexpr std::string_view kInvertedColorsKey = "inverted_colors";
constexpr std::string_view kMemoryKey = "memory";
constexpr std::string_view kScaleKey = "scale";
constexpr std::string_view kNavigatorKey = "navigator";
constexpr std::string_view kConnectionKey = "connection";

constexpr std::string_view kMaximised = "maximised";
constexpr std::string_view kNormal = "normal";
constexpr std::string_view kNotInverted = "none";
constexpr std::string_view kInverted = "inverted";

// Windows' contrast themes, those of Windows 11 and then those of Windows 10,
// after "none".
constexpr auto kContrastThemes = std::to_array<std::string_view>(
    {"none", "aquatic", "desert", "dusk", "night-sky", "high-contrast-1",
     "high-contrast-2", "high-contrast-black", "high-contrast-white"});

// The settings of a start that take one of a few values, each a field of
// PageStart, with the values it takes.
struct StartSetting {
  std::string_view key;
  std::string PageStart::* field;
  std::vector<std::string_view> values;
};
const std::vector<StartSetting>& StartSettings() {
  static const base::NoDestructor<std::vector<StartSetting>> settings(
      std::vector<StartSetting>{
          {"color_scheme", &PageStart::color_scheme, {"light", "dark"}},
          {"reduced_motion",
           &PageStart::reduced_motion,
           {"no-preference", "reduce"}},
          {"reduced_transparency",
           &PageStart::reduced_transparency,
           {"no-preference", "reduce"}},
          {"effective_type",
           &PageStart::effective_type,
           {"slow-2g", "2g", "3g", "4g"}},
      });
  return *settings;
}

// What Chrome on a desktop gives for the memory, in GiB.
constexpr auto kMemorySizes = std::to_array<int>({2, 4, 8, 16, 32});

// The scales of Windows' display settings run from 100% to 500%.
constexpr double kLeastScale = 1;
constexpr double kMostScale = 5;

// Names of time zones, locales and settings are short and ASCII. Which time
// zones and locales exist only the Page knows, because only it holds ICU's
// whole data.
constexpr size_t kMaxNameSize = 64;

// The texts of the navigator: a user agent and the languages of Chrome's
// setting are short in Chrome, and the other values shorter.
constexpr size_t kMaxNavigatorTextSize = 4096;
constexpr size_t kMaxUserAgentDataTextSize = 256;
constexpr size_t kMaxUserAgentDataListSize = 32;

// The most that a connection gives: Chrome caps the round trip at 3 seconds
// and the throughput at 10 Mbps.
constexpr uint32_t kMostRtt = 3000;
constexpr double kMostDownlink = 10;

// The names of a start, each a field of PageStart.
struct StartName {
  std::string_view key;
  std::string PageStart::* field;
};
constexpr auto kStartNames = std::to_array<StartName>({
    {kTimeZoneKey, &PageStart::time_zone},
    {kLocaleKey, &PageStart::locale},
    {kContrastThemeKey, &PageStart::contrast_theme},
});

// The numbers of a start, each a field of PageStart, and the least and the
// most that a Machine can have. Windows holds Chrome to 64 logical processors,
// in one group of them.
struct StartNumber {
  std::string_view key;
  int PageStart::* field;
  int least;
  int most;
};
constexpr int kMost = std::numeric_limits<int>::max();
constexpr int kLeast = std::numeric_limits<int>::min();
constexpr auto kStartNumbers = std::to_array<StartNumber>({
    {"processors", &PageStart::processors, 1, 64},
    {"touch_points", &PageStart::touch_points, 0, 256},
    {"work_area_x", &PageStart::work_area_x, 0, kMost},
    {"work_area_y", &PageStart::work_area_y, 0, kMost},
    {"work_area_width", &PageStart::work_area_width, 1, kMost},
    {"work_area_height", &PageStart::work_area_height, 1, kMost},
    {"window_x", &PageStart::window_x, kLeast, kMost},
    {"window_y", &PageStart::window_y, kLeast, kMost},
    {"window_width", &PageStart::window_width, 1, kMost},
    {"window_height", &PageStart::window_height, 1, kMost},
    {"screen_width", &PageStart::screen_width, 1, kMost},
    {"screen_height", &PageStart::screen_height, 1, kMost},
    {"viewport_width", &PageStart::viewport_width, 1, kMost},
    {"viewport_height", &PageStart::viewport_height, 1, kMost},
});

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
         type <= static_cast<uint32_t>(PageMessage::kCancel);
}

bool IsRequestKind(uint8_t kind) {
  return kind == static_cast<uint8_t>(PageRequestKind::kScript) ||
         kind == static_cast<uint8_t>(PageRequestKind::kFetch);
}

std::string EncodeInt32(int32_t value) {
  return EncodeUint32(static_cast<uint32_t>(value));
}

int32_t DecodeInt32(std::string_view bytes) {
  return static_cast<int32_t>(DecodeUint32(bytes));
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

bool IsName(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameSize &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' ||
                  c == '+' || c == '/';
         });
}

bool IsStartKey(std::string_view key) {
  return key == kWindowStateKey || key == kInvertedColorsKey ||
         key == kMemoryKey || key == kScaleKey || key == kNavigatorKey ||
         std::ranges::any_of(
             kStartNames,
             [key](const StartName& name) { return name.key == key; }) ||
         std::ranges::any_of(
             kStartNumbers,
             [key](const StartNumber& number) { return number.key == key; }) ||
         std::ranges::any_of(
             StartSettings(),
             [key](const StartSetting& setting) { return setting.key == key; });
}

std::string NotA(std::string_view key, std::string_view what) {
  return base::StrCat({"a start whose \"", key, "\" is not ", what});
}

// Whether `text` is UTF-8 of at most `most` bytes.
bool IsTextOfAtMost(std::string_view text, size_t most) {
  return text.size() <= most && IsText(text);
}

// Returns the text at `key` of `dict`, or nullopt if it is not text of at most
// `most` bytes.
std::optional<std::string> FindText(const base::DictValue& dict,
                                    std::string_view key,
                                    size_t most) {
  const std::string* const value = dict.FindString(key);
  if (!value || !IsTextOfAtMost(*value, most)) {
    return std::nullopt;
  }
  return *value;
}

// Returns the brands of a list such as sec-ch-ua's, each {"brand", "version"},
// or nullopt if `list` is not that.
std::optional<std::vector<PageBrandVersion>> ReadBrands(
    const base::ListValue* list) {
  if (!list || list->size() > kMaxUserAgentDataListSize) {
    return std::nullopt;
  }
  std::vector<PageBrandVersion> brands;
  for (const base::Value& item : *list) {
    if (!item.is_dict() || item.GetDict().size() != 2) {
      return std::nullopt;
    }
    std::optional<std::string> brand =
        FindText(item.GetDict(), "brand", kMaxUserAgentDataTextSize);
    std::optional<std::string> version =
        FindText(item.GetDict(), "version", kMaxUserAgentDataTextSize);
    if (!brand || !version) {
      return std::nullopt;
    }
    brands.push_back({*std::move(brand), *std::move(version)});
  }
  return brands;
}

base::ListValue WriteBrands(const std::vector<PageBrandVersion>& brands) {
  base::ListValue list;
  for (const PageBrandVersion& brand : brands) {
    list.Append(base::DictValue()
                    .Set("brand", brand.brand)
                    .Set("version", brand.version));
  }
  return list;
}

// The texts of the user agent's data, each a field of PageUserAgentData.
struct UserAgentDataText {
  std::string_view key;
  std::string PageUserAgentData::* field;
};
constexpr auto kUserAgentDataTexts = std::to_array<UserAgentDataText>({
    {"platform", &PageUserAgentData::platform},
    {"platform_version", &PageUserAgentData::platform_version},
    {"architecture", &PageUserAgentData::architecture},
    {"bitness", &PageUserAgentData::bitness},
    {"model", &PageUserAgentData::model},
    {"ua_full_version", &PageUserAgentData::ua_full_version},
});

// The keys of the user agent's data beside its texts.
constexpr std::string_view kBrandsKey = "brands";
constexpr std::string_view kFullVersionListKey = "full_version_list";
constexpr std::string_view kMobileKey = "mobile";
constexpr std::string_view kWow64Key = "wow64";
constexpr std::string_view kFormFactorsKey = "form_factors";
constexpr size_t kUserAgentDataKeys = kUserAgentDataTexts.size() + 5;

std::optional<PageUserAgentData> ReadUserAgentData(
    const base::DictValue* dict) {
  if (!dict || dict->size() != kUserAgentDataKeys) {
    return std::nullopt;
  }
  PageUserAgentData data;
  for (const UserAgentDataText& text : kUserAgentDataTexts) {
    std::optional<std::string> value =
        FindText(*dict, text.key, kMaxUserAgentDataTextSize);
    if (!value) {
      return std::nullopt;
    }
    data.*text.field = *std::move(value);
  }
  std::optional<std::vector<PageBrandVersion>> brands =
      ReadBrands(dict->FindList(kBrandsKey));
  std::optional<std::vector<PageBrandVersion>> full_version_list =
      ReadBrands(dict->FindList(kFullVersionListKey));
  const std::optional<bool> mobile = dict->FindBool(kMobileKey);
  const std::optional<bool> wow64 = dict->FindBool(kWow64Key);
  const base::ListValue* const form_factors = dict->FindList(kFormFactorsKey);
  if (!brands || !full_version_list || !mobile || !wow64 || !form_factors ||
      form_factors->size() > kMaxUserAgentDataListSize) {
    return std::nullopt;
  }
  for (const base::Value& form_factor : *form_factors) {
    if (!form_factor.is_string() ||
        !IsTextOfAtMost(form_factor.GetString(), kMaxUserAgentDataTextSize)) {
      return std::nullopt;
    }
    data.form_factors.push_back(form_factor.GetString());
  }
  data.brands = *std::move(brands);
  data.full_version_list = *std::move(full_version_list);
  data.mobile = *mobile;
  data.wow64 = *wow64;
  return data;
}

base::DictValue WriteUserAgentData(const PageUserAgentData& data) {
  base::DictValue dict;
  for (const UserAgentDataText& text : kUserAgentDataTexts) {
    dict.Set(text.key, data.*text.field);
  }
  base::ListValue form_factors;
  for (const std::string& form_factor : data.form_factors) {
    form_factors.Append(form_factor);
  }
  return std::move(dict)
      .Set(kBrandsKey, WriteBrands(data.brands))
      .Set(kFullVersionListKey, WriteBrands(data.full_version_list))
      .Set(kMobileKey, data.mobile)
      .Set(kWow64Key, data.wow64)
      .Set(kFormFactorsKey, std::move(form_factors));
}

std::optional<PageNavigator> ReadNavigator(const base::DictValue* dict) {
  if (!dict || dict->size() != 4) {
    return std::nullopt;
  }
  PageNavigator navigator;
  std::optional<std::string> user_agent =
      FindText(*dict, "user_agent", kMaxNavigatorTextSize);
  std::optional<std::string> platform =
      FindText(*dict, "platform", kMaxUserAgentDataTextSize);
  std::optional<std::string> languages =
      FindText(*dict, "languages", kMaxNavigatorTextSize);
  std::optional<PageUserAgentData> data =
      ReadUserAgentData(dict->FindDict("user_agent_data"));
  if (!user_agent || !platform || !languages || !data) {
    return std::nullopt;
  }
  navigator.user_agent = *std::move(user_agent);
  navigator.platform = *std::move(platform);
  navigator.languages = *std::move(languages);
  navigator.user_agent_data = *std::move(data);
  return navigator;
}

base::DictValue WriteNavigator(const PageNavigator& navigator) {
  return base::DictValue()
      .Set("user_agent", navigator.user_agent)
      .Set("platform", navigator.platform)
      .Set("languages", navigator.languages)
      .Set("user_agent_data", WriteUserAgentData(navigator.user_agent_data));
}

bool IsConnection(const PageConnection& connection) {
  return connection.rtt <= kMostRtt && connection.rtt % 50 == 0 &&
         connection.downlink >= 0 && connection.downlink <= kMostDownlink;
}

std::optional<PageConnection> ReadConnection(const base::DictValue* dict) {
  if (!dict || dict->size() != 2) {
    return std::nullopt;
  }
  const std::optional<int> rtt = dict->FindInt("rtt");
  const std::optional<double> downlink = dict->FindDouble("downlink");
  if (!rtt || !downlink || *rtt < 0) {
    return std::nullopt;
  }
  const PageConnection connection = {.rtt = static_cast<uint32_t>(*rtt),
                                     .downlink = *downlink};
  if (!IsConnection(connection)) {
    return std::nullopt;
  }
  return connection;
}

// What the library sends: the "start" option with the connection of
// about:blank's document.
base::expected<PageStart, std::string> ReadStartPayload(
    std::string_view payload) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
  if (!dict) {
    return base::unexpected("a start that is not a JSON object");
  }
  std::optional<base::Value> connection_value = dict->Extract(kConnectionKey);
  const std::optional<PageConnection> connection =
      ReadConnection(connection_value && connection_value->is_dict()
                         ? &connection_value->GetDict()
                         : nullptr);
  if (!connection) {
    return base::unexpected(
        NotA(kConnectionKey, "an rtt and a downlink that Chrome gives"));
  }
  base::expected<PageStart, std::string> start = ReadStart(*dict);
  if (start.has_value()) {
    start->connection = *connection;
  }
  return start;
}

// Takes the next part of `bytes`, after its size as a 32-bit number, into
// `part`. Returns false if `bytes` does not hold as much.
bool TakeSized(std::string_view& bytes, std::string_view& part) {
  if (bytes.size() < 4) {
    return false;
  }
  const uint32_t size = DecodeUint32(bytes.substr(0, 4));
  if (size > bytes.size() - 4) {
    return false;
  }
  part = bytes.substr(4, size);
  bytes.remove_prefix(4 + size);
  return true;
}

// Whether `lines` are header lines that a page's script may send: each
// "name: value" and a line feed, with a token for a name, a value with no NUL
// or CR, and no forbidden request header.
std::optional<std::string> CheckHeaderLines(std::string_view lines) {
  while (!lines.empty()) {
    const size_t end = lines.find('\n');
    if (end == std::string_view::npos) {
      return "header lines that do not end in a line feed";
    }
    const std::string_view line = lines.substr(0, end);
    lines.remove_prefix(end + 1);
    const size_t colon = line.find(": ");
    if (colon == std::string_view::npos) {
      return "a header line without a name";
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = line.substr(colon + 2);
    if (!IsHttpToken(name) || !IsValidHeaderValue(value)) {
      return "a header line that is not valid";
    }
    if (IsForbiddenRequestHeader(name, value)) {
      return base::StrCat({"the forbidden request header ", name});
    }
  }
  return std::nullopt;
}

base::expected<PageRequest, std::string> ReadRequest(std::string_view bytes) {
  if (bytes.size() < 9) {
    return base::unexpected(WrongSize("request", bytes.size()));
  }
  PageRequest request;
  request.number = DecodeUint64(bytes.substr(0, 8));
  if (request.number == kNavigationRequest) {
    return base::unexpected("a request numbered 0, which is the navigation's");
  }
  const uint8_t kind = static_cast<uint8_t>(bytes[8]);
  if (!IsRequestKind(kind)) {
    return base::unexpected(base::StrCat(
        {"a request of the unknown kind ", base::NumberToString(kind)}));
  }
  request.kind = static_cast<PageRequestKind>(kind);
  if (bytes.size() < 10 || static_cast<uint8_t>(bytes[9]) > 1) {
    return base::unexpected("a request with a mode that is not one");
  }
  request.same_origin = bytes[9] == 1;
  bytes.remove_prefix(10);
  std::string_view url;
  std::string_view method;
  std::string_view headers;
  if (!TakeSized(bytes, url) || !TakeSized(bytes, method) ||
      !TakeSized(bytes, headers) || bytes.empty() ||
      static_cast<uint8_t>(bytes[0]) > 1 ||
      (bytes[0] == 0 && bytes.size() != 1)) {
    return base::unexpected("a request that does not hold what it says");
  }
  if (!IsText(url)) {
    return base::unexpected("a request whose URL is not UTF-8");
  }
  request.url = std::string(url);
  if (!IsHttpToken(method) || IsForbiddenMethod(method)) {
    return base::unexpected("a request with a method that a page cannot send");
  }
  request.method = std::string(method);
  if (std::optional<std::string> why = CheckHeaderLines(headers)) {
    return base::unexpected(base::StrCat({"a request with ", *why}));
  }
  request.headers = std::string(headers);
  if (bytes[0] == 1) {
    request.body = std::string(bytes.substr(1));
  }
  const bool without_body = base::EqualsCaseInsensitiveASCII(method, "GET") ||
                            base::EqualsCaseInsensitiveASCII(method, "HEAD");
  if (request.body && without_body) {
    return base::unexpected("a request with a body and the method GET or HEAD");
  }
  if (request.kind == PageRequestKind::kScript &&
      (request.method != "GET" || !request.headers.empty() || request.body ||
       request.same_origin)) {
    return base::unexpected(
        "a script request with a method, header lines, a body or a mode");
  }
  return request;
}

base::expected<PageCredit, std::string> ReadCredit(std::string_view payload) {
  if (payload.size() != 12) {
    return base::unexpected(WrongSize("credit", payload.size()));
  }
  PageCredit credit;
  credit.number = DecodeUint64(payload.substr(0, 8));
  credit.bytes = DecodeUint32(payload.substr(8, 4));
  if (credit.bytes == 0 || credit.bytes > kBodyWindow) {
    return base::unexpected(base::StrCat(
        {"a credit of ", base::NumberToString(credit.bytes), " bytes"}));
  }
  return credit;
}

base::expected<PageResponseStarted, std::string> ReadResponseStarted(
    std::string_view bytes) {
  if (bytes.size() < 34) {
    return base::unexpected(WrongSize("response", bytes.size()));
  }
  PageResponseStarted response;
  response.number = DecodeUint64(bytes.substr(0, 8));
  response.status = DecodeInt32(bytes.substr(8, 4));
  const uint8_t redirected = static_cast<uint8_t>(bytes[12]);
  const uint8_t cors = static_cast<uint8_t>(bytes[13]);
  response.navigation_start =
      static_cast<int64_t>(DecodeUint64(bytes.substr(14, 8)));
  response.connection.rtt = DecodeUint32(bytes.substr(22, 4));
  response.connection.downlink =
      base::bit_cast<double>(DecodeUint64(bytes.substr(26, 8)));
  bytes.remove_prefix(34);
  std::string_view url;
  std::string_view status_text;
  if (response.status < 0 || redirected > 1 || cors > 1 ||
      !IsConnection(response.connection) || !TakeSized(bytes, url) ||
      !TakeSized(bytes, status_text)) {
    return base::unexpected("a response that does not hold what it says");
  }
  if (!IsText(url)) {
    return base::unexpected("a response whose URL is not UTF-8");
  }
  response.redirected = redirected == 1;
  response.cors = cors == 1;
  response.url = std::string(url);
  response.status_text = std::string(status_text);
  response.headers = std::string(bytes);
  return response;
}

base::expected<PageResponseBody, std::string> ReadResponseBody(
    std::string payload) {
  if (payload.size() < 9 || payload.size() > 8 + kBodyWindow) {
    return base::unexpected(WrongSize("body", payload.size()));
  }
  PageResponseBody body;
  body.number = DecodeUint64(std::string_view(payload).substr(0, 8));
  body.bytes = payload.substr(8);
  return body;
}

base::expected<PageResponseEnded, std::string> ReadResponseEnded(
    std::string_view payload) {
  if (payload.size() != 12) {
    return base::unexpected(WrongSize("end of a response", payload.size()));
  }
  PageResponseEnded ended;
  ended.number = DecodeUint64(payload.substr(0, 8));
  ended.net_error = DecodeInt32(payload.substr(8, 4));
  if (ended.net_error > 0) {
    return base::unexpected(
        base::StrCat({"the end of a response with the net error ",
                      base::NumberToString(ended.net_error)}));
  }
  return ended;
}

}  // namespace

PageRequest::PageRequest() = default;
PageRequest::PageRequest(PageRequest&&) = default;
PageRequest& PageRequest::operator=(PageRequest&&) = default;
PageRequest::~PageRequest() = default;

PageResponseStarted::PageResponseStarted() = default;
PageResponseStarted::PageResponseStarted(PageResponseStarted&&) = default;
PageResponseStarted& PageResponseStarted::operator=(PageResponseStarted&&) =
    default;
PageResponseStarted::~PageResponseStarted() = default;

PageResponseBody::PageResponseBody() = default;
PageResponseBody::PageResponseBody(PageResponseBody&&) = default;
PageResponseBody& PageResponseBody::operator=(PageResponseBody&&) = default;
PageResponseBody::~PageResponseBody() = default;

PageUserAgentData::PageUserAgentData() = default;
PageUserAgentData::PageUserAgentData(const PageUserAgentData&) = default;
PageUserAgentData& PageUserAgentData::operator=(const PageUserAgentData&) =
    default;
PageUserAgentData::~PageUserAgentData() = default;

PageNavigator::PageNavigator() = default;
PageNavigator::PageNavigator(const PageNavigator&) = default;
PageNavigator& PageNavigator::operator=(const PageNavigator&) = default;
PageNavigator::~PageNavigator() = default;

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
  for (const StartName& name : kStartNames) {
    dict.Set(name.key, start.*name.field);
  }
  for (const StartNumber& number : kStartNumbers) {
    dict.Set(number.key, start.*number.field);
  }
  dict.Set(kWindowStateKey, start.window_state == WindowState::kMaximised
                                ? kMaximised
                                : kNormal);
  dict.Set(kInvertedColorsKey,
           start.inverted_colors ? kInverted : kNotInverted);
  for (const StartSetting& setting : StartSettings()) {
    dict.Set(setting.key, start.*setting.field);
  }
  dict.Set(kMemoryKey, start.memory);
  dict.Set(kScaleKey, start.scale);
  dict.Set(kNavigatorKey, WriteNavigator(start.navigator));
  dict.Set(kConnectionKey,
           base::DictValue()
               .Set("rtt", static_cast<int>(start.connection.rtt))
               .Set("downlink", start.connection.downlink));
  return EncodeFrame(PageMessage::kStart, *base::WriteJson(dict));
}

base::expected<PageStart, std::string> ReadStart(const base::DictValue& dict) {
  for (const auto [key, value] : dict) {
    if (!IsStartKey(key)) {
      return base::unexpected(
          base::StrCat({"a start with the unknown key \"", key, "\""}));
    }
  }
  PageStart start;
  for (const StartName& name : kStartNames) {
    const std::string* const value = dict.FindString(name.key);
    if (!value || !IsName(*value)) {
      return base::unexpected(NotA(name.key, "a name"));
    }
    start.*name.field = *value;
  }
  if (std::ranges::find(kContrastThemes, start.contrast_theme) ==
      kContrastThemes.end()) {
    return base::unexpected(NotA(kContrastThemeKey, "a contrast theme"));
  }
  for (const StartNumber& number : kStartNumbers) {
    const std::optional<int> value = dict.FindInt(number.key);
    if (!value || *value < number.least || *value > number.most) {
      return base::unexpected(NotA(
          number.key, base::StrCat({"a whole number from ",
                                    base::NumberToString(number.least), " to ",
                                    base::NumberToString(number.most)})));
    }
    start.*number.field = *value;
  }
  const std::string* const window_state = dict.FindString(kWindowStateKey);
  if (!window_state ||
      (*window_state != kMaximised && *window_state != kNormal)) {
    return base::unexpected(
        NotA(kWindowStateKey, "\"maximised\" or \"normal\""));
  }
  start.window_state = *window_state == kMaximised ? WindowState::kMaximised
                                                   : WindowState::kNormal;
  const std::string* const inverted = dict.FindString(kInvertedColorsKey);
  if (!inverted || (*inverted != kInverted && *inverted != kNotInverted)) {
    return base::unexpected(
        NotA(kInvertedColorsKey, "\"none\" or \"inverted\""));
  }
  start.inverted_colors = *inverted == kInverted;
  for (const StartSetting& setting : StartSettings()) {
    const std::string* const value = dict.FindString(setting.key);
    if (!value ||
        std::ranges::find(setting.values, *value) == setting.values.end()) {
      std::string values;
      for (const std::string_view allowed : setting.values) {
        base::StrAppend(&values,
                        {values.empty() ? "" : ", ", "\"", allowed, "\""});
      }
      return base::unexpected(
          NotA(setting.key, base::StrCat({"one of ", values})));
    }
    start.*setting.field = *value;
  }
  const std::optional<int> memory = dict.FindInt(kMemoryKey);
  if (!memory ||
      std::ranges::find(kMemorySizes, *memory) == kMemorySizes.end()) {
    return base::unexpected(NotA(kMemoryKey, "2, 4, 8, 16 or 32"));
  }
  start.memory = *memory;
  // A whole number in JSON reads as an int, which FindDouble() takes too.
  const std::optional<double> scale = dict.FindDouble(kScaleKey);
  if (!scale || !(*scale >= kLeastScale && *scale <= kMostScale)) {
    return base::unexpected(NotA(kScaleKey, "a number from 1 to 5"));
  }
  start.scale = *scale;
  std::optional<PageNavigator> navigator =
      ReadNavigator(dict.FindDict(kNavigatorKey));
  if (!navigator) {
    return base::unexpected(NotA(
        kNavigatorKey, "a user agent, a platform, languages and their data"));
  }
  start.navigator = *std::move(navigator);
  return start;
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

std::string EncodeRequest(const PageRequest& request) {
  const char kind = static_cast<char>(request.kind);
  const char same_origin = request.same_origin ? 1 : 0;
  const char has_body = request.body ? 1 : 0;
  return EncodeFrame(
      PageMessage::kRequest,
      base::StrCat({EncodeUint64(request.number), std::string_view(&kind, 1),
                    std::string_view(&same_origin, 1),
                    EncodeUint32(static_cast<uint32_t>(request.url.size())),
                    request.url,
                    EncodeUint32(static_cast<uint32_t>(request.method.size())),
                    request.method,
                    EncodeUint32(static_cast<uint32_t>(request.headers.size())),
                    request.headers, std::string_view(&has_body, 1),
                    request.body.value_or("")}));
}

std::string EncodeCancel(uint64_t number) {
  return EncodeFrame(PageMessage::kCancel, EncodeUint64(number));
}

std::string EncodeCredit(const PageCredit& credit) {
  return EncodeFrame(
      PageMessage::kCredit,
      base::StrCat({EncodeUint64(credit.number), EncodeUint32(credit.bytes)}));
}

std::string EncodeGetCookies(uint64_t question) {
  return EncodeFrame(PageMessage::kGetCookies, EncodeUint64(question));
}

std::string EncodeSetCookie(std::string_view line) {
  return EncodeFrame(PageMessage::kSetCookie, line);
}

std::string EncodeLoaded() {
  return EncodeFrame(PageMessage::kLoaded, "");
}

std::string EncodeResponseStarted(const PageResponseStarted& response) {
  const char redirected = response.redirected ? 1 : 0;
  const char cors = response.cors ? 1 : 0;
  return EncodeFrame(
      PageMessage::kResponseStarted,
      base::StrCat(
          {EncodeUint64(response.number), EncodeInt32(response.status),
           std::string_view(&redirected, 1), std::string_view(&cors, 1),
           EncodeUint64(static_cast<uint64_t>(response.navigation_start)),
           EncodeUint32(response.connection.rtt),
           EncodeUint64(base::bit_cast<uint64_t>(response.connection.downlink)),
           EncodeUint32(static_cast<uint32_t>(response.url.size())),
           response.url,
           EncodeUint32(static_cast<uint32_t>(response.status_text.size())),
           response.status_text, response.headers}));
}

std::string EncodeResponseBody(uint64_t number, std::string_view bytes) {
  CHECK_LE(bytes.size(), kBodyWindow);
  return EncodeFrame(PageMessage::kResponseBody,
                     base::StrCat({EncodeUint64(number), bytes}));
}

std::string EncodeResponseEnded(const PageResponseEnded& ended) {
  return EncodeFrame(
      PageMessage::kResponseEnded,
      base::StrCat({EncodeUint64(ended.number), EncodeInt32(ended.net_error)}));
}

std::string EncodeCookies(uint64_t answer, std::string_view line) {
  return EncodeFrame(PageMessage::kCookies,
                     base::StrCat({EncodeUint64(answer), line}));
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
    case PageMessage::kRequest: {
      base::expected<PageRequest, std::string> request = ReadRequest(payload);
      if (!request.has_value()) {
        return base::unexpected(std::move(request).error());
      }
      frame.request = *std::move(request);
      return frame;
    }
    case PageMessage::kCredit: {
      base::expected<PageCredit, std::string> credit = ReadCredit(payload);
      if (!credit.has_value()) {
        return base::unexpected(std::move(credit).error());
      }
      frame.credit = *credit;
      return frame;
    }
    case PageMessage::kGetCookies:
      if (payload.size() != 8) {
        return base::unexpected(
            WrongSize("question for cookies", payload.size()));
      }
      frame.cookies_question = DecodeUint64(payload);
      return frame;
    case PageMessage::kSetCookie:
      if (!IsText(payload)) {
        return base::unexpected("a cookie that is not UTF-8");
      }
      frame.cookie_line = std::move(payload);
      return frame;
    case PageMessage::kLoaded:
      if (!payload.empty()) {
        return base::unexpected(WrongSize("load", payload.size()));
      }
      return frame;
    case PageMessage::kCancel:
      if (payload.size() != 8) {
        return base::unexpected(WrongSize("cancel", payload.size()));
      }
      frame.cancelled = DecodeUint64(payload);
      return frame;
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
    case PageMessage::kResponseStarted:
    case PageMessage::kResponseBody:
    case PageMessage::kResponseEnded:
    case PageMessage::kCookies:
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
      base::expected<PageStart, std::string> start = ReadStartPayload(payload);
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
    case PageMessage::kResponseStarted: {
      base::expected<PageResponseStarted, std::string> response =
          ReadResponseStarted(payload);
      if (!response.has_value()) {
        return base::unexpected(std::move(response).error());
      }
      frame.response = *std::move(response);
      return frame;
    }
    case PageMessage::kResponseBody: {
      base::expected<PageResponseBody, std::string> body =
          ReadResponseBody(std::move(payload));
      if (!body.has_value()) {
        return base::unexpected(std::move(body).error());
      }
      frame.body = *std::move(body);
      return frame;
    }
    case PageMessage::kResponseEnded: {
      base::expected<PageResponseEnded, std::string> ended =
          ReadResponseEnded(payload);
      if (!ended.has_value()) {
        return base::unexpected(std::move(ended).error());
      }
      frame.ended = *ended;
      return frame;
    }
    case PageMessage::kCookies:
      if (payload.size() < 8) {
        return base::unexpected(WrongSize("cookies", payload.size()));
      }
      frame.cookies_answer = DecodeUint64(std::string_view(payload));
      frame.cookies = payload.substr(8);
      return frame;
    case PageMessage::kEvaluated:
    case PageMessage::kConsole:
    case PageMessage::kUncaughtError:
    case PageMessage::kRequest:
    case PageMessage::kCredit:
    case PageMessage::kGetCookies:
    case PageMessage::kSetCookie:
    case PageMessage::kLoaded:
    case PageMessage::kCancel:
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
