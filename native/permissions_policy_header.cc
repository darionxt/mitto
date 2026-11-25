#include "mitto/permissions_policy_header.h"

#include <stddef.h>

#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "net/http/structured_headers.h"

namespace mitto {

namespace {

constexpr size_t kMaxLength = 1 << 16;
// The most that the messages of a header hold, which repeat a feature's name
// for each of its items, so that a header never makes a message for the Page
// that is too large. What is past it is left out.
constexpr size_t kMaxMessagesLength = 1 << 20;

// A header's bytes are Latin-1, as Blink's strings of them.
std::string FromLatin1(std::string_view bytes) {
  std::u16string text;
  text.reserve(bytes.size());
  for (const char c : bytes) {
    text.push_back(static_cast<unsigned char>(c));
  }
  return base::UTF16ToUTF8(text);
}

// ParsePermissionsPolicyToIR().
PolicyNode ParsePermissionsPolicy(std::string_view policy) {
  PolicyNode node;
  size_t messages_length = 0;
  const auto warn = [&](std::initializer_list<std::string_view> pieces) {
    for (const std::string_view piece : pieces) {
      messages_length += piece.size();
    }
    if (messages_length <= kMaxMessagesLength) {
      node.messages.push_back({.text = base::StrCat(pieces)});
    }
  };
  if (policy.size() > kMaxLength) {
    node.messages.push_back(
        {true,
         base::StrCat({"Permissions policy declaration exceeds size limit(",
                       base::NumberToString(policy.size()), ">",
                       base::NumberToString(kMaxLength), ")"})});
    return node;
  }
  const std::optional<net::structured_headers::Dictionary> dictionary =
      net::structured_headers::ParseDictionary(FromLatin1(policy));
  if (!dictionary) {
    node.messages.push_back(
        {true,
         "Parse of permissions policy failed because of errors reported by "
         "structured header parser."});
    return node;
  }
  for (const auto& [feature, member] : *dictionary) {
    PolicyEntry entry{.feature = feature};
    const auto process_item = [&](const net::structured_headers::Item& item) {
      std::string token;
      if (const std::string* const value = item.GetIfToken()) {
        // The special keywords are tokens.
        if (*value != "*" && *value != "self") {
          warn({"Invalid allowlist item(", *value, ") for feature ",
                entry.feature,
                ". Allowlist item must be *, self or quoted url."});
          return;
        }
        token = *value == "*" ? *value : base::StrCat({"'", *value, "'"});
      } else if (const std::string* const text = item.GetIfString()) {
        token = *text;
      } else {
        warn({"Invalid allowlist item for feature ", entry.feature,
              ". Allowlist item must be *, self, or quoted url."});
        return;
      }
      if (!token.empty()) {
        entry.tokens.push_back(std::move(token));
      }
    };
    if (const net::structured_headers::ParameterizedItem* const single =
            member.GetIfItem()) {
      process_item(single->item);
    } else if (const net::structured_headers::InnerList* const list =
                   member.GetIfInnerList()) {
      for (const auto& one : list->items) {
        if (!one.params.empty()) {
          warn({"Feature ", entry.feature, "'s parameters are ignored."});
        }
        process_item(one.item);
      }
    }
    if (entry.tokens.empty()) {
      entry.tokens.push_back("'none'");
    }
    node.entries.push_back(std::move(entry));
  }
  return node;
}

}  // namespace

PermissionsPolicyHeaders ParsePermissionsPolicyHeaders(
    const std::optional<std::string>& feature_policy,
    const std::optional<std::string>& permissions_policy) {
  PermissionsPolicyHeaders headers;
  if (feature_policy) {
    headers.feature_policy =
        ParseFeaturePolicy(FromLatin1(*feature_policy), feature_policy->size(),
                           /*is_header=*/true);
  }
  if (permissions_policy) {
    headers.permissions_policy = ParsePermissionsPolicy(*permissions_policy);
  }
  return headers;
}

}  // namespace mitto
