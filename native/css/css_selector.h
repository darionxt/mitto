// A list of selectors as the selector parser makes it, with what Blink's
// CSSSelector holds of each simple selector:
// https://drafts.csswg.org/selectors/
//
// Blink keeps a list in one array, compounds stored right to left; this keeps
// a tree of lists, complex selectors and compounds, each from left to right,
// which holds the same and needs no pointer arithmetic to walk.
//
// Upstream: third_party/blink/renderer/core/css/css_selector.h
// Upstream: third_party/blink/renderer/core/css/css_selector.cc
// Upstream: third_party/blink/renderer/core/css/css_selector_list.h

#ifndef MITTO_CSS_CSS_SELECTOR_H_
#define MITTO_CSS_CSS_SELECTOR_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace mitto {

// Blink's CSSSelector::PseudoType: what a pseudo-class or pseudo-element is.
enum class CssPseudoType : uint8_t {
  kUnknown,
  kActive,
  kActiveOption,
  kActiveViewTransition,
  kActiveViewTransitionType,
  kAfter,
  kAnimatedImage,
  kAny,
  kAnyLink,
  kAutofill,
  kAutofillPreviewed,
  kAutofillSelected,
  kBackdrop,
  kBefore,
  kBlinkInternalElement,
  kBuffering,
  kCheckMark,
  kChecked,
  kColumn,
  kCornerPresent,
  kCue,
  kCurrent,
  kDecrement,
  kDefault,
  kDefined,
  kDetailsContent,
  kDialogInTopLayer,
  kDir,
  kDisabled,
  kDoubleButton,
  kDrag,
  kEmpty,
  kEnabled,
  kEnd,
  kExpandIcon,
  kFileSelectorButton,
  kFiltered,
  kFirstChild,
  kFirstLetter,
  kFirstLine,
  kFirstOfType,
  kFirstPage,
  kFocus,
  kFocusVisible,
  kFocusWithin,
  kFullPageMedia,
  kFullScreen,
  kFullScreenAncestor,
  kFullscreen,
  kFutureCue,
  kGrammarError,
  kHas,
  kHasDatalist,
  kHasOpenMenuitem,
  kHasSlotted,
  kHighlight,
  kHorizontal,
  kHost,
  kHostContext,
  kHostHasNonAutoAppearance,
  kHover,
  kInRange,
  kIncrement,
  kIndeterminate,
  kInterestButton,
  kInterestSource,
  kInterestTarget,
  kInvalid,
  kIs,
  kIsHtml,
  kLang,
  kLastChild,
  kLastOfType,
  kLeftPage,
  kLink,
  kLinkTo,
  kListBox,
  kMarker,
  kMenulistPopoverWithMenulistAnchor,
  kModal,
  kMultiSelectFocus,
  kMuted,
  kNavigationSource,
  kNoButton,
  kNot,
  kNthChild,
  kNthLastChild,
  kNthLastOfType,
  kNthOfType,
  kOnlyChild,
  kOnlyOfType,
  kOpen,
  kOptional,
  kOutOfRange,
  kOverscrollAreaParent,
  kOverscrollBackdrop,
  kOverscrollClosed,
  kOverscrollOpen,
  // The nesting selector, &, which outside a nested rule matches as :scope.
  kParent,
  kPart,
  kPastCue,
  kPaused,
  kPermissionGranted,
  kPermissionIcon,
  kPicker,
  kPickerIcon,
  kPictureInPicture,
  kPlaceholder,
  kPlaceholderShown,
  kPlaying,
  kPopoverInTopLayer,
  kPopoverOpen,
  kReadOnly,
  kReadWrite,
  // The anchor of a relative selector in :has(), which the parser puts first.
  kRelativeAnchor,
  kRequired,
  kResizer,
  kRightPage,
  kRoot,
  kScope,
  kScrollButton,
  kScrollMarker,
  kScrollMarkerGroup,
  kScrollbar,
  kScrollbarButton,
  kScrollbarCorner,
  kScrollbarThumb,
  kScrollbarTrack,
  kScrollbarTrackPiece,
  kSearchText,
  kSeeking,
  kSelectContainsInput,
  kSelectHasSlottedButton,
  kSelectListbox,
  kSelection,
  kSingleButton,
  kSkeleton,
  kSlotted,
  kSpatialNavigationFocus,
  kSpellingError,
  kStalled,
  kStart,
  kState,
  kTarget,
  kTargetAfter,
  kTargetBefore,
  kTargetCurrent,
  kTargetText,
  kTextField,
  kToolFormActive,
  kToolSubmitActive,
  kUnbounded,
  kUserInvalid,
  kUserValid,
  kValid,
  kVertical,
  kVideoPersistent,
  kVideoPersistentAncestor,
  kViewTransition,
  kViewTransitionGroup,
  kViewTransitionGroupChildren,
  kViewTransitionImagePair,
  kViewTransitionNew,
  kViewTransitionOld,
  kVisited,
  kVolumeLocked,
  kWebKitAutofill,
  kWebKitCustomElement,
  kWebkitAnyLink,
  kWhere,
  kWindowInactive,
  kXrOverlay,
};

