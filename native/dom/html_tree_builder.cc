#include "mitto/dom/html_tree_builder.h"

#include <stddef.h>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/flat_set.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/text.h"
#include "mitto/html/html_token.h"
#include "mitto/html/html_tokenizer.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

namespace {

using Names = std::initializer_list<std::u16string_view>;

bool IsOneOf(std::u16string_view name, Names names) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool IsHtmlSpace(char16_t c) {
  return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool IsAllHtmlSpace(std::u16string_view text) {
  return std::all_of(text.begin(), text.end(), IsHtmlSpace);
}

// Blink's IsAllWhitespaceOrReplacementCharacters(), which decides whether
// characters clear the frameset-ok flag.
bool IsAllHtmlSpaceOrReplacement(std::u16string_view text) {
  return std::all_of(text.begin(), text.end(),
                     [](char16_t c) { return IsHtmlSpace(c) || c == 0xFFFD; });
}

bool IsHTMLElementNamed(const Element& element, Names names) {
  return element.IsHTMLElement() && IsOneOf(element.LocalName(), names);
}

bool IsSvgElementNamed(const Element& element, Names names) {
  return element.NamespaceURI() == html_names::kSvgNamespace &&
         IsOneOf(element.LocalName(), names);
}

bool IsMathMlElementNamed(const Element& element, Names names) {
  return element.NamespaceURI() == html_names::kMathMlNamespace &&
         IsOneOf(element.LocalName(), names);
}

const std::u16string* FindTokenAttribute(const HtmlToken& token,
                                         std::u16string_view name) {
  for (const HtmlAttribute& attribute : token.attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

constexpr Names kHeadingNames = {u"h1", u"h2", u"h3", u"h4", u"h5", u"h6"};

constexpr Names kFormattingNames = {
    u"a",    u"b", u"big",   u"code",   u"em",     u"font", u"i",
    u"nobr", u"s", u"small", u"strike", u"strong", u"tt",   u"u"};

// The HTML Standard's special elements of the HTML namespace, as Blink's
// HTMLStackItem::IsSpecialNode() has them: without keygen, search, source and
// track, which the standard has added since.
constexpr Names kSpecialNames = {
    u"address",  u"applet",   u"area",     u"article",    u"aside",
    u"base",     u"basefont", u"bgsound",  u"blockquote", u"body",
    u"br",       u"button",   u"caption",  u"center",     u"col",
    u"colgroup", u"dd",       u"details",  u"dir",        u"div",
    u"dl",       u"dt",       u"embed",    u"fieldset",   u"figcaption",
    u"figure",   u"footer",   u"form",     u"frame",      u"frameset",
    u"h1",       u"h2",       u"h3",       u"h4",         u"h5",
    u"h6",       u"head",     u"header",   u"hgroup",     u"hr",
    u"html",     u"iframe",   u"img",      u"input",      u"li",
    u"link",     u"listing",  u"main",     u"marquee",    u"menu",
    u"meta",     u"nav",      u"noembed",  u"noframes",   u"noscript",
    u"object",   u"ol",       u"p",        u"param",      u"plaintext",
    u"pre",      u"script",   u"section",  u"select",     u"style",
    u"summary",  u"table",    u"tbody",    u"tfoot",      u"thead",
    u"td",       u"template", u"textarea", u"th",         u"title",
    u"tr",       u"ul",       u"wbr",      u"xmp"};

// The end tags that "generate implied end tags" closes.
constexpr Names kImpliedEndTagNames = {u"dd",     u"dt", u"li", u"optgroup",
                                       u"option", u"p",  u"rb", u"rp",
                                       u"rt",     u"rtc"};

constexpr Names kTableBodyNames = {u"tbody", u"tfoot", u"thead"};

// The elements whose children the standard foster parents.
constexpr Names kFosterParentingNames = {u"table", u"tbody", u"tfoot", u"thead",
                                         u"tr"};

// The start tags that end foreign content, as Blink's
// IsForeignContentBreakoutStartTag() has them.
constexpr Names kForeignContentBreakoutNames = {
    u"b",       u"big",  u"blockquote", u"body",  u"br",   u"center",
    u"code",    u"dd",   u"div",        u"dl",    u"dt",   u"em",
    u"embed",   u"h1",   u"h2",         u"h3",    u"h4",   u"h5",
    u"h6",      u"head", u"hr",         u"i",     u"img",  u"li",
    u"listing", u"menu", u"meta",       u"nobr",  u"ol",   u"p",
    u"pre",     u"ruby", u"s",          u"small", u"span", u"strong",
    u"strike",  u"sub",  u"sup",        u"table", u"tt",   u"u",
    u"ul",      u"var"};

// Blink's map from the lower-case names of SVG elements to their own: the
// mixed-case names of svg_tag_names.json5, and four that Chromium does not
// implement.
constexpr std::pair<std::u16string_view, std::u16string_view> kSvgTagNames[] = {
    {u"altglyph", u"altGlyph"},
    {u"altglyphdef", u"altGlyphDef"},
    {u"altglyphitem", u"altGlyphItem"},
    {u"animatecolor", u"animateColor"},
    {u"animatemotion", u"animateMotion"},
    {u"animatetransform", u"animateTransform"},
    {u"clippath", u"clipPath"},
    {u"feblend", u"feBlend"},
    {u"fecolormatrix", u"feColorMatrix"},
    {u"fecomponenttransfer", u"feComponentTransfer"},
    {u"fecomposite", u"feComposite"},
    {u"feconvolvematrix", u"feConvolveMatrix"},
    {u"fediffuselighting", u"feDiffuseLighting"},
    {u"fedisplacementmap", u"feDisplacementMap"},
    {u"fedistantlight", u"feDistantLight"},
    {u"fedropshadow", u"feDropShadow"},
    {u"feflood", u"feFlood"},
    {u"fefunca", u"feFuncA"},
    {u"fefuncb", u"feFuncB"},
    {u"fefuncg", u"feFuncG"},
    {u"fefuncr", u"feFuncR"},
    {u"fegaussianblur", u"feGaussianBlur"},
    {u"feimage", u"feImage"},
    {u"femerge", u"feMerge"},
    {u"femergenode", u"feMergeNode"},
    {u"femorphology", u"feMorphology"},
    {u"feoffset", u"feOffset"},
    {u"fepointlight", u"fePointLight"},
    {u"fespecularlighting", u"feSpecularLighting"},
    {u"fespotlight", u"feSpotLight"},
    {u"fetile", u"feTile"},
    {u"feturbulence", u"feTurbulence"},
    {u"foreignobject", u"foreignObject"},
    {u"glyphref", u"glyphRef"},
    {u"lineargradient", u"linearGradient"},
    {u"radialgradient", u"radialGradient"},
    {u"textpath", u"textPath"},
};

// The same for SVG's attributes: the mixed-case names of
// svg_attribute_names.json5, and viewTarget.
constexpr std::pair<std::u16string_view, std::u16string_view>
    kSvgAttributeNames[] = {
        {u"attributename", u"attributeName"},
        {u"attributetype", u"attributeType"},
        {u"basefrequency", u"baseFrequency"},
        {u"baseprofile", u"baseProfile"},
        {u"calcmode", u"calcMode"},
        {u"clippathunits", u"clipPathUnits"},
        {u"diffuseconstant", u"diffuseConstant"},
        {u"edgemode", u"edgeMode"},
        {u"filterunits", u"filterUnits"},
        {u"glyphref", u"glyphRef"},
        {u"gradienttransform", u"gradientTransform"},
        {u"gradientunits", u"gradientUnits"},
        {u"kernelmatrix", u"kernelMatrix"},
        {u"kernelunitlength", u"kernelUnitLength"},
        {u"keypoints", u"keyPoints"},
        {u"keysplines", u"keySplines"},
        {u"keytimes", u"keyTimes"},
        {u"lengthadjust", u"lengthAdjust"},
        {u"limitingconeangle", u"limitingConeAngle"},
        {u"markerheight", u"markerHeight"},
        {u"markerunits", u"markerUnits"},
        {u"markerwidth", u"markerWidth"},
        {u"maskcontentunits", u"maskContentUnits"},
        {u"maskunits", u"maskUnits"},
        {u"numoctaves", u"numOctaves"},
        {u"pathlength", u"pathLength"},
        {u"patterncontentunits", u"patternContentUnits"},
        {u"patterntransform", u"patternTransform"},
        {u"patternunits", u"patternUnits"},
        {u"pointsatx", u"pointsAtX"},
        {u"pointsaty", u"pointsAtY"},
        {u"pointsatz", u"pointsAtZ"},
        {u"preservealpha", u"preserveAlpha"},
        {u"preserveaspectratio", u"preserveAspectRatio"},
        {u"primitiveunits", u"primitiveUnits"},
        {u"refx", u"refX"},
        {u"refy", u"refY"},
        {u"repeatcount", u"repeatCount"},
        {u"repeatdur", u"repeatDur"},
        {u"requiredextensions", u"requiredExtensions"},
        {u"requiredfeatures", u"requiredFeatures"},
        {u"specularconstant", u"specularConstant"},
        {u"specularexponent", u"specularExponent"},
        {u"spreadmethod", u"spreadMethod"},
        {u"startoffset", u"startOffset"},
        {u"stddeviation", u"stdDeviation"},
        {u"stitchtiles", u"stitchTiles"},
        {u"surfacescale", u"surfaceScale"},
        {u"systemlanguage", u"systemLanguage"},
        {u"tablevalues", u"tableValues"},
        {u"targetx", u"targetX"},
        {u"targety", u"targetY"},
        {u"textlength", u"textLength"},
        {u"viewbox", u"viewBox"},
        {u"viewtarget", u"viewTarget"},
        {u"xchannelselector", u"xChannelSelector"},
        {u"ychannelselector", u"yChannelSelector"},
        {u"zoomandpan", u"zoomAndPan"},
};

// The attributes that "adjust foreign attributes" puts in a namespace, as
// Blink's AdjustForeignAttributes() maps them: those of XLink, xml:lang,
// xml:space, xmlns and xmlns:xlink.
struct ForeignAttribute {
  std::u16string_view name;
  const char16_t* prefix;
  std::u16string_view local_name;
  const char16_t* namespace_uri;
};

constexpr ForeignAttribute kForeignAttributes[] = {
    {u"xlink:actuate", u"xlink", u"actuate", html_names::kXlinkNamespace},
    {u"xlink:arcrole", u"xlink", u"arcrole", html_names::kXlinkNamespace},
    {u"xlink:href", u"xlink", u"href", html_names::kXlinkNamespace},
    {u"xlink:role", u"xlink", u"role", html_names::kXlinkNamespace},
    {u"xlink:show", u"xlink", u"show", html_names::kXlinkNamespace},
    {u"xlink:title", u"xlink", u"title", html_names::kXlinkNamespace},
    {u"xlink:type", u"xlink", u"type", html_names::kXlinkNamespace},
    {u"xml:lang", u"xml", u"lang", html_names::kXmlNamespace},
    {u"xml:space", u"xml", u"space", html_names::kXmlNamespace},
    {u"xmlns", nullptr, u"xmlns", html_names::kXmlnsNamespace},
    {u"xmlns:xlink", u"xmlns", u"xlink", html_names::kXmlnsNamespace},
};

// The name that an attribute of a foreign element gets.
std::optional<QualifiedName> ForeignAttributeName(std::u16string_view name) {
  for (const ForeignAttribute& attribute : kForeignAttributes) {
    if (attribute.name == name) {
      return QualifiedName{
          .prefix = attribute.prefix
                        ? std::optional<std::u16string>(attribute.prefix)
                        : std::nullopt,
          .local_name = std::u16string(attribute.local_name),
          .namespace_uri = std::u16string(attribute.namespace_uri)};
    }
  }
  return std::nullopt;
}

template <size_t N>
void AdjustName(
    std::u16string& name,
    const std::pair<std::u16string_view, std::u16string_view> (&map)[N]) {
  for (const auto& [lower, adjusted] : map) {
    if (name == lower) {
      name = adjusted;
      return;
    }
  }
}

void AdjustSvgAttributes(HtmlToken& token) {
  for (HtmlAttribute& attribute : token.attributes) {
    AdjustName(attribute.name, kSvgAttributeNames);
  }
}

void AdjustMathMlAttributes(HtmlToken& token) {
  for (HtmlAttribute& attribute : token.attributes) {
    if (attribute.name == u"definitionurl") {
      attribute.name = u"definitionURL";
    }
  }
}

// The standard's quirks mode public identifiers, which a doctype's public
// identifier starting with any of puts the document in quirks mode.
constexpr Names kQuirksPublicIdPrefixes = {
    u"+//silmaril//dtd html pro v0r11 19970101//",
    u"-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
    u"-//as//dtd html 3.0 aswedit + extensions//",
    u"-//ietf//dtd html 2.0 level 1//",
    u"-//ietf//dtd html 2.0 level 2//",
    u"-//ietf//dtd html 2.0 strict level 1//",
    u"-//ietf//dtd html 2.0 strict level 2//",
    u"-//ietf//dtd html 2.0 strict//",
    u"-//ietf//dtd html 2.0//",
    u"-//ietf//dtd html 2.1e//",
    u"-//ietf//dtd html 3.0//",
    u"-//ietf//dtd html 3.2 final//",
    u"-//ietf//dtd html 3.2//",
    u"-//ietf//dtd html 3//",
    u"-//ietf//dtd html level 0//",
    u"-//ietf//dtd html level 1//",
    u"-//ietf//dtd html level 2//",
    u"-//ietf//dtd html level 3//",
    u"-//ietf//dtd html strict level 0//",
    u"-//ietf//dtd html strict level 1//",
    u"-//ietf//dtd html strict level 2//",
    u"-//ietf//dtd html strict level 3//",
    u"-//ietf//dtd html strict//",
    u"-//ietf//dtd html//",
    u"-//metrius//dtd metrius presentational//",
    u"-//microsoft//dtd internet explorer 2.0 html strict//",
    u"-//microsoft//dtd internet explorer 2.0 html//",
    u"-//microsoft//dtd internet explorer 2.0 tables//",
    u"-//microsoft//dtd internet explorer 3.0 html strict//",
    u"-//microsoft//dtd internet explorer 3.0 html//",
    u"-//microsoft//dtd internet explorer 3.0 tables//",
    u"-//netscape comm. corp.//dtd html//",
    u"-//netscape comm. corp.//dtd strict html//",
    u"-//o'reilly and associates//dtd html 2.0//",
    u"-//o'reilly and associates//dtd html extended 1.0//",
    u"-//o'reilly and associates//dtd html extended relaxed 1.0//",
    u"-//softquad software//dtd hotmetal pro "
    u"6.0::19990601::extensions to html 4.0//",
    u"-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
    u"-//spyglass//dtd html 2.0 extended//",
    u"-//sq//dtd html 2.0 hotmetal + extensions//",
    u"-//sun microsystems corp.//dtd hotjava html//",
    u"-//sun microsystems corp.//dtd hotjava strict html//",
    u"-//w3c//dtd html 3 1995-03-24//",
    u"-//w3c//dtd html 3.2 draft//",
    u"-//w3c//dtd html 3.2 final//",
    u"-//w3c//dtd html 3.2//",
    u"-//w3c//dtd html 3.2s draft//",
    u"-//w3c//dtd html 4.0 frameset//",
    u"-//w3c//dtd html 4.0 transitional//",
    u"-//w3c//dtd html experimental 19960712//",
    u"-//w3c//dtd html experimental 970421//",
    u"-//w3c//dtd w3 html//",
    u"-//w3o//dtd w3 html 3.0//",
    u"-//webtechs//dtd mozilla html 2.0//",
    u"-//webtechs//dtd mozilla html//"};

bool StartsWithIgnoringCase(std::u16string_view text,
                            std::u16string_view prefix) {
  return text.size() >= prefix.size() &&
         base::EqualsCaseInsensitiveASCII(text.substr(0, prefix.size()),
                                          prefix);
}

// Blink's HTMLConstructionSite::SetCompatibilityModeFromDoctype(), which
// takes a missing identifier for an empty one.
Document::CompatibilityMode CompatibilityModeFromDoctype(
    const HtmlToken& token) {
  using Mode = Document::CompatibilityMode;
  if (token.force_quirks) {
    return Mode::kQuirks;
  }
  const std::u16string public_id = token.public_identifier.value_or(u"");
  const std::u16string system_id = token.system_identifier.value_or(u"");
  const bool frameset_or_transitional =
      StartsWithIgnoringCase(public_id, u"-//w3c//dtd html 4.01 frameset//") ||
      StartsWithIgnoringCase(public_id,
                             u"-//w3c//dtd html 4.01 transitional//");
  if (token.name != u"html" ||
      std::any_of(kQuirksPublicIdPrefixes.begin(),
                  kQuirksPublicIdPrefixes.end(),
                  [&public_id](std::u16string_view prefix) {
                    return StartsWithIgnoringCase(public_id, prefix);
                  }) ||
      base::EqualsCaseInsensitiveASCII(
          public_id, u"-//w3o//dtd w3 html strict 3.0//en//") ||
      base::EqualsCaseInsensitiveASCII(public_id,
                                       u"-/w3c/dtd html 4.0 transitional/en") ||
      base::EqualsCaseInsensitiveASCII(public_id, u"html") ||
      base::EqualsCaseInsensitiveASCII(
          system_id,
          u"http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd") ||
      (system_id.empty() && frameset_or_transitional)) {
    return Mode::kQuirks;
  }
  if (StartsWithIgnoringCase(public_id, u"-//w3c//dtd xhtml 1.0 frameset//") ||
      StartsWithIgnoringCase(public_id,
                             u"-//w3c//dtd xhtml 1.0 transitional//") ||
      (!system_id.empty() && frameset_or_transitional)) {
    return Mode::kLimitedQuirks;
  }
  return Mode::kNoQuirks;
}

HtmlToken StartTag(std::u16string_view name) {
  HtmlToken token;
  token.type = HtmlToken::Type::kStartTag;
  token.name = name;
  return token;
}

HtmlToken EndTag(std::u16string_view name) {
  HtmlToken token;
  token.type = HtmlToken::Type::kEndTag;
  token.name = name;
  return token;
}

// Blink's limit on how deep the parser nests elements. An element deeper goes
// in beside its parent.
constexpr size_t kMaximumDomTreeDepth = 512;

// Where a node goes: before `next_child` in `parent`, or at its end.
struct InsertionLocation {
  cppgc::Persistent<Node> parent;
  cppgc::Persistent<Node> next_child;
};

}  // namespace

// The tree construction stage, as Blink's HTMLTreeBuilder and
// HTMLConstructionSite have it, without the parts of Blink's that a Page has
// no use for: the sanitizer, document patching, custom elements and the
// counters of its use.
class HtmlTreeBuilder {
 public:
  // For a document. What the builder refers to stays in `references`, which
  // outlives it.
  HtmlTreeBuilder(HtmlTreeBuilderReferences& references,
                  HtmlTokenizer& tokenizer)
      : references_(references),
        document_(&references.document()),
        tokenizer_(tokenizer) {}

  // For a fragment whose context is `context`, as the standard's fragment
  // parsing algorithm sets the builder up. Blink's root is the fragment
  // itself, where the standard's is an html element: the nodes that the
  // fragment ends up with are the same.
  HtmlTreeBuilder(HtmlTreeBuilderReferences& references,
                  Element& context,
                  HtmlTokenizer& tokenizer)
      : references_(references),
        document_(&references.document()),
        tokenizer_(tokenizer),
        fragment_context_(&context),
        context_entry_{&context, IsHtmlAnnotationXml(context)} {
    references_->Keep(&context);
    root_ = document_->CreateElement(html_names::kXhtmlNamespace, std::nullopt,
                                     u"html");
    references_->Keep(root_);
    open_elements_.push_back({root_, false});
    // Blink's TokenizerStateForContextElement(), which starts in PLAINTEXT
    // where the standard starts in RAWTEXT or script data, as Blink's fragment
    // parser reports no errors.
    if (IsHTMLElementNamed(context, {u"title", u"textarea"})) {
      tokenizer_->SetState(HtmlTokenizer::State::kRcdata);
    } else if (IsHTMLElementNamed(
                   context,
                   {u"style", u"xmp", u"iframe", u"noembed", u"noscript",
                    u"noframes", u"script", u"plaintext"})) {
      tokenizer_->SetState(HtmlTokenizer::State::kPlaintext);
    }
    if (context.HasHTMLTagName(u"template")) {
      template_modes_.push_back(Mode::kTemplateContents);
    }
    ResetInsertionModeAppropriately();
    // Blink leaves the form element pointer null for a context in a
    // template's contents, whose document is a template document.
    for (Node* node = context.GetDocument().IsTemplateDocument() ? nullptr
                                                                 : &context;
         node; node = node->ParentNode()) {
      if (node->IsElementNode() &&
          static_cast<Element*>(node)->HasHTMLTagName(u"form")) {
        form_element_ = static_cast<Element*>(node);
        references_->Keep(form_element_);
        break;
      }
    }
    UpdateTokenizerFlags();
  }

  HtmlTreeBuilder(const HtmlTreeBuilder&) = delete;
  HtmlTreeBuilder& operator=(const HtmlTreeBuilder&) = delete;

  // Processes one token. Returns the script element whose end tag it was.
  HTMLScriptElement* ProcessToken(HtmlToken& token) {
    ended_script_ = nullptr;
    CompactReferences();
    if (ShouldProcessTokenInForeignContent(token)) {
      ProcessTokenInForeignContent(token);
    } else {
      ProcessTokenInHtmlContent(token);
    }
    UpdateTokenizerFlags();
    return std::exchange(ended_script_, nullptr).get();
  }

  Element* fragment_root() const { return root_; }
  const std::vector<std::string>& skipped_rules() const {
    return skipped_rules_;
  }

 private:
  enum class Mode {
    kInitial,
    kBeforeHtml,
    kBeforeHead,
    kInHead,
    kAfterHead,
    kInBody,
    kText,
    kInTable,
    kInTableText,
    kInCaption,
    kInColumnGroup,
    kInTableBody,
    kInRow,
    kInCell,
    kTemplateContents,
    kAfterBody,
    kInFrameset,
    kAfterFrameset,
    kAfterAfterBody,
    kAfterAfterFrameset,
  };

  // An element on the stack of open elements.
  struct OpenElement {
    raw_ptr<Element> element;
    // Whether it is a MathML annotation-xml element whose start tag gave it
    // an encoding that makes it an HTML integration point. Blink reads the
    // attribute from the token, which a script cannot change.
    bool html_annotation_xml = false;
  };

  // An entry of the list of active formatting elements: an element, with the
  // start tag that made it, or a marker.
  struct FormattingEntry {
    bool IsMarker() const { return !element; }

    raw_ptr<Element> element;
    HtmlToken token;
  };

  static bool IsHtmlAnnotationXml(const Element& element) {
    if (!IsMathMlElementNamed(element, {u"annotation-xml"})) {
      return false;
    }
    const std::u16string* encoding = element.FindAttribute(u"encoding");
    return encoding &&
           (base::EqualsCaseInsensitiveASCII(*encoding, u"text/html") ||
            base::EqualsCaseInsensitiveASCII(*encoding,
                                             u"application/xhtml+xml"));
  }

  static bool IsHtmlIntegrationPoint(const OpenElement& entry) {
    return entry.html_annotation_xml ||
           IsSvgElementNamed(*entry.element,
                             {u"foreignObject", u"desc", u"title"});
  }

  static bool IsMathMlTextIntegrationPoint(const Element& element) {
    return IsMathMlElementNamed(element,
                                {u"mi", u"mo", u"mn", u"ms", u"mtext"});
  }

  // The elements of other namespaces that Blink takes for scope markers and
  // special elements.
  static bool IsForeignScopeMarker(const Element& element) {
    return IsMathMlElementNamed(element, {u"mi", u"mo", u"mn", u"ms", u"mtext",
                                          u"annotation-xml"}) ||
           IsSvgElementNamed(element, {u"foreignObject", u"desc", u"title"});
  }

  static bool IsSpecial(const Element& element) {
    return element.IsHTMLElement() ? IsOneOf(element.LocalName(), kSpecialNames)
                                   : IsForeignScopeMarker(element);
  }

  void Skip(const char* rule) {
    if (std::find(skipped_rules_.begin(), skipped_rules_.end(), rule) ==
        skipped_rules_.end()) {
      skipped_rules_.push_back(rule);
    }
  }

  bool IsFragment() const { return fragment_context_; }

  // Whether a template is open, which Blink's IsParsingTemplateContents()
  // asks.
  bool IsParsingTemplateContents() const {
    return std::any_of(open_elements_.begin(), open_elements_.end(),
                       [](const OpenElement& entry) {
                         return entry.element->HasHTMLTagName(u"template");
                       });
  }

  // The tokenizer's flags for the next token, as Blink's
  // UpdateTokenizerFlags() sets them.
  void UpdateTokenizerFlags() {
    bool in_other_namespace = false;
    bool in_foreign_content = false;
    if (!open_elements_.empty()) {
      const OpenElement& adjusted = AdjustedCurrentEntry();
      in_other_namespace = !adjusted.element->IsHTMLElement();
      in_foreign_content = in_other_namespace &&
                           !IsHtmlIntegrationPoint(adjusted) &&
                           !IsMathMlTextIntegrationPoint(*adjusted.element);
    }
    tokenizer_->SetForceNullCharacterReplacement(mode_ == Mode::kText ||
                                                 in_foreign_content);
    tokenizer_->SetAllowCdata(in_other_namespace);
  }

  // The stack of open elements.

  Element& CurrentNode() const { return *open_elements_.back().element; }
  bool CurrentNodeIs(std::u16string_view name) const {
    return !open_elements_.empty() && CurrentNode().HasHTMLTagName(name);
  }
  bool CurrentIsRootNode() const { return open_elements_.size() == 1; }

  // The standard's adjusted current node.
  const OpenElement& AdjustedCurrentEntry() const {
    if (IsFragment() && open_elements_.size() == 1) {
      return context_entry_;
    }
    return open_elements_.back();
  }

  void Push(Element* element, const HtmlToken* token = nullptr) {
    bool html_annotation_xml = false;
    if (token && IsMathMlElementNamed(*element, {u"annotation-xml"})) {
      const std::u16string* encoding = FindTokenAttribute(*token, u"encoding");
      html_annotation_xml =
          encoding &&
          (base::EqualsCaseInsensitiveASCII(*encoding, u"text/html") ||
           base::EqualsCaseInsensitiveASCII(*encoding,
                                            u"application/xhtml+xml"));
    }
    open_elements_.push_back({element, html_annotation_xml});
  }

  void Pop() {
    CHECK_GT(open_elements_.size(), 1u);
    open_elements_.pop_back();
  }

  // Pops elements until one of the HTML namespace named `name` is popped.
  void PopUntilPopped(std::u16string_view name) {
    while (!CurrentNode().HasHTMLTagName(name)) {
      Pop();
    }
    Pop();
  }

  void PopUntilPopped(const Element& element) {
    while (&CurrentNode() != &element) {
      Pop();
    }
    Pop();
  }

  void PopUntilNumberedHeaderPopped() {
    while (!IsHTMLElementNamed(CurrentNode(), kHeadingNames)) {
      Pop();
    }
    Pop();
  }

  // Blink's PopUntilTableScopeMarker() and its kin, which stop at the root.
  void PopUntil(Names names) {
    while (!CurrentIsRootNode() && !IsHTMLElementNamed(CurrentNode(), names)) {
      Pop();
    }
  }
  void PopUntilTableScopeMarker() {
    PopUntil({u"table", u"template", u"html"});
  }
  void PopUntilTableBodyScopeMarker() {
    PopUntil({u"tbody", u"tfoot", u"thead", u"template", u"html"});
  }
  void PopUntilTableRowScopeMarker() {
    PopUntil({u"tr", u"template", u"html"});
  }

  void PopUntilForeignContentScopeMarker() {
    while (true) {
      const OpenElement& entry = open_elements_.back();
      if (entry.element->IsHTMLElement() ||
          IsMathMlTextIntegrationPoint(*entry.element) ||
          IsHtmlIntegrationPoint(entry)) {
        return;
      }
      Pop();
    }
  }

  void PopAll() { open_elements_.clear(); }

  // The index of `element` in the stack.
  std::optional<size_t> StackIndexOf(const Element* element) const {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      if (open_elements_[i - 1].element.get() == element) {
        return i - 1;
      }
    }
    return std::nullopt;
  }

  bool StackContains(const Element* element) const {
    return StackIndexOf(element).has_value();
  }

  void RemoveFromStack(const Element* element) {
    if (const std::optional<size_t> index = StackIndexOf(element)) {
      open_elements_.erase(open_elements_.begin() +
                           static_cast<ptrdiff_t>(*index));
    }
  }

  // The topmost HTML element named `name` on the stack, as an index.
  std::optional<size_t> Topmost(std::u16string_view name) const {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      if (open_elements_[i - 1].element->HasHTMLTagName(name)) {
        return i - 1;
      }
    }
    return std::nullopt;
  }

  enum class Scope {
    kDefault,
    kListItem,
    kButton,
    kTable,
  };

  // Blink's scope markers, under which select is one, and so are the
  // elements of other namespaces that IsScopeMarkerNonHTML() names.
  static bool IsScopeMarker(const Element& element, Scope scope) {
    if (scope == Scope::kTable) {
      return IsHTMLElementNamed(element, {u"html", u"table", u"template"});
    }
    if (!element.IsHTMLElement()) {
      return IsForeignScopeMarker(element);
    }
    if (IsOneOf(element.LocalName(),
                {u"applet", u"caption", u"html", u"marquee", u"object",
                 u"table", u"td", u"template", u"th", u"select"})) {
      return true;
    }
    switch (scope) {
      case Scope::kListItem:
        return IsOneOf(element.LocalName(), {u"ol", u"ul"});
      case Scope::kButton:
        return element.LocalName() == u"button";
      default:
        return false;
    }
  }

  bool InScope(Names names, Scope scope = Scope::kDefault) const {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      const Element& element = *open_elements_[i - 1].element;
      if (IsHTMLElementNamed(element, names)) {
        return true;
      }
      if (IsScopeMarker(element, scope)) {
        return false;
      }
    }
    return false;
  }

