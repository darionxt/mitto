#include "mitto/css/css_selector_parser.h"

#include <stddef.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_token.h"
#include "mitto/css/css_token_stream.h"

namespace mitto {

namespace {

using Match = CssSimpleSelector::Match;
using Namespace = CssSimpleSelector::Namespace;
using Pseudo = CssPseudoType;

bool EqualsIgnoringASCIICase(std::u16string_view a, std::string_view b) {
  return base::EqualsCaseInsensitiveASCII(a, base::ASCIIToUTF16(b));
}

bool IsDelimiter(const CssToken& token, char32_t delimiter) {
  return token.type == CssTokenType::kDelimiter && token.delimiter == delimiter;
}

// Blink's ClampTo<int>() of a token's value.
int ClampToInt(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(value);
}

bool IsUnicodeWhitespace(char16_t c) {
  // WTF's IsUnicodeWhitespace(): ASCII whitespace and the characters of
  // Unicode's Zs, Zl and Zp categories that it lists.
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r' || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
         c == 0x202F || c == 0x205F || c == 0x3000;
}

// WTF's StringToIntStrict() for what follows the "n" of An+B, which starts
// with "-": digits, then any whitespace.
std::optional<int> StringToIntStrict(std::u16string_view text) {
  size_t index = 0;
  bool negative = false;
  if (index < text.size() && text[index] == '-') {
    negative = true;
    ++index;
  } else if (index < text.size() && text[index] == '+') {
    ++index;
  }
  if (index == text.size() || !base::IsAsciiDigit(text[index])) {
    return std::nullopt;
  }
  int64_t value = 0;
  while (index < text.size() && base::IsAsciiDigit(text[index])) {
    value = value * 10 + (text[index] - '0');
    if (value > static_cast<int64_t>(std::numeric_limits<int>::max()) + 1) {
      return std::nullopt;
    }
    ++index;
  }
  while (index < text.size() && IsUnicodeWhitespace(text[index])) {
    ++index;
  }
  if (index != text.size()) {
    return std::nullopt;
  }
  if (negative) {
    value = -value;
  }
  if (value > std::numeric_limits<int>::max() ||
      value < std::numeric_limits<int>::min()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

struct PseudoName {
  std::string_view name;
  Pseudo type;
};

// Blink's kPseudoTypeWithoutArgumentsMap, sorted.
constexpr PseudoName kPseudoWithoutArguments[] = {
    {"-internal-autofill-previewed", Pseudo::kAutofillPreviewed},
    {"-internal-autofill-selected", Pseudo::kAutofillSelected},
    {"-internal-dialog-in-top-layer", Pseudo::kDialogInTopLayer},
    {"-internal-has-datalist", Pseudo::kHasDatalist},
    {"-internal-has-open-menuitem", Pseudo::kHasOpenMenuitem},
    {"-internal-is-html", Pseudo::kIsHtml},
    {"-internal-list-box", Pseudo::kListBox},
    {"-internal-media-controls-overlay-cast-button",
     Pseudo::kWebKitCustomElement},
    {"-internal-menulist-popover-with-menulist-anchor",
     Pseudo::kMenulistPopoverWithMenulistAnchor},
    {"-internal-multi-select-focus", Pseudo::kMultiSelectFocus},
    {"-internal-popover-in-top-layer", Pseudo::kPopoverInTopLayer},
    {"-internal-relative-anchor", Pseudo::kRelativeAnchor},
    {"-internal-select-contains-input", Pseudo::kSelectContainsInput},
    {"-internal-select-has-slotted-button", Pseudo::kSelectHasSlottedButton},
    {"-internal-shadow-host-has-non-auto-appearance",
     Pseudo::kHostHasNonAutoAppearance},
    {"-internal-spatial-navigation-focus", Pseudo::kSpatialNavigationFocus},
    {"-internal-text-field", Pseudo::kTextField},
    {"-internal-video-persistent", Pseudo::kVideoPersistent},
    {"-internal-video-persistent-ancestor", Pseudo::kVideoPersistentAncestor},
    {"-webkit-any-link", Pseudo::kWebkitAnyLink},
    {"-webkit-autofill", Pseudo::kWebKitAutofill},
    {"-webkit-drag", Pseudo::kDrag},
    {"-webkit-full-page-media", Pseudo::kFullPageMedia},
    {"-webkit-full-screen", Pseudo::kFullScreen},
    {"-webkit-full-screen-ancestor", Pseudo::kFullScreenAncestor},
    {"-webkit-resizer", Pseudo::kResizer},
    {"-webkit-scrollbar", Pseudo::kScrollbar},
    {"-webkit-scrollbar-button", Pseudo::kScrollbarButton},
    {"-webkit-scrollbar-corner", Pseudo::kScrollbarCorner},
    {"-webkit-scrollbar-thumb", Pseudo::kScrollbarThumb},
    {"-webkit-scrollbar-track", Pseudo::kScrollbarTrack},
    {"-webkit-scrollbar-track-piece", Pseudo::kScrollbarTrackPiece},
    {"active", Pseudo::kActive},
    {"active-option", Pseudo::kActiveOption},
    {"active-view-transition", Pseudo::kActiveViewTransition},
    {"after", Pseudo::kAfter},
    {"animated-image", Pseudo::kAnimatedImage},
    {"any-link", Pseudo::kAnyLink},
    {"autofill", Pseudo::kAutofill},
    {"backdrop", Pseudo::kBackdrop},
    {"before", Pseudo::kBefore},
    {"buffering", Pseudo::kBuffering},
    {"checked", Pseudo::kChecked},
    {"checkmark", Pseudo::kCheckMark},
    {"column", Pseudo::kColumn},
    {"corner-present", Pseudo::kCornerPresent},
    {"cue", Pseudo::kWebKitCustomElement},
    {"current", Pseudo::kCurrent},
    {"decrement", Pseudo::kDecrement},
    {"default", Pseudo::kDefault},
    {"defined", Pseudo::kDefined},
    {"details-content", Pseudo::kDetailsContent},
    {"disabled", Pseudo::kDisabled},
    {"double-button", Pseudo::kDoubleButton},
    {"empty", Pseudo::kEmpty},
    {"enabled", Pseudo::kEnabled},
    {"end", Pseudo::kEnd},
    {"expand-icon", Pseudo::kExpandIcon},
    {"file-selector-button", Pseudo::kFileSelectorButton},
    {"filtered", Pseudo::kFiltered},
    {"first", Pseudo::kFirstPage},
    {"first-child", Pseudo::kFirstChild},
    {"first-letter", Pseudo::kFirstLetter},
    {"first-line", Pseudo::kFirstLine},
    {"first-of-type", Pseudo::kFirstOfType},
    {"focus", Pseudo::kFocus},
    {"focus-visible", Pseudo::kFocusVisible},
    {"focus-within", Pseudo::kFocusWithin},
    {"fullscreen", Pseudo::kFullscreen},
    {"future", Pseudo::kFutureCue},
    {"grammar-error", Pseudo::kGrammarError},
    {"granted", Pseudo::kPermissionGranted},
    {"has-slotted", Pseudo::kHasSlotted},
    {"horizontal", Pseudo::kHorizontal},
    {"host", Pseudo::kHost},
    {"hover", Pseudo::kHover},
    {"in-range", Pseudo::kInRange},
    {"increment", Pseudo::kIncrement},
    {"indeterminate", Pseudo::kIndeterminate},
    {"interest-button", Pseudo::kInterestButton},
    {"interest-source", Pseudo::kInterestSource},
    {"interest-target", Pseudo::kInterestTarget},
    {"invalid", Pseudo::kInvalid},
    {"last-child", Pseudo::kLastChild},
    {"last-of-type", Pseudo::kLastOfType},
    {"left", Pseudo::kLeftPage},
    {"link", Pseudo::kLink},
    {"marker", Pseudo::kMarker},
    {"modal", Pseudo::kModal},
    {"muted", Pseudo::kMuted},
    {"navigation-source", Pseudo::kNavigationSource},
    {"no-button", Pseudo::kNoButton},
    {"only-child", Pseudo::kOnlyChild},
    {"only-of-type", Pseudo::kOnlyOfType},
    {"open", Pseudo::kOpen},
    {"optional", Pseudo::kOptional},
    {"out-of-range", Pseudo::kOutOfRange},
    {"overscroll-backdrop", Pseudo::kOverscrollBackdrop},
    {"overscroll-closed", Pseudo::kOverscrollClosed},
    {"overscroll-open", Pseudo::kOverscrollOpen},
    {"past", Pseudo::kPastCue},
    {"paused", Pseudo::kPaused},
    {"permission-icon", Pseudo::kPermissionIcon},
    {"picker-icon", Pseudo::kPickerIcon},
    {"picture-in-picture", Pseudo::kPictureInPicture},
    {"placeholder", Pseudo::kPlaceholder},
    {"placeholder-shown", Pseudo::kPlaceholderShown},
    {"playing", Pseudo::kPlaying},
    {"popover-open", Pseudo::kPopoverOpen},
    {"read-only", Pseudo::kReadOnly},
    {"read-write", Pseudo::kReadWrite},
    {"required", Pseudo::kRequired},
    {"right", Pseudo::kRightPage},
    {"root", Pseudo::kRoot},
    {"scope", Pseudo::kScope},
    {"scroll-marker", Pseudo::kScrollMarker},
    {"scroll-marker-group", Pseudo::kScrollMarkerGroup},
    {"search-text", Pseudo::kSearchText},
    {"seeking", Pseudo::kSeeking},
    {"select-listbox", Pseudo::kSelectListbox},
    {"selection", Pseudo::kSelection},
    {"single-button", Pseudo::kSingleButton},
    {"skeleton", Pseudo::kSkeleton},
    {"spelling-error", Pseudo::kSpellingError},
    {"stalled", Pseudo::kStalled},
    {"start", Pseudo::kStart},
    {"target", Pseudo::kTarget},
    {"target-after", Pseudo::kTargetAfter},
    {"target-before", Pseudo::kTargetBefore},
    {"target-current", Pseudo::kTargetCurrent},
    {"target-text", Pseudo::kTargetText},
    {"tool-form-active", Pseudo::kToolFormActive},
    {"tool-submit-active", Pseudo::kToolSubmitActive},
    {"unbounded", Pseudo::kUnbounded},
    {"user-invalid", Pseudo::kUserInvalid},
    {"user-valid", Pseudo::kUserValid},
    {"valid", Pseudo::kValid},
    {"vertical", Pseudo::kVertical},
    {"view-transition", Pseudo::kViewTransition},
    {"visited", Pseudo::kVisited},
    {"volume-locked", Pseudo::kVolumeLocked},
    {"window-inactive", Pseudo::kWindowInactive},
    {"xr-overlay", Pseudo::kXrOverlay},
};

// Blink's kPseudoTypeWithArgumentsMap, sorted.
constexpr PseudoName kPseudoWithArguments[] = {
    {"-internal-overscroll-area-parent", Pseudo::kOverscrollAreaParent},
    {"-webkit-any", Pseudo::kAny},
    {"active-view-transition-type", Pseudo::kActiveViewTransitionType},
    {"cue", Pseudo::kCue},
    {"dir", Pseudo::kDir},
    {"has", Pseudo::kHas},
    {"highlight", Pseudo::kHighlight},
    {"host", Pseudo::kHost},
    {"host-context", Pseudo::kHostContext},
    {"is", Pseudo::kIs},
    {"lang", Pseudo::kLang},
    {"link-to", Pseudo::kLinkTo},
    {"not", Pseudo::kNot},
    {"nth-child", Pseudo::kNthChild},
    {"nth-last-child", Pseudo::kNthLastChild},
    {"nth-last-of-type", Pseudo::kNthLastOfType},
    {"nth-of-type", Pseudo::kNthOfType},
    {"part", Pseudo::kPart},
    {"picker", Pseudo::kPicker},
    {"scroll-button", Pseudo::kScrollButton},
    {"slotted", Pseudo::kSlotted},
    {"state", Pseudo::kState},
    {"view-transition-group", Pseudo::kViewTransitionGroup},
    {"view-transition-group-children", Pseudo::kViewTransitionGroupChildren},
    {"view-transition-image-pair", Pseudo::kViewTransitionImagePair},
    {"view-transition-new", Pseudo::kViewTransitionNew},
    {"view-transition-old", Pseudo::kViewTransitionOld},
    {"where", Pseudo::kWhere},
};

// Blink's CSSSelector::NameToPseudoType(), with the runtime-enabled features
// it checks. `name` is in lower case.
Pseudo NameToPseudoType(std::u16string_view name,
                        bool has_arguments,
                        const CssSelectorFeatures& features) {
  std::string latin1;
  latin1.reserve(name.size());
  for (const char16_t c : name) {
    if (c > 0xFF) {
      return Pseudo::kUnknown;
    }
    latin1.push_back(static_cast<char>(c));
  }
  const auto find = [&latin1](const auto& table) {
    const auto* match =
        std::lower_bound(std::begin(table), std::end(table), latin1,
                         [](const PseudoName& entry, const std::string& key) {
                           return entry.name < key;
                         });
    return match != std::end(table) && match->name == latin1 ? match->type
                                                             : Pseudo::kUnknown;
  };
  const Pseudo type = has_arguments ? find(kPseudoWithArguments)
                                    : find(kPseudoWithoutArguments);
  switch (type) {
    case Pseudo::kPlaying:
    case Pseudo::kPaused:
    case Pseudo::kSeeking:
    case Pseudo::kBuffering:
    case Pseudo::kStalled:
    case Pseudo::kMuted:
    case Pseudo::kVolumeLocked:
      return features.media_element_pseudos ? type : Pseudo::kUnknown;
    case Pseudo::kPermissionGranted:
      return features.permission_elements ? type : Pseudo::kUnknown;
    case Pseudo::kTargetCurrent:
    case Pseudo::kScrollMarker:
    case Pseudo::kScrollMarkerGroup:
      return features.pseudo_scroll_markers ? type : Pseudo::kUnknown;
    case Pseudo::kTargetBefore:
    case Pseudo::kTargetAfter:
      return features.scroll_marker_target_before_after ? type
                                                        : Pseudo::kUnknown;
    case Pseudo::kScrollButton:
      return features.pseudo_scroll_buttons ? type : Pseudo::kUnknown;
    case Pseudo::kColumn:
      return features.pseudo_column ? type : Pseudo::kUnknown;
    case Pseudo::kSearchText:
    case Pseudo::kCurrent:
      return features.search_text_highlight ? type : Pseudo::kUnknown;
    case Pseudo::kToolFormActive:
    case Pseudo::kToolSubmitActive:
      return features.declarative_web_mcp ? type : Pseudo::kUnknown;
    case Pseudo::kUnbounded:
      return features.unbounded_element ? type : Pseudo::kUnknown;
    case Pseudo::kHasSlotted:
      return features.pseudo_has_slotted ? type : Pseudo::kUnknown;
    case Pseudo::kOverscrollAreaParent:
    case Pseudo::kOverscrollBackdrop:
    case Pseudo::kOverscrollClosed:
    case Pseudo::kOverscrollOpen:
      return features.overscroll_areas ? type : Pseudo::kUnknown;
    case Pseudo::kActiveOption:
    case Pseudo::kFiltered:
      return features.customizable_combobox || features.filterable_select
                 ? type
                 : Pseudo::kUnknown;
    case Pseudo::kAnimatedImage:
      return features.image_animation ? type : Pseudo::kUnknown;
    case Pseudo::kExpandIcon:
      return features.menu_elements ? type : Pseudo::kUnknown;
    case Pseudo::kNavigationSource:
      return features.navigation_source ? type : Pseudo::kUnknown;
    case Pseudo::kSkeleton:
      return features.declarative_skeletons ? type : Pseudo::kUnknown;
    default:
      return type;
  }
}

// Blink's CSSSelectorParser::ParsePseudoType().
Pseudo ParsePseudoType(std::u16string_view name,
                       bool has_arguments,
                       const CssSelectorFeatures& features) {
  const Pseudo type = NameToPseudoType(name, has_arguments, features);
  if (type != Pseudo::kUnknown) {
    return type;
  }
  if (name.starts_with(u"-webkit-")) {
    return Pseudo::kWebKitCustomElement;
  }
  if (name.starts_with(u"-internal-")) {
    return Pseudo::kBlinkInternalElement;
  }
  return Pseudo::kUnknown;
}

// Blink's CSSSelector::UpdatePseudoType() for an author's selector: the
// type of `name`, and kUnknown where the kind of selector does not fit it
// or only the user agent's style sheets may use it. A single colon before
// one of the four legacy pseudo-elements makes a pseudo-element.
void UpdatePseudoType(CssSimpleSelector& selector,
                      std::u16string_view name,
                      bool has_arguments,
                      const CssSelectorFeatures& features) {
  selector.name = base::ToLowerASCII(name);
  selector.pseudo_type =
      ParsePseudoType(selector.name, has_arguments, features);
  const bool is_element = selector.match == Match::kPseudoElement;
  switch (selector.pseudo_type) {
    case Pseudo::kAfter:
    case Pseudo::kBefore:
    case Pseudo::kFirstLetter:
    case Pseudo::kFirstLine:
      selector.match = Match::kPseudoElement;
      return;
    case Pseudo::kExpandIcon:
    case Pseudo::kPickerIcon:
    case Pseudo::kInterestButton:
    case Pseudo::kCheckMark:
    case Pseudo::kBackdrop:
    case Pseudo::kOverscrollBackdrop:
    case Pseudo::kCue:
    case Pseudo::kMarker:
    case Pseudo::kPart:
    case Pseudo::kPlaceholder:
    case Pseudo::kFileSelectorButton:
    case Pseudo::kResizer:
    case Pseudo::kScrollbar:
    case Pseudo::kScrollbarCorner:
    case Pseudo::kScrollbarButton:
    case Pseudo::kScrollbarThumb:
    case Pseudo::kScrollbarTrack:
    case Pseudo::kScrollbarTrackPiece:
    case Pseudo::kScrollMarker:
    case Pseudo::kScrollMarkerGroup:
    case Pseudo::kScrollButton:
    case Pseudo::kColumn:
    case Pseudo::kPicker:
    case Pseudo::kSelectListbox:
    case Pseudo::kSelection:
    case Pseudo::kWebKitCustomElement:
    case Pseudo::kSlotted:
    case Pseudo::kSearchText:
    case Pseudo::kTargetText:
    case Pseudo::kHighlight:
    case Pseudo::kSpellingError:
    case Pseudo::kGrammarError:
    case Pseudo::kViewTransition:
    case Pseudo::kViewTransitionGroup:
    case Pseudo::kViewTransitionGroupChildren:
    case Pseudo::kViewTransitionImagePair:
    case Pseudo::kViewTransitionOld:
    case Pseudo::kViewTransitionNew:
    case Pseudo::kDetailsContent:
    case Pseudo::kPermissionIcon:
    case Pseudo::kSkeleton:
      if (!is_element) {
        selector.pseudo_type = Pseudo::kUnknown;
      }
      return;
    // Only the user agent's style sheets use these.
    case Pseudo::kOverscrollAreaParent:
    case Pseudo::kBlinkInternalElement:
    case Pseudo::kHasDatalist:
    case Pseudo::kHasOpenMenuitem:
    case Pseudo::kHostHasNonAutoAppearance:
    case Pseudo::kIsHtml:
    case Pseudo::kListBox:
    case Pseudo::kMultiSelectFocus:
    case Pseudo::kSelectContainsInput:
    case Pseudo::kSpatialNavigationFocus:
    case Pseudo::kVideoPersistent:
    case Pseudo::kVideoPersistentAncestor:
    // Only @page uses these.
    case Pseudo::kFirstPage:
    case Pseudo::kLeftPage:
    case Pseudo::kRightPage:
      selector.pseudo_type = Pseudo::kUnknown;
      return;
    default:
      // The pseudo-classes.
      if (is_element) {
        selector.pseudo_type = Pseudo::kUnknown;
      }
      return;
  }
}

// Blink's CSSSelector::IsTreeAbidingPseudoElement().
bool IsTreeAbidingPseudoElement(const CssSimpleSelector& selector) {
  if (selector.match != Match::kPseudoElement) {
    return false;
  }
  switch (selector.pseudo_type) {
    case Pseudo::kCheckMark:
    case Pseudo::kBefore:
    case Pseudo::kAfter:
    case Pseudo::kExpandIcon:
    case Pseudo::kPickerIcon:
    case Pseudo::kInterestButton:
    case Pseudo::kMarker:
    case Pseudo::kPlaceholder:
    case Pseudo::kFileSelectorButton:
    case Pseudo::kBackdrop:
    case Pseudo::kOverscrollBackdrop:
    case Pseudo::kViewTransition:
    case Pseudo::kViewTransitionGroup:
    case Pseudo::kViewTransitionGroupChildren:
    case Pseudo::kViewTransitionImagePair:
    case Pseudo::kViewTransitionOld:
    case Pseudo::kViewTransitionNew:
    case Pseudo::kOverscrollAreaParent:
    case Pseudo::kSkeleton:
    case Pseudo::kScrollMarkerGroup:
    case Pseudo::kScrollMarker:
    case Pseudo::kScrollButton:
    case Pseudo::kDetailsContent:
    case Pseudo::kPicker:
    case Pseudo::kPermissionIcon:
    case Pseudo::kSelectListbox:
      return true;
    default:
      return false;
  }
}

// Blink's CSSSelector::IsElementBackedPseudoElement().
bool IsElementBackedPseudoElement(Pseudo type) {
  return type == Pseudo::kDetailsContent || type == Pseudo::kPicker ||
         type == Pseudo::kPermissionIcon || type == Pseudo::kSelectListbox;
}

// Blink's CSSSelector::IsAllowedAfterPart(): every pseudo-element but
// ::part() and ::slotted(), and the pseudo-classes that are not structural.
bool IsAllowedAfterPart(const CssSimpleSelector& selector) {
  if (selector.match != Match::kPseudoElement &&
      selector.match != Match::kPseudoClass) {
    return false;
  }
  switch (selector.pseudo_type) {
    case Pseudo::kCheckMark:
    case Pseudo::kBefore:
    case Pseudo::kAfter:
    case Pseudo::kExpandIcon:
    case Pseudo::kPickerIcon:
    case Pseudo::kInterestButton:
    case Pseudo::kPlaceholder:
    case Pseudo::kFileSelectorButton:
    case Pseudo::kFirstLine:
    case Pseudo::kFirstLetter:
    case Pseudo::kPicker:
    case Pseudo::kSelectListbox:
    case Pseudo::kSelection:
    case Pseudo::kSearchText:
    case Pseudo::kTargetText:
    case Pseudo::kHighlight:
    case Pseudo::kSpellingError:
    case Pseudo::kGrammarError:
    case Pseudo::kBackdrop:
    case Pseudo::kOverscrollBackdrop:
    case Pseudo::kCue:
    case Pseudo::kMarker:
    case Pseudo::kResizer:
    case Pseudo::kScrollbar:
    case Pseudo::kScrollbarButton:
    case Pseudo::kScrollbarCorner:
    case Pseudo::kScrollbarThumb:
    case Pseudo::kScrollbarTrack:
    case Pseudo::kScrollbarTrackPiece:
    case Pseudo::kScrollMarker:
    case Pseudo::kScrollMarkerGroup:
    case Pseudo::kScrollButton:
    case Pseudo::kColumn:
    case Pseudo::kWebKitCustomElement:
    case Pseudo::kBlinkInternalElement:
    case Pseudo::kDetailsContent:
    case Pseudo::kPermissionIcon:
    case Pseudo::kViewTransition:
    case Pseudo::kViewTransitionGroup:
    case Pseudo::kViewTransitionGroupChildren:
    case Pseudo::kViewTransitionImagePair:
    case Pseudo::kViewTransitionNew:
    case Pseudo::kViewTransitionOld:
    case Pseudo::kOverscrollAreaParent:
    case Pseudo::kSkeleton:
      return true;
    case Pseudo::kSlotted:
    case Pseudo::kPart:
      return false;
    case Pseudo::kAnimatedImage:
    case Pseudo::kAutofill:
    case Pseudo::kAutofillPreviewed:
    case Pseudo::kAutofillSelected:
    case Pseudo::kWebKitAutofill:
    case Pseudo::kActive:
    case Pseudo::kActiveOption:
    case Pseudo::kActiveViewTransition:
    case Pseudo::kActiveViewTransitionType:
    case Pseudo::kAnyLink:
    case Pseudo::kBuffering:
    case Pseudo::kChecked:
    case Pseudo::kDefault:
    case Pseudo::kDialogInTopLayer:
    case Pseudo::kDisabled:
    case Pseudo::kDrag:
    case Pseudo::kEnabled:
    case Pseudo::kFiltered:
    case Pseudo::kFocus:
    case Pseudo::kFocusVisible:
    case Pseudo::kFocusWithin:
    case Pseudo::kFullPageMedia:
    case Pseudo::kHasSlotted:
    case Pseudo::kHover:
    case Pseudo::kIndeterminate:
    case Pseudo::kInterestSource:
    case Pseudo::kInterestTarget:
    case Pseudo::kInvalid:
    case Pseudo::kLang:
    case Pseudo::kLink:
    case Pseudo::kLinkTo:
    case Pseudo::kMenulistPopoverWithMenulistAnchor:
    case Pseudo::kModal:
    case Pseudo::kMuted:
    case Pseudo::kOptional:
    case Pseudo::kOverscrollClosed:
    case Pseudo::kOverscrollOpen:
    case Pseudo::kPermissionGranted:
    case Pseudo::kPlaceholderShown:
    case Pseudo::kReadOnly:
    case Pseudo::kReadWrite:
    case Pseudo::kRequired:
    case Pseudo::kSeeking:
    case Pseudo::kSelectContainsInput:
    case Pseudo::kSelectHasSlottedButton:
    case Pseudo::kStalled:
    case Pseudo::kState:
    case Pseudo::kTarget:
    case Pseudo::kUserInvalid:
    case Pseudo::kUserValid:
    case Pseudo::kValid:
    case Pseudo::kVisited:
    case Pseudo::kVolumeLocked:
    case Pseudo::kWebkitAnyLink:
    case Pseudo::kWindowInactive:
    case Pseudo::kFullScreen:
    case Pseudo::kFullScreenAncestor:
    case Pseudo::kFullscreen:
    case Pseudo::kInRange:
    case Pseudo::kOutOfRange:
    case Pseudo::kPaused:
    case Pseudo::kPictureInPicture:
    case Pseudo::kPlaying:
    case Pseudo::kXrOverlay:
    case Pseudo::kDefined:
    case Pseudo::kDir:
    case Pseudo::kFutureCue:
    case Pseudo::kIsHtml:
    case Pseudo::kListBox:
    case Pseudo::kMultiSelectFocus:
    case Pseudo::kNavigationSource:
    case Pseudo::kOpen:
    case Pseudo::kPastCue:
    case Pseudo::kPopoverInTopLayer:
    case Pseudo::kPopoverOpen:
    case Pseudo::kRelativeAnchor:
    case Pseudo::kSpatialNavigationFocus:
    case Pseudo::kTargetCurrent:
    case Pseudo::kTargetBefore:
    case Pseudo::kTargetAfter:
    case Pseudo::kTextField:
    case Pseudo::kToolFormActive:
    case Pseudo::kToolSubmitActive:
    case Pseudo::kUnbounded:
    case Pseudo::kVideoPersistent:
    case Pseudo::kVideoPersistentAncestor:
    case Pseudo::kIs:
    case Pseudo::kNot:
    case Pseudo::kWhere:
      return true;
    default:
      // :-webkit-any(), &, the scrollbar pseudo-classes, :current, @page's,
      // the structural ones and those that read the tree.
      return false;
  }
}

bool IsScrollbarPseudoClass(Pseudo type) {
  switch (type) {
    case Pseudo::kEnabled:
    case Pseudo::kDisabled:
    case Pseudo::kHover:
    case Pseudo::kActive:
    case Pseudo::kHorizontal:
    case Pseudo::kVertical:
    case Pseudo::kDecrement:
    case Pseudo::kIncrement:
    case Pseudo::kStart:
    case Pseudo::kEnd:
    case Pseudo::kDoubleButton:
    case Pseudo::kSingleButton:
    case Pseudo::kNoButton:
    case Pseudo::kCornerPresent:
    case Pseudo::kWindowInactive:
      return true;
    default:
      return false;
  }
}

bool IsUserActionPseudoClass(Pseudo type) {
  switch (type) {
    case Pseudo::kHover:
    case Pseudo::kFocus:
    case Pseudo::kFocusVisible:
    case Pseudo::kFocusWithin:
    case Pseudo::kActive:
      return true;
    default:
      return false;
  }
}

// Blink's IsPseudoClassValidAfterPseudoElement().
bool IsPseudoClassValidAfterPseudoElement(Pseudo pseudo_class,
                                          Pseudo pseudo_element,
                                          const CssSelectorFeatures& features) {
  if (IsUserActionPseudoClass(pseudo_class) &&
      (pseudo_element == Pseudo::kScrollButton ||
       pseudo_element == Pseudo::kScrollMarker)) {
    return true;
  }
  switch (pseudo_element) {
    case Pseudo::kResizer:
    case Pseudo::kScrollbar:
    case Pseudo::kScrollbarCorner:
    case Pseudo::kScrollbarButton:
    case Pseudo::kScrollbarThumb:
    case Pseudo::kScrollbarTrack:
    case Pseudo::kScrollbarTrackPiece:
      return IsScrollbarPseudoClass(pseudo_class);
    case Pseudo::kSelection:
      return pseudo_class == Pseudo::kWindowInactive;
    case Pseudo::kWebKitCustomElement:
    case Pseudo::kBlinkInternalElement:
    case Pseudo::kFileSelectorButton:
      return IsUserActionPseudoClass(pseudo_class);
    case Pseudo::kViewTransitionGroup:
    case Pseudo::kViewTransitionGroupChildren:
    case Pseudo::kViewTransitionImagePair:
    case Pseudo::kViewTransitionOld:
    case Pseudo::kViewTransitionNew:
      return pseudo_class == Pseudo::kOnlyChild;
    case Pseudo::kSearchText:
      return pseudo_class == Pseudo::kCurrent;
    case Pseudo::kScrollMarkerGroup:
      return pseudo_class == Pseudo::kFocusWithin ||
             pseudo_class == Pseudo::kHover;
    case Pseudo::kScrollMarker:
      return pseudo_class == Pseudo::kTargetCurrent ||
             pseudo_class == Pseudo::kTargetBefore ||
             pseudo_class == Pseudo::kTargetAfter;
    case Pseudo::kScrollButton:
      return pseudo_class == Pseudo::kDisabled ||
             pseudo_class == Pseudo::kEnabled;
    case Pseudo::kAfter:
    case Pseudo::kBefore:
    case Pseudo::kMarker:
      return pseudo_class == Pseudo::kHover &&
             features.pseudo_elements_hoverable;
    default:
      return false;
  }
}

// Blink's IsSimpleSelectorValidAfterPseudoElement().
bool IsSimpleSelectorValidAfterPseudoElement(
    const CssSimpleSelector& selector,
    Pseudo pseudo_element,
    const CssSelectorFeatures& features) {
  switch (pseudo_element) {
    case Pseudo::kColumn:
      return selector.pseudo_type == Pseudo::kScrollMarker;
    case Pseudo::kUnknown:
      return true;
    case Pseudo::kAfter:
    case Pseudo::kBefore:
      if (selector.pseudo_type == Pseudo::kMarker && features.marker_nested) {
        return true;
      }
      break;
    case Pseudo::kSlotted:
      if (IsTreeAbidingPseudoElement(selector)) {
        return true;
      }
      break;
    default:
      break;
  }
  if ((pseudo_element == Pseudo::kPart ||
       IsElementBackedPseudoElement(pseudo_element)) &&
      IsAllowedAfterPart(selector)) {
    return true;
  }
  if (selector.match != Match::kPseudoClass) {
    return false;
  }
  switch (selector.pseudo_type) {
    case Pseudo::kIs:
    case Pseudo::kWhere:
    case Pseudo::kNot:
      return true;
    default:
      return IsPseudoClassValidAfterPseudoElement(selector.pseudo_type,
                                                  pseudo_element, features);
  }
}

// Blink's IsPseudoClassValidWithinHasArgument().
bool IsPseudoClassValidWithinHasArgument(const CssSimpleSelector& selector) {
  switch (selector.pseudo_type) {
    case Pseudo::kHas:
    case Pseudo::kOverscrollClosed:
    case Pseudo::kOverscrollOpen:
      return false;
    default:
      return true;
  }
}

// Blink's IsScrollButtonDirectionKeyword().
bool IsScrollButtonDirection(std::u16string_view ident) {
  for (const std::string_view keyword :
       {"up", "down", "left", "right", "block-start", "block-end",
        "inline-start", "inline-end"}) {
    if (EqualsIgnoringASCIICase(ident, keyword)) {
      return true;
    }
  }
  return false;
}

bool HasPseudoElementForRightmostCompound(const CssCompoundSelector& compound) {
  return std::any_of(compound.simples.begin(), compound.simples.end(),
                     [](const CssSimpleSelector& simple) {
                       return simple.match == Match::kPseudoElement;
                     });
}

CssRelation ToRelative(CssRelation relation) {
  switch (relation) {
    case CssRelation::kChild:
      return CssRelation::kRelativeChild;
    case CssRelation::kDirectAdjacent:
      return CssRelation::kRelativeDirectAdjacent;
    case CssRelation::kIndirectAdjacent:
      return CssRelation::kRelativeIndirectAdjacent;
    default:
      return CssRelation::kRelativeDescendant;
  }
}

class Parser {
 public:
  explicit Parser(const CssSelectorParserContext& context)
      : context_(context), features_(context_.features) {}

  // Blink's CSSSelectorParser::ParseSelector().
  std::unique_ptr<CssSelectorList> ParseSelector(CssTokenStream& stream) {
    stream.ConsumeWhitespace();
    std::unique_ptr<CssSelectorList> list = ConsumeComplexSelectorList(stream);
    if (!list || !stream.AtEnd()) {
      return nullptr;
    }
    return list;
  }

 private:
  // Blink's DisallowPseudoElementsScope.
  class DisallowPseudoElementsScope {
   public:
    explicit DisallowPseudoElementsScope(Parser& parser)
        : reset_(&parser.disallow_pseudo_elements_, true) {}

   private:
    base::AutoReset<bool> reset_;
  };

  // Null, or a list of one complex selector or more.
  std::unique_ptr<CssSelectorList> ConsumeComplexSelectorList(
      CssTokenStream& stream) {
    auto list = std::make_unique<CssSelectorList>();
    std::optional<CssComplexSelector> complex = ConsumeComplexSelector(stream);
    if (!complex) {
      return nullptr;
    }
    list->complexes.push_back(std::move(*complex));
    while (!stream.AtEnd() && stream.Peek().type == CssTokenType::kComma) {
      stream.ConsumeIncludingWhitespace();
      complex = ConsumeComplexSelector(stream);
      if (!complex) {
        return nullptr;
      }
      list->complexes.push_back(std::move(*complex));
    }
    if (failed_parsing_) {
      return nullptr;
    }
    return list;
  }

  // Null, or a list of compounds, as ::slotted() and :host() take.
  std::unique_ptr<CssSelectorList> ConsumeCompoundSelectorList(
      CssTokenStream& stream) {
    auto list = std::make_unique<CssSelectorList>();
    while (true) {
      std::optional<CssCompoundSelector> compound =
          ConsumeCompoundSelector(stream);
      stream.ConsumeWhitespace();
      if (!compound) {
        return nullptr;
      }
      CssComplexSelector complex;
      complex.compounds.push_back(std::move(*compound));
      list->complexes.push_back(std::move(complex));
      if (stream.AtEnd() || stream.Peek().type != CssTokenType::kComma) {
        break;
      }
      stream.ConsumeIncludingWhitespace();
    }
    if (failed_parsing_) {
      return nullptr;
    }
    return list;
  }

  std::unique_ptr<CssSelectorList> ConsumeNestedSelectorList(
      CssTokenStream& stream) {
    if (inside_compound_pseudo_) {
      return ConsumeCompoundSelectorList(stream);
    }
    return ConsumeComplexSelectorList(stream);
  }

  // A list that drops the selectors that do not parse, as :is() and
  // :where() take, which may then be empty.
  std::unique_ptr<CssSelectorList> ConsumeForgivingNestedSelectorList(
      CssTokenStream& stream) {
    if (inside_compound_pseudo_) {
      return ConsumeForgivingCompoundSelectorList(stream);
    }
    auto list = std::make_unique<CssSelectorList>();
    while (!stream.AtEnd()) {
      base::AutoReset<bool> reset_failure(&failed_parsing_, false);
      const CssTokenStream::State state = stream.Save();
      std::optional<CssComplexSelector> complex =
          ConsumeComplexSelector(stream);
      if (!complex || failed_parsing_ || !AtEndOfComplexSelector(stream)) {
        complex.reset();
        stream.Restore(state);
        // Blink's AddPlaceholderSelectorIfNeeded() goes to the end of the
        // argument; the placeholder it may keep never matches.
        stream.SkipUntilPeekedTypeIs(CssTokenType::kComma);
      }
      if (complex) {
        list->complexes.push_back(std::move(*complex));
      }
      if (stream.Peek().type != CssTokenType::kComma) {
        break;
      }
      stream.ConsumeIncludingWhitespace();
    }
    return list;
  }

  std::unique_ptr<CssSelectorList> ConsumeForgivingCompoundSelectorList(
      CssTokenStream& stream) {
    auto list = std::make_unique<CssSelectorList>();
    while (!stream.AtEnd()) {
      base::AutoReset<bool> reset_failure(&failed_parsing_, false);
      std::optional<CssCompoundSelector> compound =
          ConsumeCompoundSelector(stream);
      stream.ConsumeWhitespace();
      if (!compound) {
        failed_parsing_ = true;
      }
      if (failed_parsing_ ||
          (!stream.AtEnd() && stream.Peek().type != CssTokenType::kComma)) {
        stream.SkipUntilPeekedTypeIs(CssTokenType::kComma);
      } else {
        CssComplexSelector complex;
        complex.compounds.push_back(std::move(*compound));
        list->complexes.push_back(std::move(complex));
      }
      if (stream.AtEnd()) {
        break;
      }
      stream.ConsumeIncludingWhitespace();
    }
    return list;
  }

  // The relative selectors of :has().
  std::unique_ptr<CssSelectorList> ConsumeRelativeSelectorList(
      CssTokenStream& stream) {
    auto list = std::make_unique<CssSelectorList>();
    std::optional<CssComplexSelector> complex = ConsumeRelativeSelector(stream);
    if (!complex) {
      return nullptr;
    }
    list->complexes.push_back(std::move(*complex));
    while (!stream.AtEnd() && stream.Peek().type == CssTokenType::kComma) {
      stream.ConsumeIncludingWhitespace();
      complex = ConsumeRelativeSelector(stream);
      if (!complex) {
        return nullptr;
      }
      list->complexes.push_back(std::move(*complex));
    }
    if (failed_parsing_) {
      return nullptr;
    }
    // :has() may not follow a pseudo-element, nor be in a pseudo-class that
    // takes only compounds.
    if (inside_compound_pseudo_ ||
        restricting_pseudo_element_ != Pseudo::kUnknown) {
      return nullptr;
    }
    return list;
  }

  std::optional<CssComplexSelector> ConsumeRelativeSelector(
      CssTokenStream& stream) {
    CssComplexSelector complex;
    CssCompoundSelector anchor;
    CssSimpleSelector anchor_selector(Match::kPseudoClass);
    anchor_selector.pseudo_type = Pseudo::kRelativeAnchor;
    anchor_selector.name = u"-internal-relative-anchor";
    anchor.simples.push_back(std::move(anchor_selector));
    complex.compounds.push_back(std::move(anchor));
    CssRelation relation = ToRelative(ConsumeCombinator(stream));
    if (!ConsumePartialComplexSelector(stream, relation,
                                       /*previous_has_pseudo_element=*/false,
                                       complex)) {
      return std::nullopt;
    }
    return complex;
  }

  static bool AtEndOfComplexSelector(const CssTokenStream& stream) {
    const CssToken& token = stream.Peek();
    return stream.AtEnd() || token.type == CssTokenType::kLeftBrace ||
           token.type == CssTokenType::kComma;
  }

  std::optional<CssComplexSelector> ConsumeComplexSelector(
      CssTokenStream& stream) {
    std::optional<CssCompoundSelector> compound =
        ConsumeCompoundSelector(stream);
    if (!compound) {
      return std::nullopt;
    }
    CssComplexSelector complex;
    const bool has_pseudo_element =
        HasPseudoElementForRightmostCompound(*compound);
    complex.compounds.push_back(std::move(*compound));
    CssRelation relation = ConsumeCombinator(stream);
    if (relation != CssRelation::kNone &&
        !ConsumePartialComplexSelector(stream, relation, has_pseudo_element,
                                       complex)) {
      return std::nullopt;
    }
    return complex;
  }

  bool ConsumePartialComplexSelector(CssTokenStream& stream,
                                     CssRelation relation,
                                     bool previous_has_pseudo_element,
                                     CssComplexSelector& complex) {
    do {
      std::optional<CssCompoundSelector> compound =
          ConsumeCompoundSelector(stream);
      if (!compound) {
        // A combinator with nothing after it, such as "a >", does not parse,
        // but whitespace at the end is no combinator.
        return relation == CssRelation::kDescendant;
      }
      compound->relation = relation;
      if (previous_has_pseudo_element) {
        // Nothing may follow a compound with a pseudo-element.
        return false;
      }
      previous_has_pseudo_element =
          HasPseudoElementForRightmostCompound(*compound);
      complex.compounds.push_back(std::move(*compound));
    } while ((relation = ConsumeCombinator(stream)) != CssRelation::kNone);
    return true;
  }

  // Blink's ConsumeCombinator(): kNone where Blink gives kSubSelector.
  static CssRelation ConsumeCombinator(CssTokenStream& stream) {
    CssRelation fallback = CssRelation::kNone;
    while (stream.Peek().type == CssTokenType::kWhitespace) {
      stream.Consume();
      fallback = CssRelation::kDescendant;
    }
    const CssToken& token = stream.Peek();
    if (token.type != CssTokenType::kDelimiter) {
      return fallback;
    }
    CssRelation relation;
    switch (token.delimiter) {
      case '+':
        relation = CssRelation::kDirectAdjacent;
        break;
      case '~':
        relation = CssRelation::kIndirectAdjacent;
        break;
      case '>':
        relation = CssRelation::kChild;
        break;
      default:
        return fallback;
    }
    stream.ConsumeIncludingWhitespace();
    return relation;
  }

  // The namespace that `prefix` names: none for no prefix, as there is no
  // default namespace, and null for a prefix that no @namespace rule
  // declares, which is every prefix but "*" and the empty one.
  static std::optional<Namespace> DetermineNamespace(
      const std::optional<std::u16string>& prefix) {
    if (!prefix || *prefix == u"*") {
      return Namespace::kAny;
    }
    if (prefix->empty()) {
      return Namespace::kNone;
    }
    return std::nullopt;
  }

  std::optional<CssCompoundSelector> ConsumeCompoundSelector(
      CssTokenStream& stream) {
    base::AutoReset<Pseudo> reset_restricting(&restricting_pseudo_element_,
                                              restricting_pseudo_element_);
    std::u16string element_name;
    std::optional<std::u16string> namespace_prefix;
    const bool has_q_name = ConsumeName(stream, element_name, namespace_prefix);
    if (context_.html_document) {
      element_name = base::ToLowerASCII(element_name);
    }
    // A type selector may not follow a pseudo-element, as in
    // "::part(x):is(div)".
    if (restricting_pseudo_element_ != Pseudo::kUnknown && has_q_name) {
      failed_parsing_ = true;
      return std::nullopt;
    }
    CssCompoundSelector compound;
    while (ConsumeSimpleSelector(stream, compound.simples)) {
      const CssSimpleSelector& simple = compound.simples.back();
      if (simple.match == Match::kPseudoElement) {
        restricting_pseudo_element_ = simple.pseudo_type;
      }
    }
    if (compound.simples.empty() && !has_q_name) {
      return std::nullopt;
    }
    if (has_q_name) {
      const std::optional<Namespace> name_space =
          DetermineNamespace(namespace_prefix);
      if (!name_space) {
        failed_parsing_ = true;
        if (compound.simples.empty()) {
          return std::nullopt;
        }
        return compound;
      }
      // "*" in any namespace matches every element, and Blink leaves it
      // out.
      if (element_name != u"*" || *name_space != Namespace::kAny) {
        CssSimpleSelector tag(Match::kTag);
        tag.name = std::move(element_name);
        tag.name_space = *name_space;
        compound.simples.insert(compound.simples.begin(), std::move(tag));
      } else if (compound.simples.empty()) {
        CssSimpleSelector tag(Match::kTag);
        tag.name = u"*";
        compound.simples.push_back(std::move(tag));
      }
    }
    return compound;
  }

  bool ConsumeSimpleSelector(CssTokenStream& stream,
                             std::vector<CssSimpleSelector>& simples) {
    const CssToken& token = stream.Peek();
    bool ok;
    if (token.type == CssTokenType::kHash) {
      ok = ConsumeId(stream, simples);
    } else if (IsDelimiter(token, '.')) {
      ok = ConsumeClass(stream, simples);
    } else if (token.type == CssTokenType::kLeftBracket) {
      ok = ConsumeAttribute(stream, simples);
    } else if (token.type == CssTokenType::kColon) {
      ok = ConsumePseudo(stream, simples);
    } else if (IsDelimiter(token, '&')) {
      stream.Consume();
      CssSimpleSelector parent(Match::kPseudoClass);
      parent.pseudo_type = Pseudo::kParent;
      simples.push_back(std::move(parent));
      ok = true;
    } else {
      return false;
    }
    if (!ok || !IsSimpleSelectorValidAfterPseudoElement(
                   simples.back(), restricting_pseudo_element_, *features_)) {
      failed_parsing_ = true;
      return false;
    }
    return true;
  }

  // A type selector's or an attribute's name, with its namespace prefix if
  // it has one. Returns false, with nothing consumed after a "|" that no
  // name follows, if there is none.
  static bool ConsumeName(CssTokenStream& stream,
                          std::u16string& name,
                          std::optional<std::u16string>& namespace_prefix) {
    name.clear();
    namespace_prefix.reset();
    const CssToken& first = stream.Peek();
    if (first.type == CssTokenType::kIdent) {
      name = first.value;
      stream.Consume();
    } else if (IsDelimiter(first, '*')) {
      name = u"*";
      stream.Consume();
    } else if (IsDelimiter(first, '|')) {
      // An empty prefix, which the "|" below ends.
    } else {
      return false;
    }
    if (!IsDelimiter(stream.Peek(), '|')) {
      return true;
    }
    const CssTokenStream::State state = stream.Save();
    stream.Consume();
    namespace_prefix = name;
    const CssToken& next = stream.Peek();
    if (next.type == CssTokenType::kIdent) {
      name = next.value;
      stream.Consume();
    } else if (IsDelimiter(next, '*')) {
      stream.Consume();
      name = u"*";
    } else {
      name.clear();
      namespace_prefix.reset();
      stream.Restore(state);
      return false;
    }
    return true;
  }

  bool ConsumeId(CssTokenStream& stream,
                 std::vector<CssSimpleSelector>& simples) {
    if (stream.Peek().hash_type != CssHashType::kId) {
      return false;
    }
    CssSimpleSelector selector(Match::kId);
    selector.name = stream.Consume().value;
    if (context_.quirks_mode) {
      selector.name = base::ToLowerASCII(selector.name);
    }
    simples.push_back(std::move(selector));
    return true;
  }

  bool ConsumeClass(CssTokenStream& stream,
                    std::vector<CssSimpleSelector>& simples) {
    stream.Consume();
    if (stream.Peek().type != CssTokenType::kIdent) {
      return false;
    }
    CssSimpleSelector selector(Match::kClass);
    selector.name = stream.Consume().value;
    if (context_.quirks_mode) {
      selector.name = base::ToLowerASCII(selector.name);
    }
    simples.push_back(std::move(selector));
    return true;
  }

  bool ConsumeAttribute(CssTokenStream& stream,
                        std::vector<CssSimpleSelector>& simples) {
    CssTokenStream::BlockGuard guard(stream);
    stream.ConsumeWhitespace();
    std::u16string attribute_name;
    std::optional<std::u16string> namespace_prefix;
    if (!ConsumeName(stream, attribute_name, namespace_prefix)) {
      return false;
    }
    if (attribute_name == u"*") {
      return false;
    }
    stream.ConsumeWhitespace();
    if (context_.html_document) {
      attribute_name = base::ToLowerASCII(attribute_name);
    }
    if (!DetermineNamespace(namespace_prefix)) {
      return false;
    }
    CssSimpleSelector selector(Match::kAttributeSet);
    selector.name = std::move(attribute_name);
    // An attribute selector without a prefix matches attributes in no
    // namespace.
    selector.name_space = namespace_prefix && *namespace_prefix == u"*"
                              ? Namespace::kAny
                              : Namespace::kNone;
    if (stream.AtEnd()) {
      simples.push_back(std::move(selector));
      return true;
    }
    selector.match = ConsumeAttributeMatch(stream);
    const CssToken& value = stream.Peek();
    if (value.type != CssTokenType::kIdent &&
        value.type != CssTokenType::kString) {
      return false;
    }
    selector.value = stream.ConsumeIncludingWhitespace().value;
    selector.attribute_case = ConsumeAttributeFlags(stream);
    if (!stream.AtEnd()) {
      return false;
    }
    simples.push_back(std::move(selector));
    return true;
  }

  Match ConsumeAttributeMatch(CssTokenStream& stream) {
    const CssToken& token = stream.Peek();
    Match match;
    switch (token.type) {
      case CssTokenType::kIncludeMatch:
        match = Match::kAttributeList;
        break;
      case CssTokenType::kDashMatch:
        match = Match::kAttributeHyphen;
        break;
      case CssTokenType::kPrefixMatch:
        match = Match::kAttributeBegin;
        break;
      case CssTokenType::kSuffixMatch:
        match = Match::kAttributeEnd;
        break;
      case CssTokenType::kSubstringMatch:
        match = Match::kAttributeContain;
        break;
      default:
        if (!IsDelimiter(token, '=')) {
          failed_parsing_ = true;
          return Match::kAttributeExact;
        }
        match = Match::kAttributeExact;
        break;
    }
    stream.ConsumeIncludingWhitespace();
    return match;
  }

  CssSimpleSelector::AttributeCase ConsumeAttributeFlags(
      CssTokenStream& stream) {
    if (stream.Peek().type != CssTokenType::kIdent) {
      return CssSimpleSelector::AttributeCase::kDefault;
    }
    const CssToken flag = stream.ConsumeIncludingWhitespace();
    if (EqualsIgnoringASCIICase(flag.value, "i")) {
      return CssSimpleSelector::AttributeCase::kInsensitive;
    }
    if (EqualsIgnoringASCIICase(flag.value, "s") &&
        features_->case_sensitive_selector) {
      return CssSimpleSelector::AttributeCase::kSensitive;
    }
    failed_parsing_ = true;
    return CssSimpleSelector::AttributeCase::kDefault;
  }

  // Blink's css_parsing_utils::ConsumeCustomIdent().
  std::optional<std::u16string> ConsumeCustomIdent(CssTokenStream& stream) {
    const CssToken& token = stream.Peek();
    if (token.type != CssTokenType::kIdent) {
      return std::nullopt;
    }
    for (const std::string_view keyword :
         {"inherit", "initial", "unset", "revert", "revert-layer", "default"}) {
      if (EqualsIgnoringASCIICase(token.value, keyword)) {
        return std::nullopt;
      }
    }
    if (features_->revert_rule &&
        EqualsIgnoringASCIICase(token.value, "revert-rule")) {
      return std::nullopt;
    }
    return stream.ConsumeIncludingWhitespace().value;
  }

  // One ident, then the end of the parentheses.
  static bool ConsumeOneIdent(CssTokenStream& stream,
                              CssSimpleSelector& selector) {
    const CssToken& ident = stream.Peek();
    if (ident.type != CssTokenType::kIdent) {
      return false;
    }
    selector.arguments.push_back(ident.value);
    stream.ConsumeIncludingWhitespace();
    return stream.AtEnd();
  }

  bool ConsumePseudo(CssTokenStream& stream,
                     std::vector<CssSimpleSelector>& simples) {
    stream.Consume();
    int colons = 1;
    if (stream.Peek().type == CssTokenType::kColon) {
      stream.Consume();
      ++colons;
    }
    const CssToken& token = stream.Peek();
    if (token.type != CssTokenType::kIdent &&
        token.type != CssTokenType::kFunction) {
      return false;
    }
    CssSimpleSelector selector(colons == 1 ? Match::kPseudoClass
                                           : Match::kPseudoElement);
    const bool has_arguments = token.type == CssTokenType::kFunction;
    UpdatePseudoType(selector, token.value, has_arguments, *features_);
    if (selector.match == Match::kPseudoElement && disallow_pseudo_elements_) {
      return false;
    }
    if (is_inside_has_argument_ &&
        !IsPseudoClassValidWithinHasArgument(selector)) {
      return false;
    }
    if (!has_arguments) {
      stream.Consume();
      if (selector.pseudo_type == Pseudo::kUnknown) {
        return false;
      }
      simples.push_back(std::move(selector));
      return true;
    }

    CssTokenStream::BlockGuard guard(stream);
    stream.ConsumeWhitespace();
    if (selector.pseudo_type == Pseudo::kUnknown) {
      return false;
    }
    switch (selector.pseudo_type) {
      case Pseudo::kIs:
      case Pseudo::kWhere: {
        // Chrome's stable set lacks CSSLogicalCombinationPseudo, which would
        // let pseudo-elements in.
        DisallowPseudoElementsScope disallow(*this);
        selector.selector_list = ConsumeForgivingNestedSelectorList(stream);
        if (!selector.selector_list || !stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kHost:
      case Pseudo::kHostContext:
      case Pseudo::kAny:
      case Pseudo::kCue: {
        DisallowPseudoElementsScope disallow(*this);
        base::AutoReset<bool> inside_compound(&inside_compound_pseudo_, true);
        selector.selector_list = ConsumeCompoundSelectorList(stream);
        if (!selector.selector_list || !stream.AtEnd()) {
          return false;
        }
        if (selector.selector_list->complexes.size() != 1 &&
            (selector.pseudo_type == Pseudo::kHost ||
             selector.pseudo_type == Pseudo::kHostContext)) {
          return false;
        }
        break;
      }
      case Pseudo::kHas: {
        DisallowPseudoElementsScope disallow(*this);
        base::AutoReset<bool> inside_has(&is_inside_has_argument_, true);
        selector.selector_list = ConsumeRelativeSelectorList(stream);
        if (!selector.selector_list || !stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kNot: {
        DisallowPseudoElementsScope disallow(*this);
        selector.selector_list = ConsumeNestedSelectorList(stream);
        if (!selector.selector_list || !stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kPicker: {
        const CssToken& ident = stream.Peek();
        if (ident.type != CssTokenType::kIdent ||
            !EqualsIgnoringASCIICase(ident.value, "select")) {
          return false;
        }
        selector.arguments.push_back(u"select");
        stream.ConsumeIncludingWhitespace();
        if (!stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kDir:
      case Pseudo::kState:
        if (!ConsumeOneIdent(stream, selector)) {
          return false;
        }
        break;
      case Pseudo::kPart:
        do {
          const CssToken& ident = stream.Peek();
          if (ident.type != CssTokenType::kIdent) {
            return false;
          }
          selector.arguments.push_back(ident.value);
          stream.ConsumeIncludingWhitespace();
        } while (!stream.AtEnd());
        break;
      case Pseudo::kActiveViewTransitionType:
        while (true) {
          const CssToken& ident = stream.Peek();
          if (ident.type != CssTokenType::kIdent) {
            return false;
          }
          selector.arguments.push_back(ident.value);
          stream.ConsumeIncludingWhitespace();
          if (stream.AtEnd()) {
            break;
          }
          if (stream.Peek().type != CssTokenType::kComma) {
            return false;
          }
          stream.ConsumeIncludingWhitespace();
          if (stream.AtEnd()) {
            return false;
          }
        }
        break;
      case Pseudo::kViewTransitionGroup:
      case Pseudo::kViewTransitionGroupChildren:
      case Pseudo::kViewTransitionImagePair:
      case Pseudo::kViewTransitionOld:
      case Pseudo::kViewTransitionNew:
        if (!ConsumeViewTransitionArguments(stream, selector)) {
          return false;
        }
        break;
      case Pseudo::kSlotted: {
        DisallowPseudoElementsScope disallow(*this);
        base::AutoReset<bool> inside_compound(&inside_compound_pseudo_, true);
        std::optional<CssCompoundSelector> inner =
            ConsumeCompoundSelector(stream);
        stream.ConsumeWhitespace();
        if (!inner || !stream.AtEnd()) {
          return false;
        }
        selector.selector_list = std::make_unique<CssSelectorList>();
        CssComplexSelector complex;
        complex.compounds.push_back(std::move(*inner));
        selector.selector_list->complexes.push_back(std::move(complex));
        break;
      }
      case Pseudo::kLang:
        if (!ConsumeLangArguments(stream, selector)) {
          return false;
        }
        break;
      case Pseudo::kNthChild:
      case Pseudo::kNthLastChild:
      case Pseudo::kNthOfType:
      case Pseudo::kNthLastOfType: {
        if (!ConsumeANPlusB(stream, selector.nth_a, selector.nth_b)) {
          return false;
        }
        stream.ConsumeWhitespace();
        if (stream.AtEnd()) {
          break;
        }
        if (selector.pseudo_type != Pseudo::kNthChild &&
            selector.pseudo_type != Pseudo::kNthLastChild) {
          return false;
        }
        if (stream.Peek().type != CssTokenType::kIdent ||
            stream.Consume().value != u"of") {
          return false;
        }
        stream.ConsumeWhitespace();
        selector.selector_list = ConsumeComplexSelectorList(stream);
        if (!selector.selector_list) {
          return false;
        }
        stream.ConsumeWhitespace();
        if (!stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kScrollButton: {
        const CssToken& ident = stream.Peek();
        if (ident.type == CssTokenType::kIdent) {
          if (!IsScrollButtonDirection(ident.value)) {
            return false;
          }
          selector.arguments.push_back(ident.value);
        } else if (IsDelimiter(ident, '*')) {
          selector.arguments.push_back(u"*");
        } else {
          return false;
        }
        stream.ConsumeIncludingWhitespace();
        if (!stream.AtEnd()) {
          return false;
        }
        break;
      }
      case Pseudo::kHighlight: {
        const CssToken& ident = stream.Peek();
        if (ident.type == CssTokenType::kIdent) {
          selector.arguments.push_back(ident.value);
        } else if (features_->highlight_universal && IsDelimiter(ident, '*')) {
          selector.arguments.push_back(u"*");
        } else {
          return false;
        }
        stream.ConsumeIncludingWhitespace();
        if (!stream.AtEnd()) {
          return false;
        }
        break;
      }
      default:
        // :link-to() needs RouteMatching, which Chrome's stable set lacks,
        // and a navigation location's parser.
        return false;
    }
    simples.push_back(std::move(selector));
    return true;
  }

  bool ConsumeLangArguments(CssTokenStream& stream,
                            CssSimpleSelector& selector) {
    if (!features_->lang_extended_ranges) {
      return ConsumeOneIdent(stream, selector);
    }
    while (!stream.AtEnd()) {
      const CssToken& range = stream.Peek();
      if (range.type != CssTokenType::kIdent &&
          range.type != CssTokenType::kString) {
        return false;
      }
      selector.arguments.push_back(range.value);
      stream.ConsumeIncludingWhitespace();
      if (!stream.AtEnd()) {
        if (stream.Peek().type != CssTokenType::kComma) {
          return false;
        }
        stream.ConsumeIncludingWhitespace();
        if (stream.AtEnd()) {
          return false;
        }
      }
    }
    return !selector.arguments.empty();
  }

  bool ConsumeViewTransitionArguments(CssTokenStream& stream,
                                      CssSimpleSelector& selector) {
    if (IsDelimiter(stream.Peek(), '.')) {
      selector.arguments.push_back(u"*");
    } else if (IsDelimiter(stream.Peek(), '*')) {
      selector.arguments.push_back(u"*");
      stream.Consume();
    } else if (std::optional<std::u16string> name =
                   ConsumeCustomIdent(stream)) {
      selector.arguments.push_back(std::move(*name));
    } else {
      return false;
    }
    while (!stream.AtEnd() && stream.Peek().type != CssTokenType::kWhitespace) {
      if (!IsDelimiter(stream.Peek(), '.')) {
        return false;
      }
      stream.Consume();
      std::optional<std::u16string> class_name = ConsumeCustomIdent(stream);
      if (!class_name) {
        return false;
      }
      selector.arguments.push_back(std::move(*class_name));
    }
    stream.ConsumeWhitespace();
    return stream.AtEnd();
  }

  // Blink's ConsumeANPlusB().
  static bool ConsumeANPlusB(CssTokenStream& stream, int& a, int& b) {
    if (stream.AtEnd() || stream.Peek().block_type != CssBlockType::kNotBlock) {
      return false;
    }
    const CssToken token = stream.Consume();
    if (token.type == CssTokenType::kNumber &&
        token.numeric_value_type == CssNumericValueType::kInteger) {
      a = 0;
      b = ClampToInt(token.numeric_value);
      return true;
    }
    if (token.type == CssTokenType::kIdent) {
      if (EqualsIgnoringASCIICase(token.value, "odd")) {
        a = 2;
        b = 1;
        return true;
      }
      if (EqualsIgnoringASCIICase(token.value, "even")) {
        a = 2;
        b = 0;
        return true;
      }
    }
    std::u16string n_string;
    if (IsDelimiter(token, '+') && stream.Peek().type == CssTokenType::kIdent) {
      a = 1;
      n_string = stream.Consume().value;
    } else if (token.type == CssTokenType::kDimension &&
               token.numeric_value_type == CssNumericValueType::kInteger) {
      a = ClampToInt(token.numeric_value);
      n_string = token.value;
    } else if (token.type == CssTokenType::kIdent) {
      if (!token.value.empty() && token.value[0] == '-') {
        a = -1;
        n_string = token.value.substr(1);
      } else {
        a = 1;
        n_string = token.value;
      }
    }
    stream.ConsumeWhitespace();
    if (n_string.empty() || (n_string[0] != 'n' && n_string[0] != 'N')) {
      return false;
    }
    if (n_string.size() > 1 && n_string[1] != '-') {
      return false;
    }
    if (n_string.size() > 2) {
      const std::optional<int> parsed =
          StringToIntStrict(std::u16string_view(n_string).substr(1));
      b = parsed.value_or(0);
      return parsed.has_value();
    }
    CssNumericSign sign =
        n_string.size() == 1 ? CssNumericSign::kNoSign : CssNumericSign::kMinus;
    if (sign == CssNumericSign::kNoSign &&
        stream.Peek().type == CssTokenType::kDelimiter) {
      const char32_t delimiter = stream.ConsumeIncludingWhitespace().delimiter;
      if (delimiter == '+') {
        sign = CssNumericSign::kPlus;
      } else if (delimiter == '-') {
        sign = CssNumericSign::kMinus;
      } else {
        return false;
      }
    }
    if (sign == CssNumericSign::kNoSign &&
        stream.Peek().type != CssTokenType::kNumber) {
      b = 0;
      return true;
    }
    const CssToken& b_token = stream.Peek();
    if (b_token.type != CssTokenType::kNumber ||
        b_token.numeric_value_type != CssNumericValueType::kInteger) {
      return false;
    }
    if ((b_token.numeric_sign == CssNumericSign::kNoSign) ==
        (sign == CssNumericSign::kNoSign)) {
      return false;
    }
    b = ClampToInt(b_token.numeric_value);
    stream.Consume();
    if (sign == CssNumericSign::kMinus) {
      b = b == std::numeric_limits<int>::min() ? std::numeric_limits<int>::max()
                                               : -b;
    }
    return true;
  }

  const CssSelectorParserContext context_;
  const raw_ref<const CssSelectorFeatures> features_;
  bool failed_parsing_ = false;
  bool disallow_pseudo_elements_ = false;
  bool inside_compound_pseudo_ = false;
  bool is_inside_has_argument_ = false;
  Pseudo restricting_pseudo_element_ = Pseudo::kUnknown;
};

}  // namespace

CssSimpleSelector::CssSimpleSelector(Match match) : match(match) {}
CssSimpleSelector::CssSimpleSelector(CssSimpleSelector&&) = default;
CssSimpleSelector& CssSimpleSelector::operator=(CssSimpleSelector&&) = default;
CssSimpleSelector::~CssSimpleSelector() = default;

CssCompoundSelector::CssCompoundSelector() = default;
CssCompoundSelector::CssCompoundSelector(CssCompoundSelector&&) = default;
CssCompoundSelector& CssCompoundSelector::operator=(CssCompoundSelector&&) =
    default;
CssCompoundSelector::~CssCompoundSelector() = default;

CssComplexSelector::CssComplexSelector() = default;
CssComplexSelector::CssComplexSelector(CssComplexSelector&&) = default;
CssComplexSelector& CssComplexSelector::operator=(CssComplexSelector&&) =
    default;
CssComplexSelector::~CssComplexSelector() = default;

bool CssComplexSelector::HasPseudoElement() const {
  return std::any_of(compounds.begin(), compounds.end(),
                     HasPseudoElementForRightmostCompound);
}

CssSelectorList::CssSelectorList() = default;
CssSelectorList::CssSelectorList(CssSelectorList&&) = default;
CssSelectorList& CssSelectorList::operator=(CssSelectorList&&) = default;
CssSelectorList::~CssSelectorList() = default;

std::unique_ptr<CssSelectorList> ParseCssSelectorList(
    std::u16string_view text,
    const CssSelectorParserContext& context) {
  CssTokenStream stream(text);
  return Parser(context).ParseSelector(stream);
}

}  // namespace mitto
