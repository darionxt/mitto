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
constexpr std::string_view kFeaturesKey = "features";
constexpr std::string_view kMediaDevicesKey = "media_devices";
constexpr std::string_view kAudioInputKey = "audio_input";
constexpr std::string_view kVideoInputKey = "video_input";
constexpr std::string_view kAudioOutputKey = "audio_output";
constexpr std::string_view kEnableBlinkFeaturesKey = "enable_blink_features";
constexpr std::string_view kDisableBlinkFeaturesKey = "disable_blink_features";
constexpr std::string_view kV8FlagsKey = "v8_flags";
constexpr std::string_view kDeprecatedUnloadKey = "deprecated_unload";
constexpr std::string_view kEnabledKey = "enabled";
constexpr std::string_view kAllowListKey = "allow_list";
constexpr std::string_view kByBucketKey = "by_bucket";
constexpr std::string_view kPercentKey = "percent";
constexpr std::string_view kBucketKey = "bucket";

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
// in one group of them, and a Machine's display refreshes from 24 to 1000
// times a second.
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
    {"refresh_rate", &PageStart::refresh_rate, 24, 1000},
});

// The lists of a start's features, each a field of PageFeatures. Blink has
// about 1,300 runtime-enabled features, and gin sets a few dozen V8 flags.
struct FeatureList {
  std::string_view key;
  std::vector<std::string> PageFeatures::* field;
  size_t most;
};
constexpr auto kFeatureLists = std::to_array<FeatureList>({
    {kEnableBlinkFeaturesKey, &PageFeatures::enable_blink_features, 4096},
    {kDisableBlinkFeaturesKey, &PageFeatures::disable_blink_features, 4096},
    {kV8FlagsKey, &PageFeatures::v8_flags, 256},
});
constexpr size_t kMaxFeatureNameSize = 128;
constexpr size_t kMaxV8FlagSize = 256;
// kDeprecateUnloadByAllowList's hosts: the seed's list has a few dozen, and a
// host has at most 253 characters.
constexpr size_t kMaxAllowListSize = 4096;
constexpr size_t kMaxHostSize = 253;

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
  return (type >= static_cast<uint32_t>(PageMessage::kHello) &&
          type <= static_cast<uint32_t>(PageMessage::kBroadcastReceived)) ||
         (type >= static_cast<uint32_t>(PageMessage::kCacheStorage) &&
          type <= static_cast<uint32_t>(PageMessage::kIndexedDbCallback)) ||
         (type >= static_cast<uint32_t>(PageMessage::kDocumentCommitted) &&
          type <= static_cast<uint32_t>(PageMessage::kSameDocumentNavigation));
}

// The frame of a message that names `document`, which comes first.
std::string EncodeDocumentFrame(PageMessage type,
                                uint64_t document,
                                std::string_view payload) {
  return EncodeFrame(type, base::StrCat({EncodeUint64(document), payload}));

}

bool IsRequestKind(uint8_t kind) {
  return kind == static_cast<uint8_t>(PageRequestKind::kScript) ||
         kind == static_cast<uint8_t>(PageRequestKind::kFetch) ||
         kind == static_cast<uint8_t>(PageRequestKind::kNavigation);
}

// The flags of a request.
constexpr uint8_t kSameOriginFlag = 1;
constexpr uint8_t kUserActivationFlag = 2;
constexpr uint8_t kReloadFlag = 4;

std::string EncodeInt32(int32_t value) {
  return EncodeUint32(static_cast<uint32_t>(value));
}

int32_t DecodeInt32(std::string_view bytes) {
  return static_cast<int32_t>(DecodeUint32(bytes));
}

std::string EncodeInt64(int64_t value) {
  return EncodeUint64(static_cast<uint64_t>(value));
}

int64_t DecodeInt64(std::string_view bytes) {
  return static_cast<int64_t>(DecodeUint64(bytes));
}

// The most redirects that //net follows, URLRequest's kMaxRedirects.
constexpr uint32_t kMostRedirects = 20;

// The fixed part of a response's payload, before its URL.
constexpr size_t kResponseStartedFixedSize = 158;

// The times of a PageLoadTiming, in the order they travel.
std::array<int64_t PageLoadTiming::*, 10> LoadTimingTimes() {
  return {&PageLoadTiming::request_start,
          &PageLoadTiming::domain_lookup_start,
          &PageLoadTiming::domain_lookup_end,
          &PageLoadTiming::connect_start,
          &PageLoadTiming::connect_end,
          &PageLoadTiming::ssl_start,
          &PageLoadTiming::send_start,
          &PageLoadTiming::receive_headers_start,
          &PageLoadTiming::receive_headers_end,
          &PageLoadTiming::receive_non_informational_headers_start};
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

// A name of a protocol, as net names them: "h2", "http/1.1", "unknown".
bool IsProtocolName(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameSize &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '/' || c == '.' ||
                  c == '-' || c == '_' || c == '+';
         });
}

// A runtime-enabled feature's name, as runtime_enabled_features.json5 has
// them, so that no comma can split one into two.
bool IsFeatureName(std::string_view name) {
  return !name.empty() && name.size() <= kMaxFeatureNameSize &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_';
         });
}

// A V8 flag as gin writes one, "--name", "--no-name" or "--name=value", whose
// value holds no space, so that V8 reads it as one flag.
bool IsV8Flag(std::string_view flag) {
  if (!flag.starts_with("--") || flag.size() > kMaxV8FlagSize) {
    return false;
  }
  flag.remove_prefix(2u);
  const size_t equals = flag.find('=');
  const std::string_view name = flag.substr(0u, equals);
  const std::string_view value = equals == std::string_view::npos
                                     ? std::string_view()
                                     : flag.substr(equals + 1u);
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-';
  }) && std::ranges::all_of(value, [](char c) {
    return base::IsAsciiPrintable(c) && c != ' ';
  });
}