  bool InScope(const Element& target) const {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      const Element& element = *open_elements_[i - 1].element;
      if (&element == &target) {
        return true;
      }
      if (IsScopeMarker(element, Scope::kDefault)) {
        return false;
      }
    }
    return false;
  }

  bool InTableScope(Names names) const { return InScope(names, Scope::kTable); }

  void GenerateImpliedEndTags(std::u16string_view except = u"") {
    while (IsHTMLElementNamed(CurrentNode(), kImpliedEndTagNames) &&
           CurrentNode().LocalName() != except) {
      Pop();
    }
  }

  // The list of active formatting elements.

  std::optional<size_t> FormattingIndexOf(const Element* element) const {
    for (size_t i = formatting_elements_.size(); i > 0; --i) {
      if (formatting_elements_[i - 1].element.get() == element) {
        return i - 1;
      }
    }
    return std::nullopt;
  }

  bool FormattingContains(const Element* element) const {
    return FormattingIndexOf(element).has_value();
  }

  void RemoveFromFormatting(const Element* element) {
    if (const std::optional<size_t> index = FormattingIndexOf(element)) {
      formatting_elements_.erase(formatting_elements_.begin() +
                                 static_cast<ptrdiff_t>(*index));
    }
  }

  void AppendMarker() { formatting_elements_.push_back({}); }

  void ClearFormattingToLastMarker() {
    while (!formatting_elements_.empty()) {
      const bool marker = formatting_elements_.back().IsMarker();
      formatting_elements_.pop_back();
      if (marker) {
        return;
      }
    }
  }

  // Blink's ClosestElementInScopeWithName(): the last element of the HTML
  // namespace named `name` after the last marker.
  Element* FormattingElementAfterLastMarker(std::u16string_view name) const {
    for (size_t i = formatting_elements_.size(); i > 0; --i) {
      const FormattingEntry& entry = formatting_elements_[i - 1];
      if (entry.IsMarker()) {
        return nullptr;
      }
      if (entry.element->HasHTMLTagName(name)) {
        return entry.element.get();
      }
    }
    return nullptr;
  }

  // Appends the current node with the token that made it, after the Noah's
  // Ark clause: no more than three of the same name and attributes after the
  // last marker.
  void AppendFormattingElement(const HtmlToken& token) {
    constexpr size_t kNoahsArkCapacity = 3;
    std::vector<const Element*> candidates;
    for (size_t i = formatting_elements_.size(); i > 0; --i) {
      const FormattingEntry& entry = formatting_elements_[i - 1];
      if (entry.IsMarker()) {
        break;
      }
      if (entry.token.name != token.name ||
          entry.element->NamespaceURI() != CurrentNode().NamespaceURI() ||
          entry.token.attributes.size() != token.attributes.size()) {
        continue;
      }
      const bool same_attributes =
          std::all_of(token.attributes.begin(), token.attributes.end(),
                      [&entry](const HtmlAttribute& attribute) {
                        const std::u16string* value =
                            FindTokenAttribute(entry.token, attribute.name);
                        return value && *value == attribute.value;
                      });
      if (same_attributes) {
        candidates.push_back(entry.element.get());
      }
    }
    for (size_t i = kNoahsArkCapacity - 1; i < candidates.size(); ++i) {
      RemoveFromFormatting(candidates[i]);
    }
    formatting_elements_.push_back({&CurrentNode(), token});
  }

  // Blink's IndexOfFirstUnopenFormattingElement().
  std::optional<size_t> FirstUnopenFormattingIndex() const {
    if (formatting_elements_.empty()) {
      return std::nullopt;
    }
    for (size_t i = formatting_elements_.size(); i > 0; --i) {
      const FormattingEntry& entry = formatting_elements_[i - 1];
      if (entry.IsMarker() || StackContains(entry.element.get())) {
        if (i < formatting_elements_.size()) {
          return i;
        }
        return std::nullopt;
      }
    }
    return 0;
  }

  void ReconstructActiveFormattingElements() {
    while (const std::optional<size_t> index = FirstUnopenFormattingIndex()) {
      const HtmlToken token = formatting_elements_[*index].token;
      Element* const element =
          CreateElement(token, html_names::kXhtmlNamespace);
      AttachOrFosterParent(element, &token);
      formatting_elements_[*index].element = element;
    }
  }

  // Making and inserting nodes, as Blink's HTMLConstructionSite does.

  static bool IsTemplate(const Node& node) {
    return node.IsElementNode() &&
           static_cast<const Element&>(node).HasHTMLTagName(u"template");
  }

  // Blink's OwnerDocumentForCurrentNode(): the document of what goes in the
  // current node, which for a template is its contents' template document.
  Document& OwnerDocumentForCurrentNode() const {
    if (open_elements_.empty()) {
      return *document_;
    }
    Element& node = CurrentNode();
    if (IsTemplate(node)) {
      return static_cast<HTMLTemplateElement&>(node).Content()->GetDocument();
    }
    return node.GetDocument();
  }

  // Blink's "create an element for the token". The attributes of a foreign
  // element that "adjust foreign attributes" names go in their namespaces.
  Element* CreateElement(const HtmlToken& token,
                         const char16_t* namespace_uri) {
    Element* const element = OwnerDocumentForCurrentNode().CreateElement(
        namespace_uri, std::nullopt, token.name);
    const bool foreign = namespace_uri != html_names::kXhtmlNamespace;
    for (const HtmlAttribute& attribute : token.attributes) {
      if (const std::optional<QualifiedName> name =
              foreign ? ForeignAttributeName(attribute.name) : std::nullopt) {
        element->SetAttributeWithoutValidation(*name, attribute.value);
      } else {
        element->SetAttributeWithoutValidation(attribute.name, attribute.value);
      }
    }
    // The token's is attribute gives the element its is value.
    if (const std::u16string* const is = element->FindAttribute(u"is")) {
      element->SetIsValue(*is);
    }
    references_->Keep(element);
    return element;
  }

  // Lets go of the elements that the builder no longer refers to, once they
  // are many, so that elements that a script removed can go.
  void CompactReferences() {
    size_t live = open_elements_.size() + formatting_elements_.size() + 4;
    if (references_->size() <= 2 * live + 256) {
      return;
    }
    std::vector<Element*> elements;
    elements.reserve(live);
    for (const OpenElement& entry : open_elements_) {
      elements.push_back(entry.element);
    }
    for (const FormattingEntry& entry : formatting_elements_) {
      if (!entry.IsMarker()) {
        elements.push_back(entry.element);
      }
    }
    for (Element* const element :
         {static_cast<Element*>(fragment_context_),
          static_cast<Element*>(root_), static_cast<Element*>(head_element_),
          static_cast<Element*>(form_element_)}) {
      if (element) {
        elements.push_back(element);
      }
    }
    references_->KeepOnly(elements);
  }

  static void DetachFromParent(Node& node) {
    if (Node* const parent = node.ParentNode()) {
      parent->ParserRemoveChild(node);
    }
  }

  // The standard's "appropriate place for inserting a node" for a template:
  // the end of its contents.
  static void AdjustForTemplate(InsertionLocation& location) {
    if (IsTemplate(*location.parent)) {
      location.parent =
          static_cast<HTMLTemplateElement*>(location.parent.Get())->Content();
      location.next_child = nullptr;
    }
  }

  void Insert(InsertionLocation location, Node* child) {
    AdjustForTemplate(location);
    DetachFromParent(*child);
    if (location.next_child &&
        location.next_child->ParentNode() == location.parent.Get()) {
      location.parent->ParserInsertBefore(child, *location.next_child);
    } else {
      location.parent->ParserAppendChild(child);
    }
  }

  // Blink's Attach(): inserts `child` at `location`, or beside its parent
  // once the stack is deeper than Blink allows.
  void Attach(InsertionLocation location, Node* child) {
    if (open_elements_.size() > kMaximumDomTreeDepth + 1 &&
        location.parent->ParentNode()) {
      location.parent = location.parent->ParentNode();
    }
    Insert(std::move(location), child);
  }

  InsertionLocation CurrentInsertionLocation() const {
    return {&CurrentNode(), nullptr};
  }

  bool ShouldFosterParent() const {
    return redirect_to_foster_parent_ &&
           IsHTMLElementNamed(CurrentNode(), kFosterParentingNames);
  }

  // Blink's FindFosterSite(), the substeps of step 2 of the standard's
  // "appropriate place for inserting a node".
  InsertionLocation FindFosterSite() const {
    const std::optional<size_t> last_template = Topmost(u"template");
    const std::optional<size_t> last_table = Topmost(u"table");
    if (last_template && (!last_table || *last_template > *last_table)) {
      return {open_elements_[*last_template].element.get(), nullptr};
    }
    if (!last_table) {
      return {open_elements_.front().element.get(), nullptr};
    }
    Element& table = *open_elements_[*last_table].element;
    if (Node* const parent = table.ParentNode()) {
      const bool is_template =
          parent->IsElementNode() &&
          static_cast<Element*>(parent)->HasHTMLTagName(u"template");
      return {parent, is_template ? nullptr : &table};
    }
    return {open_elements_[*last_table - 1].element.get(), nullptr};
  }

  void FosterParent(Node* node) { Insert(FindFosterSite(), node); }

  // Pushes `element` if `token` is not null, and inserts it where the current
  // node takes it or where foster parenting puts it.
  void AttachOrFosterParent(Element* element, const HtmlToken* token) {
    const bool should_foster_parent = ShouldFosterParent();
    const InsertionLocation location = CurrentInsertionLocation();
    if (token) {
      Push(element, token);
    }
    if (should_foster_parent) {
      FosterParent(element);
    } else {
      Attach(location, element);
    }
  }

  void InsertHTMLElement(const HtmlToken& token) {
    AttachOrFosterParent(CreateElement(token, html_names::kXhtmlNamespace),
                         &token);
  }

  void InsertSelfClosingHTMLElement(const HtmlToken& token) {
    AttachOrFosterParent(CreateElement(token, html_names::kXhtmlNamespace),
                         nullptr);
  }

  void InsertFormattingElement(const HtmlToken& token) {
    InsertHTMLElement(token);
    // Blink's feature CustomElementsDisableFormattingFixups, on for stable,
    // keeps customized built-in elements off the list.
    if (FindTokenAttribute(token, u"is")) {
      return;
    }
    AppendFormattingElement(token);
  }

  void InsertForeignElement(const HtmlToken& token,
                            const char16_t* namespace_uri) {
    Element* const element = CreateElement(token, namespace_uri);
    AttachOrFosterParent(element, token.self_closing ? nullptr : &token);
  }

  void InsertHTMLFormElement(const HtmlToken& token) {
    Element* const form = CreateElement(token, html_names::kXhtmlNamespace);
    if (!IsParsingTemplateContents()) {
      form_element_ = form;
    }
    AttachOrFosterParent(form, &token);
  }

  void InsertHTMLHeadElement(const HtmlToken& token) {
    Element* const head = CreateElement(token, html_names::kXhtmlNamespace);
    head_element_ = head;
    Attach(CurrentInsertionLocation(), head);
    Push(head);
  }

  void InsertHTMLBodyElement(const HtmlToken& token) {
    Element* const body = CreateElement(token, html_names::kXhtmlNamespace);
    Attach(CurrentInsertionLocation(), body);
    Push(body);
  }

  void InsertHTMLHtmlElement(const HtmlToken& token) {
    HtmlToken html = token;
    html.name = u"html";
    Element* const element = CreateElement(html, html_names::kXhtmlNamespace);
    document_->ParserAppendChild(element);
    Push(element);
  }

  // Blink's InsertHTMLTemplateElement(). A template with a shadowrootmode in
  // a document attaches a declarative shadow root to the current node, which
  // shadow trees will do; until then the template goes in as Blink puts one
  // whose root cannot attach.
  void InsertHTMLTemplateElement(const HtmlToken& token) {
    Element* const element = CreateElement(token, html_names::kXhtmlNamespace);
    const std::u16string* const mode =
        FindTokenAttribute(token, u"shadowrootmode");
    if (!IsFragment() && mode &&
        (base::EqualsCaseInsensitiveASCII(*mode, u"open") ||
         base::EqualsCaseInsensitiveASCII(*mode, u"closed"))) {
      Skip("declarative shadow roots");
    }
    const InsertionLocation location = CurrentInsertionLocation();
    Push(element, &token);
    Attach(location, element);
  }

  void MergeAttributes(Element& element, const HtmlToken& token) {
    for (const HtmlAttribute& attribute : token.attributes) {
      if (!element.FindAttribute(attribute.name)) {
        element.SetAttributeWithoutValidation(attribute.name, attribute.value);
      }
    }
  }

  // Blink's InsertTextNode(), which adds to the Text node before where the
  // characters go.
  void InsertText(std::u16string_view text) {
    if (text.empty()) {
      return;
    }
    InsertionLocation location =
        ShouldFosterParent() ? FindFosterSite() : CurrentInsertionLocation();
    AdjustForTemplate(location);
    Node* const previous = location.next_child
                               ? location.next_child->PreviousSibling()
                               : location.parent->LastChild();
    if (previous && previous->IsTextNode()) {
      static_cast<Text*>(previous)->AppendData(std::u16string(text));
      return;
    }
    Document& document = location.parent->GetDocument();
    Insert(std::move(location), Text::Create(document, std::u16string(text)));
  }

  void InsertComment(const std::u16string& data) {
    Attach(CurrentInsertionLocation(),
           Comment::Create(OwnerDocumentForCurrentNode(), data));
  }

  // Blink's RemoveChildIfValidForRemoval(): a node that cannot go in
  // `parent`, because it holds `parent` or `parent` is a document that has
  // its element, is taken out and goes nowhere.
  static bool RemoveIfCannotInsert(Node& parent, Node& child) {
    if ((parent.IsDocumentNode() &&
         static_cast<Document&>(parent).DocumentElement()) ||
        child.IsInclusiveAncestorOf(parent)) {
      DetachFromParent(child);
      return true;
    }
    return false;
  }

  // Blink's Reparent(): moves `child` to the end of `new_parent`.
  void Reparent(Element& new_parent, Node& child) {
    if (RemoveIfCannotInsert(new_parent, child)) {
      return;
    }
    Insert({&new_parent, nullptr}, &child);
  }

  // Blink's InsertAlreadyParsedChild().
  void InsertAlreadyParsedChild(Element& new_parent, Node& child) {
    if (IsHTMLElementNamed(new_parent, kFosterParentingNames)) {
      InsertionLocation location = FindFosterSite();
      if (RemoveIfCannotInsert(*location.parent, child)) {
        return;
      }
      Insert(std::move(location), &child);
      return;
    }
    Reparent(new_parent, child);
  }

  void TakeAllChildren(Element& new_parent, Element& old_parent) {
    while (Node* const child = old_parent.FirstChild()) {
      old_parent.ParserRemoveChild(*child);
      new_parent.ParserAppendChild(child);
    }
  }

  // Tokens.

  void ProcessTokenInHtmlContent(HtmlToken& token) {
    if (token.type == HtmlToken::Type::kCharacter) {
      ProcessCharacters(token.data);
      return;
    }
    skip_leading_newline_ = false;
    switch (token.type) {
      case HtmlToken::Type::kDoctype:
        ProcessDoctype(token);
        return;
      case HtmlToken::Type::kStartTag:
        ProcessStartTag(token);
        return;
      case HtmlToken::Type::kEndTag:
        ProcessEndTag(token);
        return;
      case HtmlToken::Type::kComment:
        ProcessComment(token);
        return;
      case HtmlToken::Type::kProcessingInstruction:
        ProcessProcessingInstruction();
        return;
      case HtmlToken::Type::kEndOfFile:
        ProcessEndOfFile();
        return;
      case HtmlToken::Type::kCharacter:
      case HtmlToken::Type::kUninitialized:
        NOTREACHED();
    }
  }

  void ProcessProcessingInstruction() {
    if (mode_ == Mode::kInTableText) {
      DefaultForInTableText();
    }
    // Blink inserts a ProcessingInstruction node, which has no class here.
    Skip("processing instructions");
  }

  void ProcessDoctype(const HtmlToken& token) {
    if (mode_ == Mode::kInitial) {
      document_->ParserAppendChild(DocumentType::Create(
          *document_, token.name, token.public_identifier.value_or(u""),
          token.system_identifier.value_or(u"")));
      document_->SetCompatibilityMode(CompatibilityModeFromDoctype(token));
      mode_ = Mode::kBeforeHtml;
      return;
    }
    if (mode_ == Mode::kInTableText) {
      DefaultForInTableText();
      ProcessDoctype(token);
    }
  }

  void ProcessComment(const HtmlToken& token) {
    switch (mode_) {
      case Mode::kInitial:
      case Mode::kBeforeHtml:
      case Mode::kAfterAfterBody:
      case Mode::kAfterAfterFrameset:
        Attach({document_.get(), nullptr},
               Comment::Create(*document_, token.data));
        return;
      case Mode::kAfterBody:
        Attach({open_elements_.front().element.get(), nullptr},
               Comment::Create(*document_, token.data));
        return;
      case Mode::kInTableText:
        DefaultForInTableText();
        ProcessComment(token);
        return;
      default:
        InsertComment(token.data);
        return;
    }
  }

  // "Anything else" in the modes before the body.

  void DefaultForInitial() {
    if (!IsFragment()) {
      document_->SetCompatibilityMode(Document::CompatibilityMode::kQuirks);
    }
    mode_ = Mode::kBeforeHtml;
  }

  void DefaultForBeforeHtml() {
    InsertHTMLHtmlElement(StartTag(u"html"));
    mode_ = Mode::kBeforeHead;
  }

  void DefaultForBeforeHead() {
    HtmlToken head = StartTag(u"head");
    ProcessStartTag(head);
  }

  void DefaultForInHead() { ProcessEndTag(EndTag(u"head")); }

  void DefaultForAfterHead() {
    HtmlToken body = StartTag(u"body");
    ProcessStartTag(body);
    frameset_ok_ = true;
  }

  void DefaultForInTableText() {
    const std::u16string characters = std::move(pending_table_characters_);
    pending_table_characters_.clear();
    if (!IsAllHtmlSpace(characters)) {
      const base::AutoReset<bool> redirect(&redirect_to_foster_parent_, true);
      ReconstructActiveFormattingElements();
      InsertText(characters);
      frameset_ok_ = false;
      mode_ = original_mode_;
      return;
    }
    InsertText(characters);
    mode_ = original_mode_;
  }

  // Characters.

  // The run of whitespace that `text` starts with, which it removes.
  static std::u16string_view TakeLeadingWhitespace(std::u16string_view& text) {
    size_t length = 0;
    while (length < text.size() && IsHtmlSpace(text[length])) {
      ++length;
    }
    const std::u16string_view whitespace = text.substr(0, length);
    text.remove_prefix(length);
    return whitespace;
  }

  void ProcessCharacters(std::u16string_view text) {
    if (skip_leading_newline_) {
      skip_leading_newline_ = false;
      if (!text.empty() && text[0] == '\n') {
        text.remove_prefix(1);
      }
    }
    while (!text.empty()) {
      switch (mode_) {
        case Mode::kInitial:
          TakeLeadingWhitespace(text);
          if (text.empty()) {
            return;
          }
          DefaultForInitial();
          [[fallthrough]];
        case Mode::kBeforeHtml:
          TakeLeadingWhitespace(text);
          if (text.empty()) {
            return;
          }
          DefaultForBeforeHtml();
          [[fallthrough]];
        case Mode::kBeforeHead:
          TakeLeadingWhitespace(text);
          if (text.empty()) {
            return;
          }
          DefaultForBeforeHead();
          [[fallthrough]];
        case Mode::kInHead:
          InsertText(TakeLeadingWhitespace(text));
          if (text.empty()) {
            return;
          }
          DefaultForInHead();
          [[fallthrough]];
        case Mode::kAfterHead:
          InsertText(TakeLeadingWhitespace(text));
          if (text.empty()) {
            return;
          }
          DefaultForAfterHead();
          [[fallthrough]];
        case Mode::kInBody:
        case Mode::kInCaption:
        case Mode::kInCell:
        case Mode::kTemplateContents:
          ProcessCharactersInBody(text);
          return;
        case Mode::kInTable:
        case Mode::kInTableBody:
        case Mode::kInRow:
          if (IsHTMLElementNamed(CurrentNode(), kFosterParentingNames)) {
            original_mode_ = mode_;
            mode_ = Mode::kInTableText;
            pending_table_characters_ += text;
          } else {
            const base::AutoReset<bool> redirect(&redirect_to_foster_parent_,
                                                 true);
            ProcessCharactersInBody(text);
          }
          return;
        case Mode::kInTableText:
          pending_table_characters_ += text;
          return;
        case Mode::kInColumnGroup:
          InsertText(TakeLeadingWhitespace(text));
          if (text.empty()) {
            return;
          }
          if (!ProcessColgroupEndTagForInColumnGroup()) {
            // The standard drops these characters.
            while (!text.empty() && !IsHtmlSpace(text[0])) {
              text.remove_prefix(1);
            }
          }
          continue;
        case Mode::kAfterBody:
        case Mode::kAfterAfterBody:
          // Blink inserts the whitespace in the current node without
          // reconstructing the active formatting elements, where the standard
          // takes it by the "in body" rules, which would.
          InsertText(TakeLeadingWhitespace(text));
          if (text.empty()) {
            return;
          }
          mode_ = Mode::kInBody;
          continue;
        case Mode::kText:
          InsertText(text);
          return;
        case Mode::kInFrameset:
        case Mode::kAfterFrameset:
        case Mode::kAfterAfterFrameset: {
          // Blink keeps the whitespace and drops the rest.
          std::u16string whitespace;
          for (const char16_t c : text) {
            if (IsHtmlSpace(c)) {
              whitespace += c;
            }
          }
          if (!whitespace.empty() && mode_ == Mode::kAfterAfterFrameset) {
            ReconstructActiveFormattingElements();
          }
          InsertText(whitespace);
          return;
        }
      }
    }
  }

  void ProcessCharactersInBody(std::u16string_view text) {
    ReconstructActiveFormattingElements();
    InsertText(text);
    if (frameset_ok_ && !IsAllHtmlSpaceOrReplacement(text)) {
      frameset_ok_ = false;
    }
  }

  // Start tags.

  void ProcessStartTag(HtmlToken& token) {
    const std::u16string& name = token.name;
    switch (mode_) {
      case Mode::kInitial:
        DefaultForInitial();
        [[fallthrough]];
      case Mode::kBeforeHtml:
        if (name == u"html") {
          InsertHTMLHtmlElement(token);
          mode_ = Mode::kBeforeHead;
          return;
        }
        DefaultForBeforeHtml();
        [[fallthrough]];
      case Mode::kBeforeHead:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
          return;
        }
        if (name == u"head") {
          InsertHTMLHeadElement(token);
          mode_ = Mode::kInHead;
          return;
        }
        DefaultForBeforeHead();
        [[fallthrough]];
      case Mode::kInHead:
        if (ProcessStartTagForInHead(token)) {
          return;
        }
        DefaultForInHead();
        [[fallthrough]];
      case Mode::kAfterHead:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
          return;
        }
        if (name == u"body") {
          frameset_ok_ = false;
          InsertHTMLBodyElement(token);
          mode_ = Mode::kInBody;
          return;
        }
        if (name == u"frameset") {
          InsertHTMLElement(token);
          mode_ = Mode::kInFrameset;
          return;
        }
        if (IsOneOf(name, {u"base", u"basefont", u"bgsound", u"link", u"meta",
                           u"noframes", u"script", u"style", u"template",
                           u"title"})) {
          // A parse error: the element goes in the head all the same.
          CHECK(head_element_);
          const cppgc::Persistent<Element> head = head_element_.get();
          Push(head.Get());
          ProcessStartTagForInHead(token);
          RemoveFromStack(head.Get());
          return;
        }
        if (name == u"head") {
          return;
        }
        DefaultForAfterHead();
        [[fallthrough]];
      case Mode::kInBody:
        ProcessStartTagForInBody(token);
        return;
      case Mode::kInTable:
        ProcessStartTagForInTable(token);
        return;
      case Mode::kInCaption:
        if (IsOneOf(name, {u"caption", u"col", u"colgroup", u"tbody", u"tfoot",
                           u"thead", u"td", u"th", u"tr"})) {
          if (!ProcessCaptionEndTagForInCaption()) {
            return;
          }
          ProcessStartTag(token);
          return;
        }
        ProcessStartTagForInBody(token);
        return;
      case Mode::kInColumnGroup:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
          return;
        }
        if (name == u"col") {
          InsertSelfClosingHTMLElement(token);
          return;
        }
        if (name == u"template") {
          ProcessTemplateStartTag(token);
          return;
        }
        if (!ProcessColgroupEndTagForInColumnGroup()) {
          return;
        }
        ProcessStartTag(token);
        return;
      case Mode::kInTableBody:
        if (name == u"tr") {
          PopUntilTableBodyScopeMarker();
          InsertHTMLElement(token);
          mode_ = Mode::kInRow;
          return;
        }
        if (name == u"td" || name == u"th") {
          HtmlToken tr = StartTag(u"tr");
          ProcessStartTag(tr);
          ProcessStartTag(token);
          return;
        }
        if (IsOneOf(name, {u"caption", u"col", u"colgroup", u"tbody", u"tfoot",
                           u"thead"})) {
          if (!InTableScope(kTableBodyNames)) {
            return;
          }
          PopUntilTableBodyScopeMarker();
          ProcessEndTag(EndTag(CurrentNode().LocalName()));
          ProcessStartTag(token);
          return;
        }
        ProcessStartTagForInTable(token);
        return;
      case Mode::kInRow:
        if (name == u"td" || name == u"th") {
          PopUntilTableRowScopeMarker();
          InsertHTMLElement(token);
          mode_ = Mode::kInCell;
          AppendMarker();
          return;
        }
        if (IsOneOf(name, {u"tr", u"caption", u"col", u"colgroup", u"tbody",
                           u"tfoot", u"thead"})) {
          if (!ProcessTrEndTagForInRow()) {
            return;
          }
          ProcessStartTag(token);
          return;
        }
        ProcessStartTagForInTable(token);
        return;
      case Mode::kInCell:
        if (IsOneOf(name, {u"caption", u"col", u"colgroup", u"td", u"th", u"tr",
                           u"tbody", u"tfoot", u"thead"})) {
          if (!InTableScope({u"td"}) && !InTableScope({u"th"})) {
            return;
          }
          CloseTheCell();
          ProcessStartTag(token);
          return;
        }
        ProcessStartTagForInBody(token);
        return;
      case Mode::kAfterBody:
      case Mode::kAfterAfterBody:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
          return;
        }
        mode_ = Mode::kInBody;
        ProcessStartTag(token);
        return;
      case Mode::kInFrameset:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
        } else if (name == u"frameset") {
          InsertHTMLElement(token);
        } else if (name == u"frame") {
          InsertSelfClosingHTMLElement(token);
        } else if (name == u"noframes") {
          ProcessStartTagForInHead(token);
        }
        return;
      case Mode::kAfterFrameset:
      case Mode::kAfterAfterFrameset:
        if (name == u"html") {
          ProcessHtmlStartTagInBody(token);
        } else if (name == u"noframes") {
          ProcessStartTagForInHead(token);
        }
        return;
      case Mode::kInTableText:
        DefaultForInTableText();
        ProcessStartTag(token);
        return;
      case Mode::kText:
        NOTREACHED();
      case Mode::kTemplateContents: {
        if (name == u"template") {
          ProcessTemplateStartTag(token);
          return;
        }
        if (IsOneOf(name, {u"link", u"script", u"style", u"meta"})) {
          ProcessStartTagForInHead(token);
          return;
        }
        Mode mode = Mode::kInBody;
        if (name == u"col") {
          mode = Mode::kInColumnGroup;
        } else if (IsOneOf(name, {u"caption", u"colgroup", u"tbody", u"tfoot",
                                  u"thead"})) {
          mode = Mode::kInTable;
        } else if (name == u"tr") {
          mode = Mode::kInTableBody;
        } else if (name == u"td" || name == u"th") {
          mode = Mode::kInRow;
        }
        CHECK(!template_modes_.empty());
        template_modes_.back() = mode;
        mode_ = mode;
        ProcessStartTag(token);
        return;
      }
    }
  }

  void ProcessHtmlStartTagInBody(const HtmlToken& token) {
    if (IsParsingTemplateContents() || IsFragment()) {
      return;
    }
    MergeAttributes(*open_elements_.front().element, token);
  }

  // The "in head" insertion mode's start tags. Returns false for "anything
  // else".
  bool ProcessStartTagForInHead(HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"html") {
      ProcessHtmlStartTagInBody(token);
      return true;
    }
    if (IsOneOf(name, {u"base", u"basefont", u"bgsound", u"link", u"meta"})) {
      InsertSelfClosingHTMLElement(token);
      return true;
    }
    if (name == u"title") {
      ProcessGenericTextStartTag(token, HtmlTokenizer::State::kRcdata);
      return true;
    }
    // A Page runs scripts, so noscript holds raw text.
    if (IsOneOf(name, {u"noscript", u"noframes", u"style"})) {
      ProcessGenericTextStartTag(token, HtmlTokenizer::State::kRawtext);
      return true;
    }
    if (name == u"script") {
      ProcessScriptStartTag(token);
      return true;
    }
    if (name == u"template") {
      ProcessTemplateStartTag(token);
      return true;
    }
    return name == u"head";
  }

  // The standard's generic raw text and RCDATA element parsing algorithms.
  void ProcessGenericTextStartTag(const HtmlToken& token,
                                  HtmlTokenizer::State state) {
    tokenizer_->SetState(state);
    original_mode_ = mode_;
    mode_ = Mode::kText;
    InsertHTMLElement(token);
  }

  // Blink's ProcessScriptStartTag() and InsertScriptElement(): the parser
  // marks the script it makes as its own, already started in a fragment, so
  // that it never runs, and gives it where its text starts.
  void ProcessScriptStartTag(const HtmlToken& token) {
    tokenizer_->SetState(HtmlTokenizer::State::kScriptData);
    original_mode_ = mode_;
    const HtmlTokenizer::ZeroBasedPosition position =
        tokenizer_->TextPosition();
    mode_ = Mode::kText;
    auto* const script = static_cast<HTMLScriptElement*>(
        CreateElement(token, html_names::kXhtmlNamespace));
    script->SetCreatedByParser(*document_);
    if (IsFragment()) {
      script->SetAlreadyStarted();
    }
    script->SetStartPosition(position.line, position.column);
    AttachOrFosterParent(script, &token);
  }

  void ProcessTemplateStartTag(const HtmlToken& token) {
    AppendMarker();
    InsertHTMLTemplateElement(token);
    frameset_ok_ = false;
    template_modes_.push_back(Mode::kTemplateContents);
    mode_ = Mode::kTemplateContents;
  }

  void ProcessFakePEndTagIfPInButtonScope() {
    if (InScope({u"p"}, Scope::kButton)) {
      ProcessEndTag(EndTag(u"p"));
    }
  }

  // Blink's ProcessCloseWhenNestedTag(), for li, dd and dt.
  void ProcessCloseWhenNestedTag(const HtmlToken& token, Names closes) {
    frameset_ok_ = false;
    for (size_t i = open_elements_.size(); i > 0; --i) {
      const Element& node = *open_elements_[i - 1].element;
      if (IsHTMLElementNamed(node, closes)) {
        ProcessEndTag(EndTag(node.LocalName()));
        break;
      }
      if (IsSpecial(node) &&
          !IsHTMLElementNamed(node, {u"address", u"div", u"p"})) {
        break;
      }
    }
    ProcessFakePEndTagIfPInButtonScope();
    InsertHTMLElement(token);
  }

  bool SecondElementIsBody() const {
    return open_elements_.size() >= 2 &&
           open_elements_[1].element->HasHTMLTagName(u"body");
  }

  void ProcessStartTagForInBody(HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"html") {
      ProcessHtmlStartTagInBody(token);
      return;
    }
    if (IsOneOf(name,
                {u"base", u"basefont", u"bgsound", u"link", u"meta",
                 u"noframes", u"script", u"style", u"title", u"template"})) {
      ProcessStartTagForInHead(token);
      return;
    }
    if (name == u"body") {
      if (!SecondElementIsBody() || IsParsingTemplateContents()) {
        return;
      }
      frameset_ok_ = false;
      MergeAttributes(*open_elements_[1].element, token);
      return;
    }
    if (name == u"frameset") {
      if (!SecondElementIsBody() || !frameset_ok_) {
        return;
      }
      const cppgc::Persistent<Element> body = open_elements_[1].element.get();
      DetachFromParent(*body);
      PopUntilPopped(*body);
      InsertHTMLElement(token);
      mode_ = Mode::kInFrameset;
      return;
    }
    if (IsOneOf(
            name,
            {u"address",  u"article",    u"aside",   u"blockquote", u"center",
             u"details",  u"dialog",     u"dir",     u"div",        u"dl",
             u"fieldset", u"figcaption", u"figure",  u"footer",     u"header",
             u"hgroup",   u"main",       u"menu",    u"nav",        u"ol",
             u"p",        u"search",     u"section", u"summary",    u"ul"})) {
      ProcessFakePEndTagIfPInButtonScope();
      InsertHTMLElement(token);
      return;
    }
    if (name == u"li") {
      ProcessCloseWhenNestedTag(token, {u"li"});
      return;
    }
    if (name == u"dd" || name == u"dt") {
      ProcessCloseWhenNestedTag(token, {u"dd", u"dt"});
      return;
    }
    if (name == u"input") {
      // Blink closes an open select, as its feature InputInSelect, which
      // would keep it open before the first option, is not on for stable.
      if (InScope({u"select"})) {
        ProcessEndTag(EndTag(u"select"));
      }
      const std::u16string* type = FindTokenAttribute(token, u"type");
      const bool disable_frameset =
          !type || !base::EqualsCaseInsensitiveASCII(*type, u"hidden");
      ReconstructActiveFormattingElements();
      InsertSelfClosingHTMLElement(token);
      if (disable_frameset) {
        frameset_ok_ = false;
      }
      return;
    }
    if (name == u"button") {
      if (InScope({u"button"})) {
        ProcessEndTag(EndTag(u"button"));
        ProcessStartTag(token);
        return;
      }
      ReconstructActiveFormattingElements();
      InsertHTMLElement(token);
      frameset_ok_ = false;
      return;
    }
    if (IsOneOf(name, kHeadingNames)) {
      ProcessFakePEndTagIfPInButtonScope();
      if (IsHTMLElementNamed(CurrentNode(), kHeadingNames)) {
        Pop();
      }
      InsertHTMLElement(token);
      return;
    }
    if (name == u"pre" || name == u"listing") {
      ProcessFakePEndTagIfPInButtonScope();
      InsertHTMLElement(token);
      skip_leading_newline_ = true;
      frameset_ok_ = false;
      return;
    }
    if (name == u"form") {
      if (form_element_ && !IsParsingTemplateContents()) {
        return;
      }
      ProcessFakePEndTagIfPInButtonScope();
      InsertHTMLFormElement(token);
      return;
    }
    if (name == u"plaintext") {
      ProcessFakePEndTagIfPInButtonScope();
      InsertHTMLElement(token);
      tokenizer_->SetState(HtmlTokenizer::State::kPlaintext);
      return;
    }
    if (name == u"a") {
      if (Element* const active_a = FormattingElementAfterLastMarker(u"a")) {
        const cppgc::Persistent<Element> a = active_a;
        ProcessEndTag(EndTag(u"a"));
        RemoveFromFormatting(a.Get());
        RemoveFromStack(a.Get());
      }
      ReconstructActiveFormattingElements();
      InsertFormattingElement(token);
      return;
    }
    if (name == u"nobr") {
      ReconstructActiveFormattingElements();
      if (InScope({u"nobr"})) {
        ProcessEndTag(EndTag(u"nobr"));
        ReconstructActiveFormattingElements();
      }
      InsertFormattingElement(token);
      return;
    }
    if (IsOneOf(name, kFormattingNames)) {
      ReconstructActiveFormattingElements();
      InsertFormattingElement(token);
      return;
    }
    if (IsOneOf(name, {u"applet", u"marquee", u"object"})) {
      ReconstructActiveFormattingElements();
      InsertHTMLElement(token);
      AppendMarker();
      frameset_ok_ = false;
      return;
    }
    if (name == u"table") {
      if (!document_->InQuirksMode() && InScope({u"p"}, Scope::kButton)) {
        ProcessEndTag(EndTag(u"p"));
      }
      InsertHTMLElement(token);
      frameset_ok_ = false;
      mode_ = Mode::kInTable;
      return;
    }
    if (IsOneOf(name, {u"area", u"br", u"embed", u"img", u"keygen", u"wbr",
                       u"image"})) {
      if (name == u"image") {
        token.name = u"img";
      }
      ReconstructActiveFormattingElements();
      InsertSelfClosingHTMLElement(token);
      frameset_ok_ = false;
      return;
    }
    if (IsOneOf(name, {u"param", u"source", u"track"})) {
      InsertSelfClosingHTMLElement(token);
      return;
    }
    if (name == u"hr") {
      ProcessFakePEndTagIfPInButtonScope();
      if (InScope({u"select"})) {
        GenerateImpliedEndTags();
      }
      InsertSelfClosingHTMLElement(token);
      frameset_ok_ = false;
      return;
    }
    if (name == u"textarea") {
      skip_leading_newline_ = true;
      tokenizer_->SetState(HtmlTokenizer::State::kRcdata);
      original_mode_ = mode_;
      frameset_ok_ = false;
      mode_ = Mode::kText;
      InsertHTMLElement(token);
      return;
    }
    if (name == u"xmp") {
      ProcessFakePEndTagIfPInButtonScope();
      ReconstructActiveFormattingElements();
      frameset_ok_ = false;
      ProcessGenericTextStartTag(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    if (name == u"iframe") {
      frameset_ok_ = false;
      ProcessGenericTextStartTag(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    if (name == u"noembed" || name == u"noscript") {
      ProcessGenericTextStartTag(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    // select, option and optgroup as the Engine Release parses them, with no
    // insertion mode of their own: a select inside another closes it.
    if (name == u"select") {
      if (IsFragment() && fragment_context_->HasHTMLTagName(u"select")) {
        return;
      }
      if (InScope({u"select"})) {
        ProcessEndTag(EndTag(u"select"));
        return;
      }
      ReconstructActiveFormattingElements();
      InsertHTMLElement(token);
      frameset_ok_ = false;
      return;
    }
    if (name == u"optgroup" || name == u"option") {
      if (InScope({u"select"})) {
        GenerateImpliedEndTags(name == u"option" ? u"optgroup" : u"");
      } else if (CurrentNodeIs(u"option")) {
        ProcessEndTag(EndTag(u"option"));
      }
      ReconstructActiveFormattingElements();
      InsertHTMLElement(token);
      return;
    }
    if (name == u"rb" || name == u"rtc") {
      if (InScope({u"ruby"})) {
        GenerateImpliedEndTags();
      }
      InsertHTMLElement(token);
      return;
    }
    if (name == u"rp" || name == u"rt") {
      if (InScope({u"ruby"})) {
        GenerateImpliedEndTags(u"rtc");
      }
      InsertHTMLElement(token);
      return;
    }
    if (IsOneOf(name, {u"caption", u"col", u"colgroup", u"frame", u"head",
                       u"tbody", u"tfoot", u"thead", u"th", u"td", u"tr"})) {
      return;  // A parse error, which the standard ignores.
    }
    if (name == u"math") {
      ReconstructActiveFormattingElements();
      AdjustMathMlAttributes(token);
      InsertForeignElement(token, html_names::kMathMlNamespace);
      return;
    }
    if (name == u"svg") {
      ReconstructActiveFormattingElements();
      AdjustSvgAttributes(token);
      InsertForeignElement(token, html_names::kSvgNamespace);
      return;
    }
    ReconstructActiveFormattingElements();
    InsertHTMLElement(token);
  }

  void ProcessStartTagForInTable(HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"caption") {
      PopUntilTableScopeMarker();
      AppendMarker();
      InsertHTMLElement(token);
      mode_ = Mode::kInCaption;
      return;
    }
    if (name == u"colgroup") {
      PopUntilTableScopeMarker();
      InsertHTMLElement(token);
      mode_ = Mode::kInColumnGroup;
      return;
    }
    if (name == u"col") {
      HtmlToken colgroup = StartTag(u"colgroup");
      ProcessStartTag(colgroup);
      ProcessStartTag(token);
      return;
    }
    if (IsOneOf(name, kTableBodyNames)) {
      PopUntilTableScopeMarker();
      InsertHTMLElement(token);
      mode_ = Mode::kInTableBody;
      return;
    }
    if (IsOneOf(name, {u"td", u"th", u"tr"})) {
      HtmlToken tbody = StartTag(u"tbody");
      ProcessStartTag(tbody);
      ProcessStartTag(token);
      return;
    }
    if (name == u"table") {
      if (!ProcessTableEndTagForInTable()) {
        return;
      }
      ProcessStartTag(token);
      return;
    }
    if (name == u"style" || name == u"script") {
      ProcessStartTagForInHead(token);
      return;
    }
    if (name == u"input") {
      const std::u16string* type = FindTokenAttribute(token, u"type");
      if (type && base::EqualsCaseInsensitiveASCII(*type, u"hidden")) {
        InsertSelfClosingHTMLElement(token);
        return;
      }
    } else if (name == u"form") {
      if (form_element_ && !IsParsingTemplateContents()) {
        return;
      }
      InsertHTMLFormElement(token);
      Pop();
      return;
    } else if (name == u"template") {
      ProcessTemplateStartTag(token);
      return;
    }
    const base::AutoReset<bool> redirect(&redirect_to_foster_parent_, true);
    ProcessStartTagForInBody(token);
  }

  // End tags.

  void ProcessEndTag(const HtmlToken& token) {
    const std::u16string& name = token.name;
    switch (mode_) {
      case Mode::kInitial:
        DefaultForInitial();
        [[fallthrough]];
      case Mode::kBeforeHtml:
        if (!IsOneOf(name, {u"head", u"body", u"html", u"br"})) {
          return;
        }
        DefaultForBeforeHtml();
        [[fallthrough]];
      case Mode::kBeforeHead:
        if (!IsOneOf(name, {u"head", u"body", u"html", u"br"})) {
          return;
        }
        DefaultForBeforeHead();
        [[fallthrough]];
      case Mode::kInHead:
        if (name == u"template") {
          ProcessTemplateEndTag();
          return;
        }
        if (name == u"head") {
          Pop();
          mode_ = Mode::kAfterHead;
          return;
        }
        if (!IsOneOf(name, {u"body", u"html", u"br"})) {
          return;
        }
        DefaultForInHead();
        [[fallthrough]];
      case Mode::kAfterHead:
        if (!IsOneOf(name, {u"body", u"html", u"br"})) {
          return;
        }
        DefaultForAfterHead();
        [[fallthrough]];
      case Mode::kInBody:
        ProcessEndTagForInBody(token);
        return;
      case Mode::kInTable:
        ProcessEndTagForInTable(token);
        return;
      case Mode::kInCaption:
        if (name == u"caption") {
          ProcessCaptionEndTagForInCaption();
          return;
        }
        if (name == u"table") {
          if (!ProcessCaptionEndTagForInCaption()) {
            return;
          }
          ProcessEndTag(token);
          return;
        }
        if (IsOneOf(name, {u"body", u"col", u"colgroup", u"html", u"tbody",
                           u"tfoot", u"thead", u"td", u"th", u"tr"})) {
          return;
        }
        ProcessEndTagForInBody(token);
        return;
      case Mode::kInColumnGroup:
        if (name == u"colgroup") {
          ProcessColgroupEndTagForInColumnGroup();
          return;
        }
        if (name == u"col") {
          return;
        }
        if (name == u"template") {
          ProcessTemplateEndTag();
          return;
        }
        if (!ProcessColgroupEndTagForInColumnGroup()) {
          return;
        }
        ProcessEndTag(token);
        return;
      case Mode::kInRow:
        ProcessEndTagForInRow(token);
        return;
      case Mode::kInCell:
        ProcessEndTagForInCell(token);
        return;
      case Mode::kInTableBody:
        ProcessEndTagForInTableBody(token);
        return;
      case Mode::kAfterBody:
        if (name == u"html") {
          if (!IsFragment()) {
            mode_ = Mode::kAfterAfterBody;
          }
          return;
        }
        [[fallthrough]];
      case Mode::kAfterAfterBody:
        mode_ = Mode::kInBody;
        ProcessEndTag(token);
        return;
      case Mode::kText:
        if (name == u"script" && CurrentNodeIs(u"script")) {
          ended_script_ = static_cast<HTMLScriptElement*>(&CurrentNode());
          Pop();
          mode_ = original_mode_;
          tokenizer_->SetState(HtmlTokenizer::State::kData);
          return;
        }
        Pop();
        mode_ = original_mode_;
        return;
      case Mode::kInFrameset:
        if (name == u"frameset") {
          if (CurrentIsRootNode() || IsParsingTemplateContents()) {
            return;
          }
          Pop();
          if (!IsFragment() && !CurrentNodeIs(u"frameset")) {
            mode_ = Mode::kAfterFrameset;
          }
        }
        return;
      case Mode::kAfterFrameset:
        if (name == u"html") {
          mode_ = Mode::kAfterAfterFrameset;
        }
        return;
      case Mode::kAfterAfterFrameset:
        return;
      case Mode::kInTableText:
        DefaultForInTableText();
        ProcessEndTag(token);
        return;
      case Mode::kTemplateContents:
        if (name == u"template") {
          ProcessTemplateEndTag();
        }
        return;
    }
  }

  // Returns false where the standard ignores the end tag.
  bool ProcessTemplateEndTag() {
    if (!IsParsingTemplateContents()) {
      return false;
    }
    GenerateImpliedEndTags();
    PopUntilPopped(u"template");
    ClearFormattingToLastMarker();
    template_modes_.pop_back();
    ResetInsertionModeAppropriately();
    return true;
  }

  bool ProcessBodyEndTagForInBody() {
    if (!InScope({u"body"})) {
      return false;
    }
    mode_ = Mode::kAfterBody;
    return true;
  }

  void ProcessAnyOtherEndTagForInBody(const HtmlToken& token) {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      Element& node = *open_elements_[i - 1].element;
      if (node.HasHTMLTagName(token.name)) {
        const cppgc::Persistent<Element> found = &node;
        GenerateImpliedEndTags(token.name);
        PopUntilPopped(*found);
        return;
      }
      if (IsSpecial(node)) {
        return;
      }
    }
  }

  void ProcessEndTagForInBody(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"body") {
      ProcessBodyEndTagForInBody();
      return;
    }
    if (name == u"html") {
      if (ProcessBodyEndTagForInBody()) {
        ProcessEndTag(token);
      }
      return;
    }
    if (IsOneOf(name, {u"address",    u"article", u"aside",   u"blockquote",
                       u"button",     u"center",  u"details", u"dialog",
                       u"dir",        u"div",     u"dl",      u"fieldset",
                       u"figcaption", u"figure",  u"footer",  u"header",
                       u"hgroup",     u"listing", u"main",    u"menu",
                       u"nav",        u"ol",      u"pre",     u"search",
                       u"section",    u"summary", u"select",  u"ul"})) {
      if (!InScope({name})) {
        return;
      }
      GenerateImpliedEndTags();
      PopUntilPopped(name);
      return;
    }
    if (name == u"form") {
      // Blink's feature CorrectTemplateFormParsing is not on for stable: so
      // "any other end tag" follows, inside a template's contents and out.
      if (!IsParsingTemplateContents()) {
        const cppgc::Persistent<Element> form = form_element_.get();
        form_element_ = nullptr;
        if (!form || !InScope(*form)) {
          return;
        }
        GenerateImpliedEndTags();
        RemoveFromStack(form.Get());
      }
      ProcessAnyOtherEndTagForInBody(token);
      return;
    }
    if (name == u"p") {
      if (!InScope({u"p"}, Scope::kButton)) {
        HtmlToken p = StartTag(u"p");
        ProcessStartTag(p);
        ProcessEndTag(token);
        return;
      }
      GenerateImpliedEndTags(u"p");
      PopUntilPopped(u"p");
      return;
    }
    if (name == u"li") {
      if (!InScope({u"li"}, Scope::kListItem)) {
        return;
      }
      GenerateImpliedEndTags(u"li");
      PopUntilPopped(u"li");
      return;
    }
    if (name == u"dd" || name == u"dt") {
      if (!InScope({name})) {
        return;
      }
      GenerateImpliedEndTags(name);
      PopUntilPopped(name);
      return;
    }
    if (IsOneOf(name, kHeadingNames)) {
      if (!InScope(kHeadingNames)) {
        return;
      }
      GenerateImpliedEndTags();
      PopUntilNumberedHeaderPopped();
      return;
    }
    if (IsOneOf(name, kFormattingNames)) {
      CallTheAdoptionAgency(token);
      return;
    }
    if (IsOneOf(name, {u"applet", u"marquee", u"object"})) {
      if (!InScope({name})) {
        return;
      }
      GenerateImpliedEndTags();
      PopUntilPopped(name);
      ClearFormattingToLastMarker();
      return;
    }
    if (name == u"br") {
      HtmlToken br = StartTag(u"br");
      ProcessStartTag(br);
      return;
    }
    if (name == u"template") {
      ProcessTemplateEndTag();
      return;
    }
    ProcessAnyOtherEndTagForInBody(token);
  }

  // The adoption agency algorithm, with the steps that Blink's feature
  // HTMLAdoptionAlgorithmNewSteps, on for stable, takes.
  void CallTheAdoptionAgency(const HtmlToken& token) {
    constexpr int kOuterIterationLimit = 8;
    constexpr int kInnerIterationLimit = 3;

    // Blink matches the current node's local name in any namespace.
    if (CurrentNode().LocalName() == token.name &&
        !FormattingContains(&CurrentNode())) {
      Pop();
      return;
    }

    for (int i = 0; i < kOuterIterationLimit; ++i) {
      const cppgc::Persistent<Element> formatting_element =
          FormattingElementAfterLastMarker(token.name);
      if (!formatting_element) {
        ProcessAnyOtherEndTagForInBody(token);
        return;
      }
      const std::optional<size_t> formatting_index =
          StackIndexOf(formatting_element.Get());
      if (!formatting_index) {
        RemoveFromFormatting(formatting_element.Get());
        return;
      }
      if (!InScope(*formatting_element)) {
        return;
      }
      // The furthest block: the lowest special element above the formatting
      // element.
      cppgc::Persistent<Element> furthest_block;
      for (size_t index = open_elements_.size() - 1; index > *formatting_index;
           --index) {
        if (IsSpecial(*open_elements_[index].element)) {
          furthest_block = open_elements_[index].element.get();
        }
      }
      if (!furthest_block) {
        PopUntilPopped(*formatting_element);
        RemoveFromFormatting(formatting_element.Get());
        return;
      }
      const cppgc::Persistent<Element> common_ancestor =
          open_elements_[*formatting_index - 1].element.get();
      // The bookmark: the element after which the new formatting element goes
      // in the list, or the formatting element itself while it has not moved.
      cppgc::Persistent<Element> bookmark = formatting_element;
      bool bookmark_moved = false;

      cppgc::Persistent<Element> node = furthest_block;
      cppgc::Persistent<Element> next_node =
          open_elements_[*StackIndexOf(node.Get()) - 1].element.get();
      cppgc::Persistent<Element> last_node = furthest_block;
      int inner_loop_counter = 0;
      while (true) {
        ++inner_loop_counter;
        node = next_node;
        next_node = open_elements_[*StackIndexOf(node.Get()) - 1].element.get();
        if (node == formatting_element) {
          break;
        }
        bool in_formatting_list = FormattingContains(node.Get());
        if (inner_loop_counter > kInnerIterationLimit && in_formatting_list) {
          RemoveFromFormatting(node.Get());
          in_formatting_list = false;
        }
        if (!in_formatting_list) {
          RemoveFromStack(node.Get());
          continue;
        }
        const size_t entry_index = *FormattingIndexOf(node.Get());
        Element* const new_element =
            CreateElement(formatting_elements_[entry_index].token,
                          html_names::kXhtmlNamespace);
        formatting_elements_[entry_index].element = new_element;
        open_elements_[*StackIndexOf(node.Get())].element = new_element;
        node = new_element;
        if (last_node == furthest_block) {
          bookmark = node;
          bookmark_moved = true;
        }
        Reparent(*node, *last_node);
        last_node = node;
      }
      InsertAlreadyParsedChild(*common_ancestor, *last_node);
      if (!StackContains(formatting_element.Get()) ||
          !StackContains(furthest_block.Get())) {
        return;
      }
      const std::optional<size_t> entry_index =
          FormattingIndexOf(formatting_element.Get());
      CHECK(entry_index);
      const HtmlToken formatting_token =
          formatting_elements_[*entry_index].token;
      Element* const new_element =
          CreateElement(formatting_token, html_names::kXhtmlNamespace);
      // Blink's SwapTo().
      if (!bookmark_moved) {
        formatting_elements_[*entry_index].element = new_element;
      } else {
        if (const std::optional<size_t> mark =
                FormattingIndexOf(bookmark.Get())) {
          formatting_elements_.insert(
              formatting_elements_.begin() + static_cast<ptrdiff_t>(*mark) + 1,
              {new_element, formatting_token});
        }
        RemoveFromFormatting(formatting_element.Get());
      }
      RemoveFromStack(formatting_element.Get());
      const size_t furthest_index = *StackIndexOf(furthest_block.Get());
      open_elements_.insert(
          open_elements_.begin() + static_cast<ptrdiff_t>(furthest_index) + 1,
          {new_element, false});
      TakeAllChildren(*new_element, *furthest_block);
      Reparent(*furthest_block, *new_element);
    }
  }

  void ResetInsertionModeAppropriately() {
    for (size_t i = open_elements_.size(); i > 0; --i) {
      const bool last = i == 1;
      const Element& node = (last && IsFragment())
                                ? *fragment_context_
                                : *open_elements_[i - 1].element;
      if (node.IsHTMLElement()) {
        const std::u16string& name = node.LocalName();
        if ((name == u"td" || name == u"th") && !last) {
          mode_ = Mode::kInCell;
          return;
        }
        if (name == u"tr") {
          mode_ = Mode::kInRow;
          return;
        }
        if (IsOneOf(name, kTableBodyNames)) {
          mode_ = Mode::kInTableBody;
          return;
        }
        if (name == u"caption") {
          mode_ = Mode::kInCaption;
          return;
        }
        if (name == u"colgroup") {
          mode_ = Mode::kInColumnGroup;
          return;
        }
        if (name == u"table") {
          mode_ = Mode::kInTable;
          return;
        }
        if (name == u"template") {
          CHECK(!template_modes_.empty());
          mode_ = template_modes_.back();
          return;
        }
        if (name == u"head" && !last) {
          mode_ = Mode::kInHead;
          return;
        }
        if (name == u"body") {
          mode_ = Mode::kInBody;
          return;
        }
        if (name == u"frameset") {
          mode_ = Mode::kInFrameset;
          return;
        }
        if (name == u"html") {
          mode_ = head_element_ ? Mode::kAfterHead : Mode::kBeforeHead;
          return;
        }
      }
      if (last) {
        mode_ = Mode::kInBody;
        return;
      }
    }
  }

  void ProcessEndTagForInTable(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"table") {
      ProcessTableEndTagForInTable();
      return;
    }
    if (IsOneOf(name, {u"body", u"caption", u"col", u"colgroup", u"html",
                       u"tbody", u"tfoot", u"thead", u"td", u"th", u"tr"})) {
      return;
    }
    const base::AutoReset<bool> redirect(&redirect_to_foster_parent_, true);
    ProcessEndTagForInBody(token);
  }

  void ProcessEndTagForInTableBody(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (IsOneOf(name, kTableBodyNames)) {
      if (!InTableScope({name})) {
        return;
      }
      PopUntilTableBodyScopeMarker();
      Pop();
      mode_ = Mode::kInTable;
      return;
    }
    if (name == u"table") {
      if (!InTableScope(kTableBodyNames)) {
        return;
      }
      PopUntilTableBodyScopeMarker();
      ProcessEndTag(EndTag(CurrentNode().LocalName()));
      ProcessEndTag(token);
      return;
    }
    if (IsOneOf(name, {u"body", u"caption", u"col", u"colgroup", u"html", u"td",
                       u"th", u"tr"})) {
      return;
    }
    ProcessEndTagForInTable(token);
  }

  void ProcessEndTagForInRow(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"tr") {
      ProcessTrEndTagForInRow();
      return;
    }
    if (name == u"table") {
      if (!ProcessTrEndTagForInRow()) {
        return;
      }
      ProcessEndTag(token);
      return;
    }
    if (IsOneOf(name, kTableBodyNames)) {
      if (!InTableScope({name})) {
        return;
      }
      ProcessEndTag(EndTag(u"tr"));
      ProcessEndTag(token);
      return;
    }
    if (IsOneOf(name, {u"body", u"caption", u"col", u"colgroup", u"html", u"td",
                       u"th"})) {
      return;
    }
    ProcessEndTagForInTable(token);
  }

  void ProcessEndTagForInCell(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"td" || name == u"th") {
      if (!InTableScope({name})) {
        return;
      }
      GenerateImpliedEndTags();
      PopUntilPopped(name);
      ClearFormattingToLastMarker();
      mode_ = Mode::kInRow;
      return;
    }
    if (IsOneOf(name, {u"body", u"caption", u"col", u"colgroup", u"html"})) {
      return;
    }
    if (IsOneOf(name, {u"table", u"tr", u"tbody", u"tfoot", u"thead"})) {
      if (!InTableScope({name})) {
        return;
      }
      CloseTheCell();
      ProcessEndTag(token);
      return;
    }
    ProcessEndTagForInBody(token);
  }

  bool ProcessCaptionEndTagForInCaption() {
    if (!InTableScope({u"caption"})) {
      return false;
    }
    GenerateImpliedEndTags();
    PopUntilPopped(u"caption");
    ClearFormattingToLastMarker();
    mode_ = Mode::kInTable;
    return true;
  }

  bool ProcessTrEndTagForInRow() {
    if (!InTableScope({u"tr"})) {
      return false;
    }
    PopUntilTableRowScopeMarker();
    Pop();
    mode_ = Mode::kInTableBody;
    return true;
  }

  bool ProcessTableEndTagForInTable() {
    if (!InTableScope({u"table"})) {
      return false;
    }
    PopUntilPopped(u"table");
    ResetInsertionModeAppropriately();
    return true;
  }

  bool ProcessColgroupEndTagForInColumnGroup() {
    if (CurrentIsRootNode() || CurrentNodeIs(u"template")) {
      return false;
    }
    Pop();
    mode_ = Mode::kInTable;
    return true;
  }

  void CloseTheCell() {
    ProcessEndTag(EndTag(InTableScope({u"td"}) ? u"td" : u"th"));
  }

  // The end of the input.

  bool ProcessEndOfFileForInTemplateContents() {
    if (!ProcessTemplateEndTag()) {
      return false;
    }
    ProcessEndOfFile();
    return true;
  }

  void ProcessEndOfFile() {
    switch (mode_) {
      case Mode::kInitial:
        DefaultForInitial();
        [[fallthrough]];
      case Mode::kBeforeHtml:
        DefaultForBeforeHtml();
        [[fallthrough]];
      case Mode::kBeforeHead:
        DefaultForBeforeHead();
        [[fallthrough]];
      case Mode::kInHead:
        DefaultForInHead();
        [[fallthrough]];
      case Mode::kAfterHead:
        DefaultForAfterHead();
        [[fallthrough]];
      case Mode::kInBody:
      case Mode::kInCell:
      case Mode::kInCaption:
      case Mode::kInRow:
        if (!template_modes_.empty() &&
            ProcessEndOfFileForInTemplateContents()) {
          return;
        }
        break;
      case Mode::kAfterBody:
      case Mode::kAfterAfterBody:
      case Mode::kAfterFrameset:
      case Mode::kAfterAfterFrameset:
        break;
      case Mode::kInColumnGroup:
        if (CurrentIsRootNode()) {
          return;
        }
        ProcessColgroupEndTagForInColumnGroup();
        [[fallthrough]];
      case Mode::kInFrameset:
      case Mode::kInTable:
      case Mode::kInTableBody:
        if (!template_modes_.empty() &&
            ProcessEndOfFileForInTemplateContents()) {
          return;
        }
        break;
      case Mode::kInTableText:
        DefaultForInTableText();
        ProcessEndOfFile();
        return;
      case Mode::kText:
        Pop();
        mode_ = original_mode_;
        ProcessEndOfFile();
        return;
      case Mode::kTemplateContents:
        if (ProcessEndOfFileForInTemplateContents()) {
          return;
        }
        break;
    }
    // The standard's "stop parsing".
    PopAll();
  }

  // Foreign content.

  bool ShouldProcessTokenInForeignContent(const HtmlToken& token) const {
    if (open_elements_.empty()) {
      return false;
    }
    const OpenElement& adjusted = AdjustedCurrentEntry();
    const Element& node = *adjusted.element;
    if (node.IsHTMLElement()) {
      return false;
    }
    const bool start_tag = token.type == HtmlToken::Type::kStartTag;
    const bool character = token.type == HtmlToken::Type::kCharacter;
    if (IsMathMlTextIntegrationPoint(node)) {
      if (start_tag && token.name != u"mglyph" && token.name != u"malignmark") {
        return false;
      }
      if (character) {
        return false;
      }
    }
    if (IsMathMlElementNamed(node, {u"annotation-xml"}) && start_tag &&
        token.name == u"svg") {
      return false;
    }
    if (IsHtmlIntegrationPoint(adjusted) && (start_tag || character)) {
      return false;
    }
    return token.type != HtmlToken::Type::kEndOfFile;
  }

  static bool IsForeignContentBreakout(const HtmlToken& token) {
    if (token.name == u"font") {
      return FindTokenAttribute(token, u"color") ||
             FindTokenAttribute(token, u"face") ||
             FindTokenAttribute(token, u"size");
    }
    return IsOneOf(token.name, kForeignContentBreakoutNames);
  }

  void ProcessTokenInForeignContent(HtmlToken& token) {
    switch (token.type) {
      case HtmlToken::Type::kCharacter:
        InsertText(token.data);
        if (frameset_ok_ && !IsAllHtmlSpaceOrReplacement(token.data)) {
          frameset_ok_ = false;
        }
        return;
      case HtmlToken::Type::kDoctype:
        return;
      case HtmlToken::Type::kProcessingInstruction:
        ProcessProcessingInstruction();
        return;
      case HtmlToken::Type::kComment:
        InsertComment(token.data);
        return;
      case HtmlToken::Type::kStartTag: {
        // Blink pops back to HTML content in a fragment too, where the
        // standard would insert the element in the context's namespace.
        if (IsForeignContentBreakout(token)) {
          PopUntilForeignContentScopeMarker();
          ProcessStartTag(token);
          return;
        }
        const Element& adjusted = *AdjustedCurrentEntry().element;
        const char16_t* const namespace_uri =
            adjusted.NamespaceURI() == html_names::kMathMlNamespace
                ? html_names::kMathMlNamespace
                : html_names::kSvgNamespace;
        if (namespace_uri == html_names::kMathMlNamespace) {
          AdjustMathMlAttributes(token);
        } else {
          AdjustName(token.name, kSvgTagNames);
          AdjustSvgAttributes(token);
        }
        if (token.name == u"script" && token.self_closing &&
            namespace_uri == html_names::kSvgNamespace) {
          token.self_closing = false;
          InsertForeignElement(token, namespace_uri);
          HtmlToken end_script = EndTag(u"script");
          ProcessTokenInForeignContent(end_script);
          return;
        }
        InsertForeignElement(token, namespace_uri);
        return;
      }
      case HtmlToken::Type::kEndTag: {
        if (AdjustedCurrentEntry().element->NamespaceURI() ==
            html_names::kSvgNamespace) {
          AdjustName(token.name, kSvgTagNames);
        }
        if (token.name == u"script" &&
            IsSvgElementNamed(CurrentNode(), {u"script"})) {
          // Blink runs the script here. SVG's script element has no class
          // yet, so it does not run.
          Pop();
          return;
        }
        if (token.name == u"br" || token.name == u"p") {
          PopUntilForeignContentScopeMarker();
          ProcessEndTag(token);
          return;
        }
        if (!CurrentNode().IsHTMLElement()) {
          for (size_t i = open_elements_.size(); i > 0; --i) {
            Element& node = *open_elements_[i - 1].element;
            if (node.LocalName() == token.name) {
              const cppgc::Persistent<Element> found = &node;
              PopUntilPopped(*found);
              return;
            }
            if (i >= 2 && open_elements_[i - 2].element->IsHTMLElement()) {
              break;
            }
          }
        }
        ProcessEndTag(token);
        return;
      }
      case HtmlToken::Type::kEndOfFile:
      case HtmlToken::Type::kUninitialized:
        NOTREACHED();
    }
  }

  // What keeps the document and the elements below alive, which the
  // builder's owner traces.
  const raw_ref<HtmlTreeBuilderReferences> references_;
  const raw_ptr<Document> document_;
  const raw_ref<HtmlTokenizer> tokenizer_;
  const raw_ptr<Element> fragment_context_;
  // The context element as the adjusted current node of a fragment takes it.
  const OpenElement context_entry_;
  raw_ptr<Element> root_;
  Mode mode_ = Mode::kInitial;
  Mode original_mode_ = Mode::kInitial;
  std::vector<Mode> template_modes_;
  std::vector<OpenElement> open_elements_;
  std::vector<FormattingEntry> formatting_elements_;
  raw_ptr<Element> head_element_;
  raw_ptr<Element> form_element_;
  std::u16string pending_table_characters_;
  bool frameset_ok_ = true;
  bool skip_leading_newline_ = false;
  bool redirect_to_foster_parent_ = false;
  raw_ptr<HTMLScriptElement> ended_script_;
  std::vector<std::string> skipped_rules_;
};

