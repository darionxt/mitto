#include "mitto/bindings/runtime_feature_set.h"

#include <stddef.h>

#include <utility>
#include <vector>

#include "base/strings/string_split.h"

namespace mitto::bindings {

namespace {

std::optional<size_t> IndexOf(std::string_view name) {
  const base::span<const RuntimeFeatureInfo> table = RuntimeFeatureTable();
  for (size_t index = 0; index < table.size(); ++index) {
    if (name == table[index].name) {
      return index;
    }
  }
  return std::nullopt;
}

}  // namespace

RuntimeFeatureSet::RuntimeFeatureSet() = default;
RuntimeFeatureSet::RuntimeFeatureSet(const RuntimeFeatureSet&) = default;
RuntimeFeatureSet& RuntimeFeatureSet::operator=(const RuntimeFeatureSet&) =
    default;
RuntimeFeatureSet::~RuntimeFeatureSet() = default;

RuntimeFeatureSet RuntimeFeatureSet::Stable() {
  RuntimeFeatureSet set;
  const base::span<const RuntimeFeatureInfo> table = RuntimeFeatureTable();
  for (size_t index = 0; index < table.size(); ++index) {
    set.states_[index] = table[index].stable;
  }
  return set;
}

// static
base::expected<RuntimeFeatureSet, std::string> RuntimeFeatureSet::FromSwitches(
    std::string_view enable,
    std::string_view disable) {
  RuntimeFeatureSet set = Stable();
  if (std::optional<std::string> unknown = set.Enable(enable)) {
    return base::unexpected(std::move(*unknown));
  }
  if (std::optional<std::string> unknown = set.Disable(disable)) {
    return base::unexpected(std::move(*unknown));
  }
  return set;
}

std::optional<std::string> RuntimeFeatureSet::Enable(std::string_view names) {
  return Set(names, true);
}

std::optional<std::string> RuntimeFeatureSet::Disable(std::string_view names) {
  return Set(names, false);
}

std::optional<std::string> RuntimeFeatureSet::Set(std::string_view names,
                                                  bool enabled) {
  std::vector<size_t> indices;
  for (const std::string_view name : base::SplitStringPiece(
           names, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::optional<size_t> index = IndexOf(name);
    if (!index) {
      return std::string(name);
    }
    indices.push_back(*index);
  }
  for (const size_t index : indices) {
    states_[index] = enabled;
  }
  return std::nullopt;
}

bool RuntimeFeatureSet::IsEnabled(RuntimeFeature feature) const {
  const RuntimeFeatureInfo& info =
      RuntimeFeatureTable()[static_cast<size_t>(feature)];
  for (const RuntimeFeature depends_on : info.depends_on) {
    if (!IsEnabled(depends_on)) {
      return false;
    }
  }
  for (const RuntimeFeature implied_by : info.implied_by) {
    if (IsEnabled(implied_by)) {
      return true;
    }
  }
  return states_[static_cast<size_t>(feature)];
}

}  // namespace mitto::bindings