base::expected<DeprecatedUnload, std::string> ReadDeprecatedUnload(
    const base::Value* value) {
  const auto not_valid = [](std::string_view what) {
    return base::unexpected(base::StrCat(
        {"a start whose features' \"", kDeprecatedUnloadKey, "\" ", what}));
  };
  const base::DictValue* const dict = value ? value->GetIfDict() : nullptr;
  if (!dict) {
    return not_valid("is not an object");
  }
  for (const auto [key, item] : *dict) {
    if (key != kEnabledKey && key != kAllowListKey && key != kByBucketKey) {
      return not_valid(base::StrCat({"has the unknown key \"", key, "\""}));
    }
  }
  DeprecatedUnload unload;
  const std::optional<bool> enabled = dict->FindBool(kEnabledKey);
  if (!enabled) {
    return not_valid("has no \"enabled\" that is true or false");
  }
  unload.enabled = *enabled;
  const base::Value* const allow_list = dict->Find(kAllowListKey);
  if (!allow_list) {
    return not_valid("has no \"allow_list\"");
  }
  if (!allow_list->is_none()) {
    const base::ListValue* const hosts = allow_list->GetIfList();
    if (!hosts || hosts->size() > kMaxAllowListSize) {
      return not_valid("has an \"allow_list\" that is not a list of hosts");
    }
    unload.allow_list.emplace();
    for (const base::Value& host : *hosts) {
      const std::string* const text = host.GetIfString();
      if (!text || text->empty() || text->size() > kMaxHostSize ||
          !base::IsStringASCII(*text)) {
        return not_valid("has an \"allow_list\" that is not a list of hosts");
      }
      unload.allow_list->push_back(*text);
    }
  }
  const base::Value* const by_bucket = dict->Find(kByBucketKey);
  if (!by_bucket) {
    return not_valid("has no \"by_bucket\"");
  }
  if (!by_bucket->is_none()) {
    const base::DictValue* const rollout = by_bucket->GetIfDict();
    const std::optional<int> percent =
        rollout ? rollout->FindInt(kPercentKey) : std::nullopt;
    const base::Value* const bucket =
        rollout ? rollout->Find(kBucketKey) : nullptr;
    const bool valid = rollout && rollout->size() == 2u && percent &&
                       *percent >= 0 && *percent <= 100 && bucket &&
                       (bucket->is_none() || bucket->is_int());
    if (!valid) {
      return not_valid(
          "has a \"by_bucket\" that is not a percent and a bucket or null");
    }
    unload.by_bucket = DeprecatedUnload::ByBucket{
        .percent = *percent,
        .bucket = bucket->is_int() ? std::optional<int>(bucket->GetInt())
                                   : std::nullopt};
  }
  return unload;
}

base::DictValue WriteDeprecatedUnload(const DeprecatedUnload& unload) {
  base::DictValue dict;
  dict.Set(kEnabledKey, unload.enabled);
  if (unload.allow_list) {
    base::ListValue hosts;
    for (const std::string& host : *unload.allow_list) {
      hosts.Append(host);
    }
    dict.Set(kAllowListKey, std::move(hosts));
  } else {
    dict.Set(kAllowListKey, base::Value());
  }
  if (unload.by_bucket) {
    base::DictValue rollout;
    rollout.Set(kPercentKey, unload.by_bucket->percent);
    if (unload.by_bucket->bucket) {
      rollout.Set(kBucketKey, *unload.by_bucket->bucket);
    } else {
      rollout.Set(kBucketKey, base::Value());
    }
    dict.Set(kByBucketKey, std::move(rollout));
  } else {
    dict.Set(kByBucketKey, base::Value());
  }
  return dict;
}

base::expected<PageFeatures, std::string> ReadFeatures(
    const base::DictValue& dict) {
  for (const auto [key, value] : dict) {
    if (key != kDeprecatedUnloadKey &&
        std::ranges::none_of(kFeatureLists, [key](const FeatureList& list) {
          return list.key == key;
        })) {
      return base::unexpected(base::StrCat(
          {"a start whose features have the unknown key \"", key, "\""}));
    }
  }
  PageFeatures features;
  for (const FeatureList& list : kFeatureLists) {
    const base::ListValue* const items = dict.FindList(list.key);
    if (!items || items->size() > list.most) {
      return base::unexpected(base::StrCat(
          {"a start whose features' \"", list.key,
           "\" is not a list of at most ", base::NumberToString(list.most)}));
    }
    for (const base::Value& item : *items) {
      const std::string* const text = item.GetIfString();
      const bool valid =
          text &&
          (list.key == kV8FlagsKey ? IsV8Flag(*text) : IsFeatureName(*text));
      if (!valid) {
        return base::unexpected(base::StrCat(
            {"a start whose features' \"", list.key, "\" holds ",
             list.key == kV8FlagsKey ? "what is not a V8 flag"
                                     : "what is not a feature's name"}));
      }
      (features.*list.field).push_back(*text);
    }
  }
  base::expected<DeprecatedUnload, std::string> unload =
      ReadDeprecatedUnload(dict.Find(kDeprecatedUnloadKey));
  if (!unload.has_value()) {
    return base::unexpected(std::move(unload).error());
  }
  features.deprecated_unload = *std::move(unload);
  return features;
}