namespace {

HtmlTokenizer::Options TokenizerOptions() {
  return HtmlTokenizer::Options();
}

}  // namespace

HtmlTreeBuilderReferences::HtmlTreeBuilderReferences(Document& document)
    : document_(&document) {}

HtmlTreeBuilderReferences::~HtmlTreeBuilderReferences() = default;

void HtmlTreeBuilderReferences::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(document_);
  elements_.Trace(visitor);
}

void HtmlTreeBuilderReferences::Keep(Element* element) {
  elements_.push_back(element);
}

void HtmlTreeBuilderReferences::KeepOnly(
    const std::vector<Element*>& elements) {
  // In place, with nothing allocated on the C++ heap, so that no collection
  // runs while an element that the builder still holds is out of the list.
  const base::flat_set<const Element*> kept(elements.begin(), elements.end());
  elements_.EraseIf(
      [&kept](Element* element) { return !kept.contains(element); });
}

HtmlDocumentParser::HtmlDocumentParser(Document& document,
                                       HtmlTreeBuilderReferences* references)
    : own_references_(
          references
              ? nullptr
              : bindings::MakeGarbageCollected<HtmlTreeBuilderReferences>(
                    document)),
      tokenizer_(std::make_unique<HtmlTokenizer>(TokenizerOptions())),
      tree_builder_(std::make_unique<HtmlTreeBuilder>(
          references ? *references : *own_references_,
          *tokenizer_)) {
  CHECK(!document.HasChildren());
}