struct CssSelectorList;

// One simple selector.
struct CssSimpleSelector {
  // Blink's CSSSelector::MatchType, but for kUniversalTag, which is a kTag
  // whose name is "*".
  enum class Match : uint8_t {
    kTag,
    kId,
    kClass,
    kAttributeSet,
    kAttributeExact,
    kAttributeList,
    kAttributeHyphen,
    kAttributeBegin,
    kAttributeEnd,
    kAttributeContain,
    kPseudoClass,
    kPseudoElement,
  };
  // The namespace that a type or attribute selector matches: any, which a
  // type selector without a prefix has with no default namespace, or none,
  // which an attribute selector without a prefix has.
  enum class Namespace : uint8_t {
    kAny,
    kNone,
  };
  // How an attribute selector compares values: as the attribute says, in any
  // case for the flag i, or in case for the flag s.
  enum class AttributeCase : uint8_t {
    kDefault,
    kInsensitive,
    kSensitive,
  };

  explicit CssSimpleSelector(Match match);
  CssSimpleSelector(CssSimpleSelector&&);
  CssSimpleSelector& operator=(CssSimpleSelector&&);
  ~CssSimpleSelector();

  bool IsAttributeSelector() const {
    return match >= Match::kAttributeSet && match <= Match::kAttributeContain;
  }

  Match match;
  CssPseudoType pseudo_type = CssPseudoType::kUnknown;
  // A type selector's local name, "*" for the universal selector; an ID's or
  // a class's name, in lower case in a document in quirks mode; an attribute
  // selector's local name; a pseudo-class's or pseudo-element's name in lower
  // case.
  std::u16string name;
  // An attribute selector's value.
  std::u16string value;
  Namespace name_space = Namespace::kAny;
  AttributeCase attribute_case = AttributeCase::kDefault;
  // The idents in the parentheses of :dir(), :state(), :lang(), ::part(),
  // ::picker(), ::highlight(), ::scroll-button(),
  // :active-view-transition-type() and the view transition pseudo-elements.
  std::vector<std::u16string> arguments;
  // An+B of the :nth-*() pseudo-classes.
  int nth_a = 0;
  int nth_b = 0;
  // The selectors in the parentheses of :is(), :where(), :not(), :has(),
  // :-webkit-any(), :host(), :host-context(), ::cue(), ::slotted(), and of
  // :nth-child() and :nth-last-child() after "of".
  std::unique_ptr<CssSelectorList> selector_list;
};

// The relation of a compound to the one before it, as Blink's
// CSSSelector::RelationType has it for the relations a parser makes outside
// nested rules.
enum class CssRelation : uint8_t {
  // The first compound of a complex selector.
  kNone,
  kDescendant,
  kChild,
  kDirectAdjacent,
  kIndirectAdjacent,
  // The relation of a relative selector's first compound to the anchor of
  // :has().
  kRelativeDescendant,
  kRelativeChild,
  kRelativeDirectAdjacent,
  kRelativeIndirectAdjacent,
};

struct CssCompoundSelector {
  CssCompoundSelector();
  CssCompoundSelector(CssCompoundSelector&&);
  CssCompoundSelector& operator=(CssCompoundSelector&&);
  ~CssCompoundSelector();

  CssRelation relation = CssRelation::kNone;
  // From left to right. Never empty: a type selector that matches any
  // element in any namespace is left out.
  std::vector<CssSimpleSelector> simples;
};

struct CssComplexSelector {
  CssComplexSelector();
  CssComplexSelector(CssComplexSelector&&);
  CssComplexSelector& operator=(CssComplexSelector&&);
  ~CssComplexSelector();

  // Whether one of its compounds holds a pseudo-element, which Blink's
  // SelectorQuery leaves out: no element matches it.
  bool HasPseudoElement() const;

  // From left to right.
  std::vector<CssCompoundSelector> compounds;
};

struct CssSelectorList {
  CssSelectorList();
  CssSelectorList(CssSelectorList&&);
  CssSelectorList& operator=(CssSelectorList&&);
  ~CssSelectorList();

  // Empty only for a forgiving list, such as that of :is(), where none of
  // its selectors parsed.
  std::vector<CssComplexSelector> complexes;
};

}  // namespace mitto

#endif  // MITTO_CSS_CSS_SELECTOR_H_