bool IsStartKey(std::string_view key) {
  return key == kWindowStateKey || key == kInvertedColorsKey ||
         key == kMemoryKey || key == kScaleKey || key == kNavigatorKey ||
         key == kFeaturesKey || key == kMediaDevicesKey ||
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
  const uint8_t flags =
      bytes.size() < 10 ? 0xff : static_cast<uint8_t>(bytes[9]);
  if (flags & ~(kSameOriginFlag | kUserActivationFlag | kReloadFlag)) {
    return base::unexpected("a request with flags that are not ones");
  }
  request.same_origin = flags & kSameOriginFlag;
  request.user_activation = flags & kUserActivationFlag;
  request.reload = flags & kReloadFlag;
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
  if (request.kind != PageRequestKind::kNavigation &&
      (request.user_activation || request.reload)) {
    return base::unexpected(
        "a request that is not a navigation with a navigation's flags");
  }
  if (request.kind == PageRequestKind::kNavigation &&
      (request.method != "GET" || !request.headers.empty() || request.body ||
       request.same_origin)) {
    return base::unexpected(
        "a navigation with a method, header lines, a body or a mode");
  }
  return request;
}

bool IsSameSite(uint8_t same_site) {
  return same_site <= static_cast<uint8_t>(PageCookieSameSite::kNoRestriction);
}

std::string EncodeSized(std::string_view part) {
  return base::StrCat({EncodeUint32(static_cast<uint32_t>(part.size())), part});
}

// A cookie: a byte of flags, 1 if it is secure, 2 if it is partitioned and 4
// if it expires, a byte of its same-site mode, when it expires, 0 if it does
// not, then its name, value, domain and path, each after its size.
std::string EncodeCookie(const PageCookie& cookie) {
  const char flags =
      static_cast<char>((cookie.secure ? 1 : 0) | (cookie.partitioned ? 2 : 0) |
                        (cookie.expires ? 4 : 0));
  const char same_site = static_cast<char>(cookie.same_site);
  return base::StrCat({std::string_view(&flags, 1),
                       std::string_view(&same_site, 1),
                       EncodeInt64(cookie.expires.value_or(0)),
                       EncodeSized(cookie.name), EncodeSized(cookie.value),
                       EncodeSized(cookie.domain), EncodeSized(cookie.path)});
}

// Takes a cookie from the front of `bytes`. A cookie of the Session's store
// may hold any bytes, which the Page reads as UTF-8 as Blink does.
std::optional<PageCookie> TakeCookie(std::string_view& bytes) {
  if (bytes.size() < 10) {
    return std::nullopt;
  }
  const uint8_t flags = static_cast<uint8_t>(bytes[0]);
  const uint8_t same_site = static_cast<uint8_t>(bytes[1]);
  if (flags > 7 || !IsSameSite(same_site)) {
    return std::nullopt;
  }
  PageCookie cookie;
  cookie.secure = flags & 1;
  cookie.partitioned = flags & 2;
  const int64_t expires = DecodeInt64(bytes.substr(2, 8));
  if (flags & 4) {
    cookie.expires = expires;
  } else if (expires != 0) {
    return std::nullopt;
  }
  cookie.same_site = static_cast<PageCookieSameSite>(same_site);
  bytes.remove_prefix(10);
  std::string_view name;
  std::string_view value;
  std::string_view domain;
  std::string_view path;
  if (!TakeSized(bytes, name) || !TakeSized(bytes, value) ||
      !TakeSized(bytes, domain) || !TakeSized(bytes, path)) {
    return std::nullopt;
  }
  cookie.name = std::string(name);
  cookie.value = std::string(value);
  cookie.domain = std::string(domain);
  cookie.path = std::string(path);
  return cookie;
}

// A question of a CookieStore: its number, a byte, 1 for all cookies and 0
// for those of a name, and the name, empty for all.
base::expected<PageCookieStoreGet, std::string> ReadCookieStoreGet(
    std::string_view payload) {
  if (payload.size() < 9) {
    return base::unexpected(WrongSize("CookieStore question", payload.size()));
  }
  PageCookieStoreGet get;
  get.question = DecodeUint64(payload.substr(0, 8));
  const uint8_t all = static_cast<uint8_t>(payload[8]);
  const std::string_view name = payload.substr(9);
  if (all > 1 || (all == 1 && !name.empty()) || !IsText(name)) {
    return base::unexpected("a question of a CookieStore that is not valid");
  }
  get.all = all == 1;
  get.name = std::string(name);
  return get;
}

// A cookie that a CookieStore sets: the number of the question, when the
// cookie expires, a byte of its same-site mode and one that is 1 if it is
// partitioned, then its name, value, domain, each after its size, and path.
base::expected<PageCookieStoreSet, std::string> ReadCookieStoreSet(
    std::string_view payload) {
  constexpr std::string_view kNotValid =
      "a cookie of a CookieStore that is not valid";
  if (payload.size() < 18) {
    return base::unexpected(WrongSize("CookieStore cookie", payload.size()));
  }
  PageCookieStoreSet set;
  set.question = DecodeUint64(payload.substr(0, 8));
  set.expiry = DecodeInt64(payload.substr(8, 8));
  const uint8_t same_site = static_cast<uint8_t>(payload[16]);
  const uint8_t partitioned = static_cast<uint8_t>(payload[17]);
  if (same_site == static_cast<uint8_t>(PageCookieSameSite::kNone) ||
      !IsSameSite(same_site) || partitioned > 1) {
    return base::unexpected(std::string(kNotValid));
  }
  set.same_site = static_cast<PageCookieSameSite>(same_site);
  set.partitioned = partitioned == 1;
  std::string_view bytes = payload.substr(18);
  std::string_view name;
  std::string_view value;
  std::string_view domain;
  if (!TakeSized(bytes, name) || !TakeSized(bytes, value) ||
      !TakeSized(bytes, domain) || !IsText(name) || !IsText(value) ||
      !IsText(domain) || !IsText(bytes)) {
    return base::unexpected(std::string(kNotValid));
  }
  set.name = std::string(name);
  set.value = std::string(value);
  set.domain = std::string(domain);
  set.path = std::string(bytes);
  return set;
}