HtmlDocumentParser::~HtmlDocumentParser() = default;

void HtmlDocumentParser::Append(std::u16string_view markup) {
  tokenizer_->Append(markup);
}

void HtmlDocumentParser::Finish() {
  tokenizer_->Finish();
}

bool HtmlDocumentParser::HasSeenEndOfFile() const {
  return tokenizer_->HasSeenEndOfFile();
}

void HtmlDocumentParser::PushInsertionPoint() {
  tokenizer_->PushInsertionPoint();
}

void HtmlDocumentParser::PopInsertionPoint() {
  tokenizer_->PopInsertionPoint();
}

bool HtmlDocumentParser::HasInsertionPoint() const {
  return tokenizer_->HasInsertionPoint();
}

void HtmlDocumentParser::Insert(std::u16string_view markup) {
  tokenizer_->Insert(markup);
}

HTMLScriptElement* HtmlDocumentParser::Pump() {
  HtmlToken token;
  while (tokenizer_->NextToken(token)) {
    if (HTMLScriptElement* const script = tree_builder_->ProcessToken(token)) {
      return script;
    }
  }
  return nullptr;
}

const std::vector<std::string>& HtmlDocumentParser::skipped_rules() const {
  return tree_builder_->skipped_rules();
}

std::vector<std::string> ParseHtmlDocument(Document& document,
                                           std::u16string_view markup) {
  HtmlDocumentParser parser(document);
  parser.Append(markup);
  parser.Finish();
  while (parser.Pump()) {
  }
  return parser.skipped_rules();
}

NodeVector ParseHtmlFragment(Element& context,
                             std::u16string_view markup,
                             std::vector<std::string>* skipped_rules) {
  HtmlTokenizer tokenizer(TokenizerOptions());
  const cppgc::Persistent<HtmlTreeBuilderReferences> references =
      bindings::MakeGarbageCollected<HtmlTreeBuilderReferences>(
          context.GetDocument());
  HtmlTreeBuilder tree_builder(*references, context, tokenizer);
  tokenizer.Append(markup);
  tokenizer.Finish();
  HtmlToken token;
  while (tokenizer.NextToken(token)) {
    tree_builder.ProcessToken(token);
  }
  if (skipped_rules) {
    *skipped_rules = tree_builder.skipped_rules();
  }
  Element* const root = tree_builder.fragment_root();
  NodeVector nodes;
  while (Node* const child = root->FirstChild()) {
    nodes.emplace_back(child);
    root->ParserRemoveChild(*child);
  }
  return nodes;
}

}  // namespace mitto
