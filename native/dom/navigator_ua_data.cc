#include "mitto/dom/navigator_ua_data.h"

#include <string_view>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigator_ua_data.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

v8::Local<v8::String> String(v8::Isolate* isolate, const std::string& text) {
  return bindings::ToV8StringFromUtf8(isolate, text);
}

// A NavigatorUABrandVersion, whose members a page reads in their sorted order.
v8::Local<v8::Value> BrandVersion(v8::Isolate* isolate,
                                  const PageBrandVersion& brand) {
  return PlainObject(isolate, {{"brand", String(isolate, brand.brand)},
                               {"version", String(isolate, brand.version)}});
}

std::vector<v8::Local<v8::Value>> BrandVersions(
    v8::Isolate* isolate,
    const std::vector<PageBrandVersion>& brands) {
  std::vector<v8::Local<v8::Value>> items;
  for (const PageBrandVersion& brand : brands) {
    items.push_back(BrandVersion(isolate, brand));
  }
  return items;
}

// A sequence<NavigatorUABrandVersion>, which is not frozen.
v8::Local<v8::Value> BrandVersionArray(
    v8::Isolate* isolate,
    const std::vector<PageBrandVersion>& brands) {
  std::vector<v8::Local<v8::Value>> items = BrandVersions(isolate, brands);
  return v8::Array::New(isolate, items.data(), items.size());
}

}  // namespace

// static
NavigatorUAData* NavigatorUAData::Create(const PageUserAgentData& data) {
  return bindings::MakeGarbageCollected<NavigatorUAData>(data);
}

NavigatorUAData::NavigatorUAData(const PageUserAgentData& data) : data_(data) {}

NavigatorUAData::~NavigatorUAData() = default;

const bindings::WrapperTypeInfo* NavigatorUAData::GetWrapperTypeInfo() const {
  return &bindings::V8NavigatorUAData::kWrapperTypeInfo;
}

v8::Local<v8::Value> NavigatorUAData::Brands() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return FrozenArray(isolate, BrandVersions(isolate, data_.brands));
}

bool NavigatorUAData::Mobile() const {
  return data_.mobile;
}

std::u16string NavigatorUAData::Platform() const {
  return base::UTF8ToUTF16(data_.platform);
}

v8::Local<v8::Promise> NavigatorUAData::GetHighEntropyValues(
    const std::vector<std::u16string>& hints) const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // The low-entropy values always, then each hint asked for. A top-level
  // page's permissions policy allows its own origin
  // ch-ua-high-entropy-values. A UADataValues writes its members in their
  // sorted order.
  auto asked = [&hints](std::u16string_view hint) {
    return std::ranges::find(hints, hint) != hints.end();
  };
  std::vector<std::pair<std::string_view, v8::Local<v8::Value>>> members;
  if (asked(u"architecture")) {
    members.emplace_back("architecture", String(isolate, data_.architecture));
  }
  if (asked(u"bitness")) {
    members.emplace_back("bitness", String(isolate, data_.bitness));
  }
  members.emplace_back("brands", BrandVersionArray(isolate, data_.brands));
  if (asked(u"formFactors")) {
    std::vector<v8::Local<v8::Value>> form_factors;
    for (const std::string& form_factor : data_.form_factors) {
      form_factors.push_back(String(isolate, form_factor));
    }
    members.emplace_back(
        "formFactors",
        v8::Array::New(isolate, form_factors.data(), form_factors.size()));
  }
  if (asked(u"fullVersionList")) {
    members.emplace_back("fullVersionList",
                         BrandVersionArray(isolate, data_.full_version_list));
  }
  members.emplace_back("mobile", v8::Boolean::New(isolate, data_.mobile));
  if (asked(u"model")) {
    members.emplace_back("model", String(isolate, data_.model));
  }
  members.emplace_back("platform", String(isolate, data_.platform));
  if (asked(u"platformVersion")) {
    members.emplace_back("platformVersion",
                         String(isolate, data_.platform_version));
  }
  if (asked(u"uaFullVersion")) {
    members.emplace_back("uaFullVersion",
                         String(isolate, data_.ua_full_version));
  }
  if (asked(u"wow64")) {
    members.emplace_back("wow64", v8::Boolean::New(isolate, data_.wow64));
  }
  // Blink resolves it from a task of the permission task source.
  ResolveFromTask(isolate, resolver, PlainObject(isolate, members));
  return resolver->GetPromise();
}

v8::Local<v8::Object> NavigatorUAData::ToJSON() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::vector<v8::Local<v8::Value>> brands =
      BrandVersions(isolate, data_.brands);
  return PlainObject(
      isolate,
      {{"brands", v8::Array::New(isolate, brands.data(), brands.size())},
       {"mobile", v8::Boolean::New(isolate, data_.mobile)},
       {"platform", String(isolate, data_.platform)}});
}

}  // namespace mitto