base::expected<PageCookieStoreCookies, std::string> ReadCookieStoreCookies(
    std::string_view payload) {
  if (payload.size() < 12) {
    return base::unexpected(
        WrongSize("list of CookieStore cookies", payload.size()));
  }
  PageCookieStoreCookies cookies;
  cookies.question = DecodeUint64(payload.substr(0, 8));
  const uint32_t count = DecodeUint32(payload.substr(8, 4));
  std::string_view bytes = payload.substr(12);
  // Each cookie takes at least 26 bytes, which bounds what to reserve.
  if (count > bytes.size() / 26) {
    return base::unexpected("cookies of a CookieStore that are not valid");
  }
  cookies.cookies.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::optional<PageCookie> cookie = TakeCookie(bytes);
    if (!cookie) {
      return base::unexpected("cookies of a CookieStore that are not valid");
    }
    cookies.cookies.push_back(*std::move(cookie));
  }
  if (!bytes.empty()) {
    return base::unexpected("cookies of a CookieStore that are not valid");
  }
  return cookies;
}

base::expected<PageCookieStoreChange, std::string> ReadCookieStoreChange(
    std::string_view payload) {
  if (payload.empty() || static_cast<uint8_t>(payload[0]) > 1) {
    return base::unexpected("a change of cookies that is not valid");
  }
  PageCookieStoreChange change;
  change.deleted = payload[0] == 1;
  std::string_view bytes = payload.substr(1);
  std::optional<PageCookie> cookie = TakeCookie(bytes);
  if (!cookie || !bytes.empty()) {
    return base::unexpected("a change of cookies that is not valid");
  }
  change.cookie = *std::move(cookie);
  return change;
}

// A message of a BroadcastChannel: its name after its size, a byte that is 1
// if it is locked to its agent cluster, and its bytes, which are the rest.
base::expected<PageBroadcast, std::string> ReadBroadcast(
    std::string_view payload) {
  std::string_view name;
  if (!TakeSized(payload, name) || payload.empty()) {
    return base::unexpected(WrongSize("broadcast", payload.size()));
  }
  const uint8_t locked = static_cast<uint8_t>(payload[0]);
  if (locked > 1 || !IsText(name)) {
    return base::unexpected("a broadcast that is not valid");
  }
  PageBroadcast broadcast;
  broadcast.name = std::string(name);
  broadcast.locked = locked == 1;
  broadcast.data = std::string(payload.substr(1));
  return broadcast;
}

// A document that the Page has made: its ID, a byte for its source, and for a
// navigation's response the navigation's number.
base::expected<PageDocumentCommitted, std::string> ReadDocumentCommitted(
    std::string_view payload) {
  if (payload.size() != 9 && payload.size() != 17) {
    return base::unexpected(WrongSize("document", payload.size()));
  }
  PageDocumentCommitted committed;
  committed.document = DecodeUint64(payload.substr(0, 8));
  const uint8_t source = static_cast<uint8_t>(payload[8]);
  const bool from_response =
      source == static_cast<uint8_t>(PageDocumentSource::kNavigationResponse);
  if ((source != static_cast<uint8_t>(PageDocumentSource::kInitialEmpty) &&
       !from_response) ||
      (payload.size() == 17) != from_response) {
    return base::unexpected("a document that is not valid");
  }
  if (committed.document == kNoDocument) {
    return base::unexpected("a document numbered 0");
  }
  committed.source = static_cast<PageDocumentSource>(source);
  if (from_response) {
    committed.navigation = DecodeUint64(payload.substr(9, 8));
  }
  return committed;
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
  if (bytes.size() < kResponseStartedFixedSize) {
    return base::unexpected(WrongSize("response", bytes.size()));
  }
  PageResponseStarted response;
  response.number = DecodeUint64(bytes.substr(0, 8));
  response.status = DecodeInt32(bytes.substr(8, 4));
  const uint8_t redirected = static_cast<uint8_t>(bytes[12]);
  const uint8_t cors = static_cast<uint8_t>(bytes[13]);
  const uint8_t spdy = static_cast<uint8_t>(bytes[14]);
  const uint8_t alpn = static_cast<uint8_t>(bytes[15]);
  response.navigation_start =
      static_cast<int64_t>(DecodeUint64(bytes.substr(16, 8)));
  response.connection.rtt = DecodeUint32(bytes.substr(24, 4));
  response.connection.downlink =
      base::bit_cast<double>(DecodeUint64(bytes.substr(28, 8)));
  size_t offset = 36;
  bool times_valid = true;
  for (int64_t PageLoadTiming::* time : LoadTimingTimes()) {
    response.timing.*time = DecodeInt64(bytes.substr(offset, 8));
    times_valid = times_valid && response.timing.*time >= 0;
    offset += 8;
  }
  const uint8_t socket_reused = static_cast<uint8_t>(bytes[116]);
  const uint8_t timing_allow_passed = static_cast<uint8_t>(bytes[117]);
  const uint8_t validated = static_cast<uint8_t>(bytes[118]);
  const uint8_t cross_origin_redirect = static_cast<uint8_t>(bytes[119]);
  const uint8_t restarted = static_cast<uint8_t>(bytes[120]);
  const uint8_t opaque = static_cast<uint8_t>(bytes[121]);
  response.received_bytes = DecodeInt64(bytes.substr(122, 8));
  response.redirect_count = DecodeUint32(bytes.substr(130, 4));
  response.first_request_start = DecodeInt64(bytes.substr(134, 8));
  response.last_redirect_end = DecodeInt64(bytes.substr(142, 8));
  response.critical_ch_restart = DecodeInt64(bytes.substr(150, 8));
  bytes.remove_prefix(kResponseStartedFixedSize);
  if (!times_valid || socket_reused > 1 || timing_allow_passed > 1 ||
      validated > 1 || cross_origin_redirect > 1 || restarted > 1 ||
      opaque > 1 || (opaque == 1 && cors == 1) || response.received_bytes < 0 ||
      response.redirect_count > kMostRedirects ||
      response.first_request_start < 0 || response.last_redirect_end < 0 ||
      response.critical_ch_restart < 0) {
    return base::unexpected("a response whose timing //net cannot give");
  }
  response.timing.socket_reused = socket_reused == 1;
  response.timing_allow_passed = timing_allow_passed == 1;
  response.validated = validated == 1;
  response.cross_origin_redirect = cross_origin_redirect == 1;
  response.restarted_for_critical_ch = restarted == 1;
  response.opaque = opaque == 1;
  std::string_view url;
  std::string_view status_text;
  std::string_view connection_info;
  std::string_view alpn_negotiated_protocol;
  std::string_view referrer;
  if (response.status < 0 || redirected > 1 || cors > 1 || spdy > 1 ||
      alpn > 1 || !IsConnection(response.connection) ||
      !TakeSized(bytes, url) || !TakeSized(bytes, status_text) ||
      !TakeSized(bytes, connection_info) ||
      !TakeSized(bytes, alpn_negotiated_protocol) ||
      !TakeSized(bytes, referrer)) {
    return base::unexpected("a response that does not hold what it says");
  }
  if (!IsText(url) || !IsText(referrer)) {
    return base::unexpected("a response whose URL is not UTF-8");
  }
  if (!IsProtocolName(connection_info) ||
      !IsProtocolName(alpn_negotiated_protocol)) {
    return base::unexpected("a response whose protocol is not a name");
  }
  response.redirected = redirected == 1;
  response.cors = cors == 1;
  response.was_fetched_via_spdy = spdy == 1;
  response.was_alpn_negotiated = alpn == 1;
  response.url = std::string(url);
  response.status_text = std::string(status_text);
  response.connection_info = std::string(connection_info);
  response.alpn_negotiated_protocol = std::string(alpn_negotiated_protocol);
  response.referrer = std::string(referrer);
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
  if (payload.size() != 36) {
    return base::unexpected(
        base::StrCat({"an end of a response of ",
                      base::NumberToString(payload.size()), " bytes"}));
  }
  PageResponseEnded ended;
  ended.number = DecodeUint64(payload.substr(0, 8));
  ended.net_error = DecodeInt32(payload.substr(8, 4));
  ended.encoded_body_size = DecodeInt64(payload.substr(12, 8));
  ended.received_bytes = DecodeInt64(payload.substr(20, 8));
  ended.completion_time = DecodeInt64(payload.substr(28, 8));
  if (ended.net_error > 0) {
    return base::unexpected(
        base::StrCat({"the end of a response with the net error ",
                      base::NumberToString(ended.net_error)}));
  }
  if (ended.encoded_body_size < 0 || ended.received_bytes < 0 ||
      ended.completion_time < 0) {
    return base::unexpected("the end of a response that //net cannot give");
  }
  return ended;
}

}  // namespace

PageRequest::PageRequest() = default;
PageRequest::PageRequest(PageRequest&&) = default;
PageRequest& PageRequest::operator=(PageRequest&&) = default;
PageRequest::~PageRequest() = default;

PageResponseStarted::PageResponseStarted() = default;
PageResponseStarted::PageResponseStarted(const PageResponseStarted&) = default;
PageResponseStarted& PageResponseStarted::operator=(
    const PageResponseStarted&) = default;
PageResponseStarted::PageResponseStarted(PageResponseStarted&&) = default;
PageResponseStarted& PageResponseStarted::operator=(PageResponseStarted&&) =
    default;
PageResponseStarted::~PageResponseStarted() = default;

PageCookie::PageCookie() = default;
PageCookie::PageCookie(const PageCookie&) = default;
PageCookie& PageCookie::operator=(const PageCookie&) = default;
PageCookie::PageCookie(PageCookie&&) = default;
PageCookie& PageCookie::operator=(PageCookie&&) = default;
PageCookie::~PageCookie() = default;

PageCookieStoreSet::PageCookieStoreSet() = default;
PageCookieStoreSet::PageCookieStoreSet(PageCookieStoreSet&&) = default;
PageCookieStoreSet& PageCookieStoreSet::operator=(PageCookieStoreSet&&) =
    default;
PageCookieStoreSet::~PageCookieStoreSet() = default;

PageCookieStoreCookies::PageCookieStoreCookies() = default;
PageCookieStoreCookies::PageCookieStoreCookies(PageCookieStoreCookies&&) =
    default;
PageCookieStoreCookies& PageCookieStoreCookies::operator=(
    PageCookieStoreCookies&&) = default;
PageCookieStoreCookies::~PageCookieStoreCookies() = default;

PageBroadcast::PageBroadcast() = default;
PageBroadcast::PageBroadcast(const PageBroadcast&) = default;
PageBroadcast& PageBroadcast::operator=(const PageBroadcast&) = default;
PageBroadcast::PageBroadcast(PageBroadcast&&) = default;
PageBroadcast& PageBroadcast::operator=(PageBroadcast&&) = default;
PageBroadcast::~PageBroadcast() = default;

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
DeprecatedUnload::DeprecatedUnload() = default;
DeprecatedUnload::DeprecatedUnload(const DeprecatedUnload&) = default;
DeprecatedUnload& DeprecatedUnload::operator=(const DeprecatedUnload&) =
    default;
DeprecatedUnload::~DeprecatedUnload() = default;

PageFeatures::PageFeatures() = default;
PageFeatures::PageFeatures(const PageFeatures&) = default;
PageFeatures& PageFeatures::operator=(const PageFeatures&) = default;
PageFeatures::~PageFeatures() = default;

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
  base::DictValue features;
  for (const FeatureList& list : kFeatureLists) {
    base::ListValue items;
    for (const std::string& item : start.features.*list.field) {
      items.Append(item);
    }
    features.Set(list.key, std::move(items));
  }
  features.Set(kDeprecatedUnloadKey,
               WriteDeprecatedUnload(start.features.deprecated_unload));
  dict.Set(kFeaturesKey, std::move(features));
  dict.Set(kMediaDevicesKey,
           base::DictValue()
               .Set(kAudioInputKey, start.media_devices.audio_input)
               .Set(kVideoInputKey, start.media_devices.video_input)
               .Set(kAudioOutputKey, start.media_devices.audio_output));
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
  const base::DictValue* const features = dict.FindDict(kFeaturesKey);
  if (!features) {
    return base::unexpected(NotA(kFeaturesKey, "an object"));
  }
  base::expected<PageFeatures, std::string> read = ReadFeatures(*features);
  if (!read.has_value()) {
    return base::unexpected(std::move(read).error());
  }
  start.features = *std::move(read);
  const base::DictValue* const devices = dict.FindDict(kMediaDevicesKey);
  const std::optional<bool> audio_input =
      devices ? devices->FindBool(kAudioInputKey) : std::nullopt;
  const std::optional<bool> video_input =
      devices ? devices->FindBool(kVideoInputKey) : std::nullopt;
  const std::optional<bool> audio_output =
      devices ? devices->FindBool(kAudioOutputKey) : std::nullopt;
  if (!devices || devices->size() != 3u || !audio_input || !video_input ||
      !audio_output) {
    return base::unexpected(NotA(
        kMediaDevicesKey,
        "whether there is an audio input, a video input and an audio output"));
  }
  start.media_devices = {.audio_input = *audio_input,
                         .video_input = *video_input,
                         .audio_output = *audio_output};
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

std::string EncodeKnownGap(std::string_view name) {
  return EncodeFrame(PageMessage::kKnownGap, name);
}

std::string EncodeRequest(uint64_t document, const PageRequest& request) {
  const char kind = static_cast<char>(request.kind);
  const char flags =
      static_cast<char>((request.same_origin ? kSameOriginFlag : 0) |
                        (request.user_activation ? kUserActivationFlag : 0) |
                        (request.reload ? kReloadFlag : 0));
  const char has_body = request.body ? 1 : 0;
  return EncodeDocumentFrame(
      PageMessage::kRequest, document,
      base::StrCat({EncodeUint64(request.number), std::string_view(&kind, 1),
                    std::string_view(&flags, 1),
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

std::string EncodeGetCookies(uint64_t document, uint64_t question) {
  return EncodeDocumentFrame(PageMessage::kGetCookies, document,
                             EncodeUint64(question));
}

std::string EncodeSetCookie(uint64_t document, std::string_view line) {
  return EncodeDocumentFrame(PageMessage::kSetCookie, document, line);
}

std::string EncodeLoaded(uint64_t document) {
  return EncodeDocumentFrame(PageMessage::kLoaded, document, "");
}

std::string EncodeResponseStarted(const PageResponseStarted& response) {
  const char redirected = response.redirected ? 1 : 0;
  const char cors = response.cors ? 1 : 0;
  const char spdy = response.was_fetched_via_spdy ? 1 : 0;
  const char alpn = response.was_alpn_negotiated ? 1 : 0;
  std::string timing;
  for (int64_t PageLoadTiming::* time : LoadTimingTimes()) {
    timing += EncodeInt64(response.timing.*time);
  }
  const std::array<char, 6> flags = {
      response.timing.socket_reused ? '\1' : '\0',
      response.timing_allow_passed ? '\1' : '\0',
      response.validated ? '\1' : '\0',
      response.cross_origin_redirect ? '\1' : '\0',
      response.restarted_for_critical_ch ? '\1' : '\0',
      response.opaque ? '\1' : '\0'};
  return EncodeFrame(
      PageMessage::kResponseStarted,
      base::StrCat(
          {EncodeUint64(response.number),
           EncodeInt32(response.status),
           std::string_view(&redirected, 1),
           std::string_view(&cors, 1),
           std::string_view(&spdy, 1),
           std::string_view(&alpn, 1),
           EncodeUint64(static_cast<uint64_t>(response.navigation_start)),
           EncodeUint32(response.connection.rtt),
           EncodeUint64(base::bit_cast<uint64_t>(response.connection.downlink)),
           timing,
           std::string_view(flags.data(), flags.size()),
           EncodeInt64(response.received_bytes),
           EncodeUint32(response.redirect_count),
           EncodeInt64(response.first_request_start),
           EncodeInt64(response.last_redirect_end),
           EncodeInt64(response.critical_ch_restart),
           EncodeUint32(static_cast<uint32_t>(response.url.size())),
           response.url,
           EncodeUint32(static_cast<uint32_t>(response.status_text.size())),
           response.status_text,
           EncodeUint32(static_cast<uint32_t>(response.connection_info.size())),
           response.connection_info,
           EncodeUint32(
               static_cast<uint32_t>(response.alpn_negotiated_protocol.size())),
           response.alpn_negotiated_protocol,
           EncodeUint32(static_cast<uint32_t>(response.referrer.size())),
           response.referrer,
           response.headers}));
}

std::string EncodeResponseBody(uint64_t number, std::string_view bytes) {
  CHECK_LE(bytes.size(), kBodyWindow);
  return EncodeFrame(PageMessage::kResponseBody,
                     base::StrCat({EncodeUint64(number), bytes}));
}

std::string EncodeResponseEnded(const PageResponseEnded& ended) {
  return EncodeFrame(
      PageMessage::kResponseEnded,
      base::StrCat({EncodeUint64(ended.number), EncodeInt32(ended.net_error),
                    EncodeInt64(ended.encoded_body_size),
                    EncodeInt64(ended.received_bytes),
                    EncodeInt64(ended.completion_time)}));
}

std::string EncodeCookies(uint64_t answer, std::string_view line) {
  return EncodeFrame(PageMessage::kCookies,
                     base::StrCat({EncodeUint64(answer), line}));
}

std::string EncodeCookieStoreGet(uint64_t document,
                                 const PageCookieStoreGet& get) {
  const char all = get.all ? 1 : 0;
  return EncodeDocumentFrame(
      PageMessage::kCookieStoreGet, document,
      base::StrCat(
          {EncodeUint64(get.question), std::string_view(&all, 1), get.name}));
}

std::string EncodeCookieStoreSet(uint64_t document,
                                 const PageCookieStoreSet& set) {
  const char same_site = static_cast<char>(set.same_site);
  const char partitioned = set.partitioned ? 1 : 0;
  return EncodeDocumentFrame(
      PageMessage::kCookieStoreSet, document,
      base::StrCat({EncodeUint64(set.question), EncodeInt64(set.expiry),
                    std::string_view(&same_site, 1),
                    std::string_view(&partitioned, 1), EncodeSized(set.name),
                    EncodeSized(set.value), EncodeSized(set.domain),
                    set.path}));
}

std::string EncodeCookieStoreListen(uint64_t document, bool listen) {
  const char byte = listen ? 1 : 0;
  return EncodeDocumentFrame(PageMessage::kCookieStoreListen, document,
                             std::string_view(&byte, 1));
}

std::string EncodeCookieStoreCookies(const PageCookieStoreCookies& cookies) {
  std::string payload = base::StrCat(
      {EncodeUint64(cookies.question),
       EncodeUint32(static_cast<uint32_t>(cookies.cookies.size()))});
  for (const PageCookie& cookie : cookies.cookies) {
    payload += EncodeCookie(cookie);
  }
  return EncodeFrame(PageMessage::kCookieStoreCookies, payload);
}

std::string EncodeCookieStoreStored(uint64_t answer, bool stored) {
  const char byte = stored ? 1 : 0;
  return EncodeFrame(
      PageMessage::kCookieStoreStored,
      base::StrCat({EncodeUint64(answer), std::string_view(&byte, 1)}));
}

std::string EncodeCookieStoreChange(const PageCookieStoreChange& change) {
  const char deleted = change.deleted ? 1 : 0;
  return EncodeFrame(PageMessage::kCookieStoreChange,
                     base::StrCat({std::string_view(&deleted, 1),
                                   EncodeCookie(change.cookie)}));
}

bool BroadcastFits(const PageBroadcast& broadcast) {
  return broadcast.name.size() <= kMaxPayloadSize - 13 &&
         broadcast.data.size() <= kMaxPayloadSize - 13 - broadcast.name.size();
}

namespace {

std::string EncodeBroadcastPayload(const PageBroadcast& broadcast) {
  const char locked = broadcast.locked ? 1 : 0;
  return base::StrCat({EncodeSized(broadcast.name),
                       std::string_view(&locked, 1), broadcast.data});
}

}  // namespace

std::string EncodeBroadcast(uint64_t document, const PageBroadcast& broadcast) {
  return EncodeDocumentFrame(PageMessage::kBroadcast, document,
                             EncodeBroadcastPayload(broadcast));
}

std::string EncodeCacheStorage(uint64_t document,
                               const PageCacheStorageCall& call) {
  return EncodeDocumentFrame(PageMessage::kCacheStorage, document,
                             EncodeCacheStorageCall(call));
}

std::string EncodeIndexedDb(uint64_t document, const PageIndexedDbCall& call) {
  return EncodeDocumentFrame(PageMessage::kIndexedDb, document,
                             EncodeIndexedDbCall(call));
}

std::string EncodeBroadcastReceived(const PageBroadcast& broadcast) {
  return EncodeFrame(PageMessage::kBroadcastReceived,
                     EncodeBroadcastPayload(broadcast));
}

std::string EncodeDocumentCommitted(const PageDocumentCommitted& committed) {
  const char source = static_cast<char>(committed.source);
  std::string payload = base::StrCat(
      {EncodeUint64(committed.document), std::string_view(&source, 1)});
  if (committed.source == PageDocumentSource::kNavigationResponse) {
    payload += EncodeUint64(committed.navigation);
  }
  return EncodeFrame(PageMessage::kDocumentCommitted, payload);
}

std::string EncodeSameDocumentNavigation(uint64_t document,
                                         std::string_view url) {
  return EncodeDocumentFrame(PageMessage::kSameDocumentNavigation, document,
                             url);
}

bool NamesDocument(PageMessage type) {
  switch (type) {
    case PageMessage::kSameDocumentNavigation:
    case PageMessage::kRequest:
    case PageMessage::kGetCookies:
    case PageMessage::kSetCookie:
    case PageMessage::kLoaded:
    case PageMessage::kCookieStoreGet:
    case PageMessage::kCookieStoreSet:
    case PageMessage::kCookieStoreListen:
    case PageMessage::kBroadcast:
    case PageMessage::kCacheStorage:
    case PageMessage::kIndexedDb:
      return true;
    default:
      return false;
  }
}

base::expected<FrameFromPage, std::string> ReadFrameFromPage(
    uint32_t type,
    std::string payload) {
  if (!IsKnown(type)) {
    return base::unexpected(UnknownType(type));
  }
  FrameFromPage frame;
  frame.type = static_cast<PageMessage>(type);
  if (NamesDocument(frame.type)) {
    if (payload.size() < 8) {
      return base::unexpected(base::StrCat(
          {"message type ", base::NumberToString(type), " names no document"}));
    }
    frame.document = DecodeUint64(payload);
    if (frame.document == kNoDocument) {
      return base::unexpected(base::StrCat(
          {"message type ", base::NumberToString(type), " names document 0"}));
    }
    payload.erase(0, 8);
  }
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
    case PageMessage::kKnownGap:
      if (!IsText(payload)) {
        return base::unexpected("a Known Gap that is not UTF-8");
      }
      frame.known_gap = std::move(payload);
      return frame;
    case PageMessage::kCookieStoreGet: {
      base::expected<PageCookieStoreGet, std::string> get =
          ReadCookieStoreGet(payload);
      if (!get.has_value()) {
        return base::unexpected(std::move(get).error());
      }
      frame.cookie_store_get = *std::move(get);
      return frame;
    }
    case PageMessage::kCookieStoreSet: {
      base::expected<PageCookieStoreSet, std::string> set =
          ReadCookieStoreSet(payload);
      if (!set.has_value()) {
        return base::unexpected(std::move(set).error());
      }
      frame.cookie_store_set = *std::move(set);
      return frame;
    }
    case PageMessage::kCookieStoreListen:
      if (payload.size() != 1 || static_cast<uint8_t>(payload[0]) > 1) {
        return base::unexpected("a CookieStore's listening that is not valid");
      }
      frame.cookie_store_listen = payload[0] == 1;
      return frame;
    case PageMessage::kBroadcast: {
      base::expected<PageBroadcast, std::string> broadcast =
          ReadBroadcast(payload);
      if (!broadcast.has_value()) {
        return base::unexpected(std::move(broadcast).error());
      }
      frame.broadcast = *std::move(broadcast);
      return frame;
    }
    case PageMessage::kCacheStorage: {
      base::expected<PageCacheStorageCall, std::string> call =
          ReadCacheStorageCall(payload);
      if (!call.has_value()) {
        return base::unexpected(std::move(call).error());
      }
      frame.cache_storage_call = *std::move(call);
      return frame;
    }
    case PageMessage::kIndexedDb: {
      base::expected<PageIndexedDbCall, std::string> call =
          ReadIndexedDbCall(payload);
      if (!call.has_value()) {
        return base::unexpected(std::move(call).error());
      }
      frame.indexed_db_call = *std::move(call);
      return frame;
    }
    case PageMessage::kDocumentCommitted: {
      base::expected<PageDocumentCommitted, std::string> committed =
          ReadDocumentCommitted(payload);
      if (!committed.has_value()) {
        return base::unexpected(std::move(committed).error());
      }
      frame.committed = *committed;
      return frame;
    }
    case PageMessage::kSameDocumentNavigation:
      if (!IsText(payload)) {
        return base::unexpected("a same-document URL that is not UTF-8");
      }
      frame.same_document_url = std::move(payload);
      return frame;
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
    case PageMessage::kResponseStarted:
    case PageMessage::kResponseBody:
    case PageMessage::kResponseEnded:
    case PageMessage::kCookies:
    case PageMessage::kCookieStoreCookies:
    case PageMessage::kCookieStoreStored:
    case PageMessage::kCookieStoreChange:
    case PageMessage::kBroadcastReceived:
    case PageMessage::kCacheStorageAnswer:
    case PageMessage::kIndexedDbCallback:
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
    case PageMessage::kCookieStoreCookies: {
      base::expected<PageCookieStoreCookies, std::string> cookies =
          ReadCookieStoreCookies(payload);
      if (!cookies.has_value()) {
        return base::unexpected(std::move(cookies).error());
      }
      frame.cookie_store_cookies = *std::move(cookies);
      return frame;
    }
    case PageMessage::kCookieStoreStored:
      if (payload.size() != 9 || static_cast<uint8_t>(payload[8]) > 1) {
        return base::unexpected(
            WrongSize("CookieStore answer", payload.size()));
      }
      frame.cookie_store_stored_answer =
          DecodeUint64(std::string_view(payload).substr(0, 8));
      frame.cookie_store_stored = payload[8] == 1;
      return frame;
    case PageMessage::kCookieStoreChange: {
      base::expected<PageCookieStoreChange, std::string> change =
          ReadCookieStoreChange(payload);
      if (!change.has_value()) {
        return base::unexpected(std::move(change).error());
      }
      frame.cookie_store_change = *std::move(change);
      return frame;
    }
    case PageMessage::kBroadcastReceived: {
      base::expected<PageBroadcast, std::string> broadcast =
          ReadBroadcast(payload);
      if (!broadcast.has_value()) {
        return base::unexpected(std::move(broadcast).error());
      }
      frame.broadcast = *std::move(broadcast);
      return frame;
    }
    case PageMessage::kCacheStorageAnswer: {
      base::expected<PageCacheStorageAnswer, std::string> answer =
          ReadCacheStorageAnswer(payload);
      if (!answer.has_value()) {
        return base::unexpected(std::move(answer).error());
      }
      frame.cache_storage_answer = *std::move(answer);
      return frame;
    }
    case PageMessage::kIndexedDbCallback: {
      base::expected<PageIndexedDbCallback, std::string> callback =
          ReadIndexedDbCallback(payload);
      if (!callback.has_value()) {
        return base::unexpected(std::move(callback).error());
      }
      frame.indexed_db_callback = *std::move(callback);
      return frame;
    }
    case PageMessage::kEvaluated:
    case PageMessage::kConsole:
    case PageMessage::kUncaughtError:
    case PageMessage::kRequest:
    case PageMessage::kCredit:
    case PageMessage::kGetCookies:
    case PageMessage::kSetCookie:
    case PageMessage::kLoaded:
    case PageMessage::kCancel:
    case PageMessage::kKnownGap:
    case PageMessage::kCookieStoreGet:
    case PageMessage::kCookieStoreSet:
    case PageMessage::kCookieStoreListen:
    case PageMessage::kBroadcast:
    case PageMessage::kCacheStorage:
    case PageMessage::kIndexedDb:
    case PageMessage::kDocumentCommitted:
    case PageMessage::kSameDocumentNavigation:
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
