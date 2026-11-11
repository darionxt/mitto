#include "mitto/dom/html_tree_builder.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ref.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_script_element.h"
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

bool IsAsciiWhitespace(char16_t c) {
  return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

// The run of whitespace that `text` starts with.
size_t LeadingWhitespace(std::u16string_view text) {
  size_t length = 0;
  while (length < text.size() && IsAsciiWhitespace(text[length])) {
    ++length;
  }
  return length;
}

bool IsHTMLElementNamed(const Element& element, Names names) {
  return element.IsHTMLElement() && IsOneOf(element.LocalName(), names);
}

constexpr Names kFormattingNames = {
    u"a",    u"b", u"big",   u"code",   u"em",     u"font", u"i",
    u"nobr", u"s", u"small", u"strike", u"strong", u"tt",   u"u"};

// The HTML Standard's special elements of the HTML namespace.
constexpr Names kSpecialNames = {
    u"address",   u"applet",   u"area",     u"article",    u"aside",
    u"base",      u"basefont", u"bgsound",  u"blockquote", u"body",
    u"br",        u"button",   u"caption",  u"center",     u"col",
    u"colgroup",  u"dd",       u"details",  u"dir",        u"div",
    u"dl",        u"dt",       u"embed",    u"fieldset",   u"figcaption",
    u"figure",    u"footer",   u"form",     u"frame",      u"frameset",
    u"h1",        u"h2",       u"h3",       u"h4",         u"h5",
    u"h6",        u"head",     u"header",   u"hgroup",     u"hr",
    u"html",      u"iframe",   u"img",      u"input",      u"keygen",
    u"li",        u"link",     u"listing",  u"main",       u"marquee",
    u"menu",      u"meta",     u"nav",      u"noembed",    u"noframes",
    u"noscript",  u"object",   u"ol",       u"p",          u"param",
    u"plaintext", u"pre",      u"script",   u"search",     u"section",
    u"select",    u"source",   u"style",    u"summary",    u"table",
    u"tbody",     u"td",       u"template", u"textarea",   u"tfoot",
    u"th",        u"thead",    u"title",    u"tr",         u"track",
    u"ul",        u"wbr",      u"xmp"};

constexpr Names kHeadingNames = {u"h1", u"h2", u"h3", u"h4", u"h5", u"h6"};

constexpr Names kTableNames = {u"caption", u"col", u"colgroup", u"table",
                               u"tbody",   u"td",  u"tfoot",    u"th",
                               u"thead",   u"tr"};

// The end tags that the standard's "generate implied end tags" closes.
constexpr Names kImpliedEndTagNames = {u"dd",     u"dt", u"li", u"optgroup",
                                       u"option", u"p",  u"rb", u"rp",
                                       u"rt",     u"rtc"};

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

}  // namespace

class HtmlTreeBuilder {
 public:
  // For a document.
  HtmlTreeBuilder(Document& document, HtmlTokenizer& tokenizer)
      : document_(&document), tokenizer_(tokenizer) {}

  // For a fragment whose context is `context`, as the standard's fragment
  // parsing algorithm sets the builder up.
  HtmlTreeBuilder(Element& context, HtmlTokenizer& tokenizer)
      : document_(&context.GetDocument()),
        tokenizer_(tokenizer),
        fragment_context_(&context) {
    root_ = document_->CreateElement(html_names::kXhtmlNamespace, std::nullopt,
                                     u"html");
    open_elements_.emplace_back(root_.Get());
    if (context.IsHTMLElement()) {
      const std::u16string& name = context.LocalName();
      if (IsOneOf(name, {u"title", u"textarea"})) {
        tokenizer_->SetState(HtmlTokenizer::State::kRcdata);
      } else if (IsOneOf(name, {u"style", u"xmp", u"iframe", u"noembed",
                                u"noframes", u"noscript"})) {
        tokenizer_->SetState(HtmlTokenizer::State::kRawtext);
      } else if (name == u"script") {
        tokenizer_->SetState(HtmlTokenizer::State::kScriptData);
      } else if (name == u"plaintext") {
        tokenizer_->SetState(HtmlTokenizer::State::kPlaintext);
      }
    }
    ResetInsertionModeAppropriately();
    for (Node* node = &context; node; node = node->ParentNode()) {
      if (node->IsElementNode() &&
          static_cast<Element*>(node)->HasHTMLTagName(u"form")) {
        form_element_ = static_cast<Element*>(node);
        break;
      }
    }
  }

  HtmlTreeBuilder(const HtmlTreeBuilder&) = delete;
  HtmlTreeBuilder& operator=(const HtmlTreeBuilder&) = delete;

  // Processes one token. Returns the script element whose end tag it was.
  HTMLScriptElement* ProcessToken(HtmlToken& token) {
    const bool ignore_line_feed = ignore_next_line_feed_;
    ignore_next_line_feed_ = false;
    ended_script_ = nullptr;
    switch (token.type) {
      case HtmlToken::Type::kDoctype:
        ProcessDoctype(token);
        break;
      case HtmlToken::Type::kStartTag:
        ProcessStartTag(token);
        break;
      case HtmlToken::Type::kEndTag:
        ProcessEndTag(token);
        break;
      case HtmlToken::Type::kComment:
        ProcessComment(token.data);
        break;
      case HtmlToken::Type::kCharacter: {
        std::u16string_view text = token.data;
        if (ignore_line_feed && !text.empty() && text[0] == '\n') {
          text.remove_prefix(1);
        }
        ProcessCharacters(text);
        break;
      }
      case HtmlToken::Type::kProcessingInstruction:
        // Blink inserts a ProcessingInstruction node, which has no class here.
        Skip("processing instructions");
        break;
      case HtmlToken::Type::kEndOfFile:
        ProcessEndOfFile();
        break;
      case HtmlToken::Type::kUninitialized:
        NOTREACHED();
    }
    HTMLScriptElement* const script = ended_script_.Get();
    ended_script_ = nullptr;
    return script;
  }

  Element* fragment_root() const { return root_.Get(); }
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
    kAfterBody,
    kAfterAfterBody,
  };

  enum class Scope {
    kDefault,
    kListItem,
    kButton,
  };

  void Skip(const char* rule) {
    if (std::find(skipped_rules_.begin(), skipped_rules_.end(), rule) ==
        skipped_rules_.end()) {
      skipped_rules_.push_back(rule);
    }
  }

  void SetMode(Mode mode) {
    mode_ = mode;
    // Blink replaces U+0000 in the tokenizer in the text insertion mode.
    tokenizer_->SetForceNullCharacterReplacement(mode == Mode::kText);
  }

  bool IsFragment() const { return fragment_context_; }

  // The stack of open elements.

  Element& CurrentNode() const { return *open_elements_.back(); }
  bool CurrentNodeIs(std::u16string_view name) const {
    return !open_elements_.empty() && CurrentNode().HasHTMLTagName(name);
  }

  // Pops the current node. `implied` says it was closed by something other
  // than its own end tag, which for a formatting element means the standard
  // would make it again where it reconstructs the active formatting elements.
  void Pop(bool implied = true) {
    CHECK(!open_elements_.empty());
    if (implied && IsHTMLElementNamed(CurrentNode(), kFormattingNames)) {
      formatting_elements_closed_ = true;
    }
    open_elements_.pop_back();
  }

  void PopUntil(Names names) {
    while (!open_elements_.empty()) {
      const bool found = IsHTMLElementNamed(CurrentNode(), names);
      Pop(!found);
      if (found) {
        return;
      }
    }
  }

  void PopUntilElement(const Element& element) {
    while (!open_elements_.empty()) {
      const bool found = &CurrentNode() == &element;
      Pop(!found);
      if (found) {
        return;
      }
    }
  }

  void RemoveFromStack(const Element& element) {
    const auto found =
        std::find_if(open_elements_.begin(), open_elements_.end(),
                     [&element](const cppgc::Persistent<Element>& open) {
                       return open.Get() == &element;
                     });
    if (found != open_elements_.end()) {
      open_elements_.erase(found);
    }
  }

  bool StackContains(Names names) const {
    return std::any_of(open_elements_.begin(), open_elements_.end(),
                       [names](const cppgc::Persistent<Element>& element) {
                         return IsHTMLElementNamed(*element, names);
                       });
  }

  static bool IsScopeBoundary(const Element& element, Scope scope) {
    if (IsHTMLElementNamed(
            element, {u"applet", u"caption", u"html", u"table", u"td", u"th",
                      u"marquee", u"object", u"template"})) {
      return true;
    }
    switch (scope) {
      case Scope::kDefault:
        return false;
      case Scope::kListItem:
        return IsHTMLElementNamed(element, {u"ol", u"ul"});
      case Scope::kButton:
        return IsHTMLElementNamed(element, {u"button"});
    }
    NOTREACHED();
  }

  bool InScope(Names names, Scope scope = Scope::kDefault) const {
    for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
      if (IsHTMLElementNamed(**it, names)) {
        return true;
      }
      if (IsScopeBoundary(**it, scope)) {
        return false;
      }
    }
    return false;
  }

  bool ElementInScope(const Element& element) const {
    for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
      if (it->Get() == &element) {
        return true;
      }
      if (IsScopeBoundary(**it, Scope::kDefault)) {
        return false;
      }
    }
    return false;
  }

  void GenerateImpliedEndTags(std::u16string_view except = u"") {
    while (!open_elements_.empty() &&
           IsHTMLElementNamed(CurrentNode(), kImpliedEndTagNames) &&
           CurrentNode().LocalName() != except) {
      Pop();
    }
  }

  void ClosePElement() {
    GenerateImpliedEndTags(u"p");
    PopUntil({u"p"});
  }

  void ClosePIfInButtonScope() {
    if (InScope({u"p"}, Scope::kButton)) {
      ClosePElement();
    }
  }

  // The standard's "reconstruct the active formatting elements", whose list
  // the builder does not keep: it notes the rule when the list would have
  // held an element that something other than its end tag closed.
  void ReconstructActiveFormattingElements() {
    if (formatting_elements_closed_) {
      Skip("the list of active formatting elements");
      formatting_elements_closed_ = false;
    }
  }

  // Making nodes.

  Element* CreateElement(const HtmlToken& token,
                         const char16_t* namespace_uri) {
    Element* const element =
        document_->CreateElement(namespace_uri, std::nullopt, token.name);
    for (const HtmlAttribute& attribute : token.attributes) {
      element->SetAttributeWithoutValidation(attribute.name, attribute.value);
    }
    return element;
  }

  // The standard's "insert a foreign element" with the adjusted insertion
  // location, which is the current node without tables and templates.
  Element* InsertElement(
      const HtmlToken& token,
      const char16_t* namespace_uri = html_names::kXhtmlNamespace) {
    Element* const element = CreateElement(token, namespace_uri);
    CurrentNode().ParserAppendChild(element);
    open_elements_.emplace_back(element);
    return element;
  }

  void InsertVoidElement(const HtmlToken& token) {
    InsertElement(token);
    Pop(/*implied=*/false);
  }

  void InsertCharacters(std::u16string_view text) {
    if (text.empty()) {
      return;
    }
    Element& parent = CurrentNode();
    if (Node* const last = parent.LastChild(); last && last->IsTextNode()) {
      static_cast<Text*>(last)->AppendData(std::u16string(text));
      return;
    }
    parent.ParserAppendChild(Text::Create(*document_, std::u16string(text)));
  }

  void InsertComment(const std::u16string& data, Node& parent) {
    parent.ParserAppendChild(Comment::Create(*document_, data));
  }

  // The standard's generic raw text and RCDATA element parsing algorithms.
  void ParseText(const HtmlToken& token, HtmlTokenizer::State state) {
    InsertElement(token);
    tokenizer_->SetState(state);
    original_mode_ = mode_;
    SetMode(Mode::kText);
  }

  void AddMissingAttributes(Element& element, const HtmlToken& token) {
    for (const HtmlAttribute& attribute : token.attributes) {
      if (!element.FindAttribute(attribute.name)) {
        element.SetAttributeWithoutValidation(attribute.name, attribute.value);
      }
    }
  }

  // The standard's "reset the insertion mode appropriately", for the fragment
  // case, whose stack holds only the root.
  void ResetInsertionModeAppropriately() {
    const Element& node = *fragment_context_;
    if (!node.IsHTMLElement()) {
      SetMode(Mode::kInBody);
      return;
    }
    const std::u16string& name = node.LocalName();
    if (name == u"select") {
      Skip("select");
    } else if (IsOneOf(name, kTableNames)) {
      Skip("tables");
    } else if (name == u"template") {
      Skip("templates");
    } else if (name == u"frameset") {
      Skip("framesets");
    } else if (name == u"html") {
      SetMode(head_element_ ? Mode::kAfterHead : Mode::kBeforeHead);
      return;
    }
    SetMode(Mode::kInBody);
  }

  // Tokens.

  void ProcessDoctype(const HtmlToken& token) {
    if (mode_ != Mode::kInitial) {
      return;  // A parse error, which the standard ignores.
    }
    document_->ParserAppendChild(DocumentType::Create(
        *document_, token.name, token.public_identifier.value_or(u""),
        token.system_identifier.value_or(u"")));
    document_->SetCompatibilityMode(CompatibilityModeFromDoctype(token));
    SetMode(Mode::kBeforeHtml);
  }

  void ProcessComment(const std::u16string& data) {
    switch (mode_) {
      case Mode::kInitial:
      case Mode::kBeforeHtml:
      case Mode::kAfterAfterBody:
        InsertComment(data, *document_);
        return;
      case Mode::kAfterBody:
        InsertComment(data, *open_elements_.front());
        return;
      default:
        InsertComment(data, CurrentNode());
        return;
    }
  }

  void ProcessCharacters(std::u16string_view text) {
    while (!text.empty()) {
      switch (mode_) {
        case Mode::kInitial:
        case Mode::kBeforeHtml:
        case Mode::kBeforeHead: {
          text.remove_prefix(LeadingWhitespace(text));
          if (text.empty()) {
            return;
          }
          AnythingElse();
          continue;
        }
        case Mode::kInHead:
        case Mode::kAfterHead: {
          const size_t whitespace = LeadingWhitespace(text);
          InsertCharacters(text.substr(0, whitespace));
          text.remove_prefix(whitespace);
          if (text.empty()) {
            return;
          }
          AnythingElse();
          continue;
        }
        case Mode::kInBody:
          ProcessCharactersInBody(text);
          return;
        case Mode::kText:
          InsertCharacters(text);
          return;
        case Mode::kAfterBody:
        case Mode::kAfterAfterBody: {
          const size_t whitespace = LeadingWhitespace(text);
          ProcessCharactersInBody(text.substr(0, whitespace));
          text.remove_prefix(whitespace);
          if (text.empty()) {
            return;
          }
          SetMode(Mode::kInBody);
          continue;
        }
      }
    }
  }

  void ProcessCharactersInBody(std::u16string_view text) {
    std::u16string characters;
    for (const char16_t c : text) {
      if (c != 0) {
        characters += c;
      }
    }
    if (characters.empty()) {
      return;
    }
    ReconstructActiveFormattingElements();
    InsertCharacters(characters);
    if (LeadingWhitespace(characters) != characters.size()) {
      frameset_ok_ = false;
    }
  }

  // What each mode does with "anything else" before it reprocesses the token.
  void AnythingElse() {
    switch (mode_) {
      case Mode::kInitial:
        document_->SetCompatibilityMode(Document::CompatibilityMode::kQuirks);
        SetMode(Mode::kBeforeHtml);
        return;
      case Mode::kBeforeHtml:
        InsertHtmlElement(HtmlToken());
        return;
      case Mode::kBeforeHead: {
        HtmlToken head;
        head.name = u"head";
        head_element_ = InsertElement(head);
        SetMode(Mode::kInHead);
        return;
      }
      case Mode::kInHead:
        Pop();
        SetMode(Mode::kAfterHead);
        return;
      case Mode::kAfterHead: {
        HtmlToken body;
        body.name = u"body";
        InsertElement(body);
        SetMode(Mode::kInBody);
        return;
      }
      default:
        NOTREACHED();
    }
  }

  void InsertHtmlElement(const HtmlToken& token) {
    HtmlToken html = token;
    html.name = u"html";
    Element* const element = CreateElement(html, html_names::kXhtmlNamespace);
    document_->ParserAppendChild(element);
    open_elements_.emplace_back(element);
    SetMode(Mode::kBeforeHead);
  }

  void ProcessStartTag(HtmlToken& token) {
    while (true) {
      const std::u16string& name = token.name;
      switch (mode_) {
        case Mode::kInitial:
          AnythingElse();
          continue;
        case Mode::kBeforeHtml:
          if (name == u"html") {
            InsertHtmlElement(token);
            return;
          }
          AnythingElse();
          continue;
        case Mode::kBeforeHead:
          if (name == u"html") {
            ProcessStartTagInBody(token);
            return;
          }
          if (name == u"head") {
            head_element_ = InsertElement(token);
            SetMode(Mode::kInHead);
            return;
          }
          AnythingElse();
          continue;
        case Mode::kInHead:
          if (ProcessStartTagInHead(token)) {
            return;
          }
          AnythingElse();
          continue;
        case Mode::kAfterHead:
          if (name == u"html") {
            ProcessStartTagInBody(token);
            return;
          }
          if (name == u"body") {
            InsertElement(token);
            frameset_ok_ = false;
            SetMode(Mode::kInBody);
            return;
          }
          if (name == u"frameset") {
            Skip("framesets");
            InsertElement(token);
            SetMode(Mode::kInBody);
            return;
          }
          if (IsOneOf(name, {u"base", u"basefont", u"bgsound", u"link", u"meta",
                             u"noframes", u"script", u"style", u"template",
                             u"title"})) {
            // A parse error: the element goes in the head all the same.
            if (!head_element_) {
              AnythingElse();
              continue;
            }
            open_elements_.emplace_back(head_element_.Get());
            const cppgc::Persistent<Element> head = head_element_;
            ProcessStartTagInHead(token);
            RemoveFromStack(*head);
            return;
          }
          if (name == u"head") {
            return;
          }
          AnythingElse();
          continue;
        case Mode::kInBody:
          ProcessStartTagInBody(token);
          return;
        case Mode::kText:
          NOTREACHED();
        case Mode::kAfterBody:
        case Mode::kAfterAfterBody:
          if (name == u"html") {
            ProcessStartTagInBody(token);
            return;
          }
          SetMode(Mode::kInBody);
          continue;
      }
    }
  }

  // The "in head" insertion mode's start tags. Returns false for "anything
  // else".
  bool ProcessStartTagInHead(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"html") {
      ProcessStartTagInBody(token);
      return true;
    }
    if (IsOneOf(name, {u"base", u"basefont", u"bgsound", u"link", u"meta"})) {
      InsertVoidElement(token);
      return true;
    }
    if (name == u"title") {
      ParseText(token, HtmlTokenizer::State::kRcdata);
      return true;
    }
    // A Page runs scripts, so noscript holds raw text.
    if (IsOneOf(name, {u"noscript", u"noframes", u"style"})) {
      ParseText(token, HtmlTokenizer::State::kRawtext);
      return true;
    }
    if (name == u"script") {
      ParseText(token, HtmlTokenizer::State::kScriptData);
      return true;
    }
    if (name == u"template") {
      Skip("templates");
      InsertElement(token);
      return true;
    }
    if (name == u"head") {
      return true;
    }
    return false;
  }

  void ProcessStartTagInBody(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"html") {
      AddMissingAttributes(*open_elements_.front(), token);
      return;
    }
    if (IsOneOf(name,
                {u"base", u"basefont", u"bgsound", u"link", u"meta",
                 u"noframes", u"script", u"style", u"template", u"title"})) {
      ProcessStartTagInHead(token);
      return;
    }
    if (name == u"body") {
      if (open_elements_.size() < 2 ||
          !open_elements_[1]->HasHTMLTagName(u"body")) {
        return;
      }
      frameset_ok_ = false;
      AddMissingAttributes(*open_elements_[1], token);
      return;
    }
    if (name == u"frameset") {
      Skip("framesets");
      return;
    }
    if (IsOneOf(
            name,
            {u"address",  u"article",    u"aside",   u"blockquote", u"center",
             u"details",  u"dialog",     u"dir",     u"div",        u"dl",
             u"fieldset", u"figcaption", u"figure",  u"footer",     u"header",
             u"hgroup",   u"main",       u"menu",    u"nav",        u"ol",
             u"p",        u"search",     u"section", u"summary",    u"ul"})) {
      ClosePIfInButtonScope();
      InsertElement(token);
      return;
    }
    if (IsOneOf(name, kHeadingNames)) {
      ClosePIfInButtonScope();
      if (IsHTMLElementNamed(CurrentNode(), kHeadingNames)) {
        Pop();
      }
      InsertElement(token);
      return;
    }
    if (IsOneOf(name, {u"pre", u"listing"})) {
      ClosePIfInButtonScope();
      InsertElement(token);
      ignore_next_line_feed_ = true;
      frameset_ok_ = false;
      return;
    }
    if (name == u"form") {
      if (form_element_ && !StackContains({u"template"})) {
        return;
      }
      ClosePIfInButtonScope();
      Element* const form = InsertElement(token);
      if (!StackContains({u"template"})) {
        form_element_ = form;
      }
      return;
    }
    if (name == u"li" || name == u"dd" || name == u"dt") {
      frameset_ok_ = false;
      const bool is_li = name == u"li";
      for (auto it = open_elements_.rbegin(); it != open_elements_.rend();
           ++it) {
        const Element& node = **it;
        if (is_li ? IsHTMLElementNamed(node, {u"li"})
                  : IsHTMLElementNamed(node, {u"dd", u"dt"})) {
          const std::u16string closed = node.LocalName();
          GenerateImpliedEndTags(closed);
          PopUntil({closed});
          break;
        }
        if (IsHTMLElementNamed(node, kSpecialNames) &&
            !IsHTMLElementNamed(node, {u"address", u"div", u"p"})) {
          break;
        }
      }
      ClosePIfInButtonScope();
      InsertElement(token);
      return;
    }
    if (name == u"plaintext") {
      ClosePIfInButtonScope();
      InsertElement(token);
      tokenizer_->SetState(HtmlTokenizer::State::kPlaintext);
      return;
    }
    if (name == u"button") {
      if (InScope({u"button"})) {
        GenerateImpliedEndTags();
        PopUntil({u"button"});
      }
      ReconstructActiveFormattingElements();
      InsertElement(token);
      frameset_ok_ = false;
      return;
    }
    if (name == u"a") {
      if (StackContains({u"a"})) {
        Skip("the adoption agency algorithm");
        PopUntil({u"a"});
      }
      ReconstructActiveFormattingElements();
      InsertElement(token);
      return;
    }
    if (name == u"nobr") {
      ReconstructActiveFormattingElements();
      if (InScope({u"nobr"})) {
        Skip("the adoption agency algorithm");
        PopUntil({u"nobr"});
      }
      InsertElement(token);
      return;
    }
    if (IsOneOf(name, kFormattingNames)) {
      ReconstructActiveFormattingElements();
      InsertElement(token);
      return;
    }
    if (IsOneOf(name, {u"applet", u"marquee", u"object"})) {
      ReconstructActiveFormattingElements();
      InsertElement(token);
      frameset_ok_ = false;
      return;
    }
    if (IsOneOf(name, kTableNames)) {
      // Each of these needs the table insertion modes. The builder takes them
      // for ordinary elements meanwhile.
      Skip("tables");
      if (name == u"table" && !document_->InQuirksMode()) {
        ClosePIfInButtonScope();
      }
      if (name == u"col") {
        InsertVoidElement(token);
      } else {
        InsertElement(token);
      }
      frameset_ok_ = false;
      return;
    }
    if (IsOneOf(name, {u"area", u"br", u"embed", u"img", u"keygen", u"wbr",
                       u"image"})) {
      ReconstructActiveFormattingElements();
      HtmlToken element = token;
      if (name == u"image") {
        element.name = u"img";
      }
      InsertVoidElement(element);
      frameset_ok_ = false;
      return;
    }
    if (name == u"input") {
      ReconstructActiveFormattingElements();
      InsertVoidElement(token);
      const auto type =
          std::find_if(token.attributes.begin(), token.attributes.end(),
                       [](const HtmlAttribute& attribute) {
                         return attribute.name == u"type";
                       });
      if (type == token.attributes.end() ||
          !base::EqualsCaseInsensitiveASCII(type->value, u"hidden")) {
        frameset_ok_ = false;
      }
      return;
    }
    if (IsOneOf(name, {u"param", u"source", u"track"})) {
      InsertVoidElement(token);
      return;
    }
    if (name == u"hr") {
      ClosePIfInButtonScope();
      InsertVoidElement(token);
      frameset_ok_ = false;
      return;
    }
    if (name == u"textarea") {
      InsertElement(token);
      ignore_next_line_feed_ = true;
      tokenizer_->SetState(HtmlTokenizer::State::kRcdata);
      original_mode_ = mode_;
      frameset_ok_ = false;
      SetMode(Mode::kText);
      return;
    }
    if (name == u"xmp") {
      ClosePIfInButtonScope();
      ReconstructActiveFormattingElements();
      frameset_ok_ = false;
      ParseText(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    if (name == u"iframe") {
      frameset_ok_ = false;
      ParseText(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    if (name == u"noembed" || name == u"noscript") {
      ParseText(token, HtmlTokenizer::State::kRawtext);
      return;
    }
    if (name == u"select") {
      Skip("select");
      ReconstructActiveFormattingElements();
      InsertElement(token);
      frameset_ok_ = false;
      return;
    }
    if (name == u"optgroup" || name == u"option") {
      if (CurrentNodeIs(u"option")) {
        Pop();
      }
      ReconstructActiveFormattingElements();
      InsertElement(token);
      return;
    }
    if (name == u"rb" || name == u"rtc") {
      if (InScope({u"ruby"})) {
        GenerateImpliedEndTags();
      }
      InsertElement(token);
      return;
    }
    if (name == u"rp" || name == u"rt") {
      if (InScope({u"ruby"})) {
        GenerateImpliedEndTags(u"rtc");
      }
      InsertElement(token);
      return;
    }
    if (name == u"math" || name == u"svg") {
      // Foreign content, whose elements and attributes need adjusting and
      // whose own insertion rules the builder lacks.
      Skip("foreign content");
      ReconstructActiveFormattingElements();
      InsertElement(token, name == u"math" ? html_names::kMathMlNamespace
                                           : html_names::kSvgNamespace);
      if (token.self_closing) {
        Pop(/*implied=*/false);
      }
      return;
    }
    if (name == u"head" || name == u"frame") {
      return;  // A parse error, which the standard ignores.
    }
    ReconstructActiveFormattingElements();
    InsertElement(token);
  }

  void ProcessEndTag(const HtmlToken& token) {
    while (true) {
      const std::u16string& name = token.name;
      switch (mode_) {
        case Mode::kInitial:
          AnythingElse();
          continue;
        case Mode::kBeforeHtml:
        case Mode::kBeforeHead:
          if (!IsOneOf(name, {u"head", u"body", u"html", u"br"})) {
            return;
          }
          AnythingElse();
          continue;
        case Mode::kInHead:
          if (name == u"head") {
            Pop();
            SetMode(Mode::kAfterHead);
            return;
          }
          if (name == u"template") {
            Skip("templates");
            if (StackContains({u"template"})) {
              PopUntil({u"template"});
            }
            return;
          }
          if (!IsOneOf(name, {u"body", u"html", u"br"})) {
            return;
          }
          AnythingElse();
          continue;
        case Mode::kAfterHead:
          if (name == u"template") {
            Skip("templates");
            return;
          }
          if (!IsOneOf(name, {u"body", u"html", u"br"})) {
            return;
          }
          AnythingElse();
          continue;
        case Mode::kInBody:
          if (ProcessEndTagInBody(token)) {
            continue;
          }
          return;
        case Mode::kText:
          if (name == u"script" && CurrentNodeIs(u"script")) {
            ended_script_ = static_cast<HTMLScriptElement*>(&CurrentNode());
          }
          Pop(/*implied=*/false);
          SetMode(original_mode_);
          return;
        case Mode::kAfterBody:
          if (name == u"html") {
            if (!IsFragment()) {
              SetMode(Mode::kAfterAfterBody);
            }
            return;
          }
          SetMode(Mode::kInBody);
          continue;
        case Mode::kAfterAfterBody:
          SetMode(Mode::kInBody);
          continue;
      }
    }
  }

  // The "in body" insertion mode's end tags. Returns true to reprocess the
  // token in the mode it switched to.
  bool ProcessEndTagInBody(const HtmlToken& token) {
    const std::u16string& name = token.name;
    if (name == u"template") {
      Skip("templates");
      if (StackContains({u"template"})) {
        PopUntil({u"template"});
      }
      return false;
    }
    if (name == u"body" || name == u"html") {
      if (!InScope({u"body"})) {
        return false;
      }
      SetMode(Mode::kAfterBody);
      return name == u"html";
    }
    if (IsOneOf(name, {u"address",    u"article", u"aside",   u"blockquote",
                       u"button",     u"center",  u"details", u"dialog",
                       u"dir",        u"div",     u"dl",      u"fieldset",
                       u"figcaption", u"figure",  u"footer",  u"header",
                       u"hgroup",     u"listing", u"main",    u"menu",
                       u"nav",        u"ol",      u"pre",     u"search",
                       u"section",    u"summary", u"ul"})) {
      if (!InScope({name})) {
        return false;
      }
      GenerateImpliedEndTags();
      PopUntil({name});
      return false;
    }
    if (name == u"form") {
      if (StackContains({u"template"})) {
        Skip("templates");
      }
      const cppgc::Persistent<Element> form = form_element_;
      form_element_ = nullptr;
      if (!form || !ElementInScope(*form)) {
        return false;
      }
      GenerateImpliedEndTags();
      RemoveFromStack(*form);
      return false;
    }
    if (name == u"p") {
      if (!InScope({u"p"}, Scope::kButton)) {
        HtmlToken p;
        p.name = u"p";
        InsertElement(p);
      }
      ClosePElement();
      return false;
    }
    if (name == u"li") {
      if (!InScope({u"li"}, Scope::kListItem)) {
        return false;
      }
      GenerateImpliedEndTags(u"li");
      PopUntil({u"li"});
      return false;
    }
    if (name == u"dd" || name == u"dt") {
      if (!InScope({name})) {
        return false;
      }
      GenerateImpliedEndTags(name);
      PopUntil({name});
      return false;
    }
    if (IsOneOf(name, kHeadingNames)) {
      if (!InScope(kHeadingNames)) {
        return false;
      }
      GenerateImpliedEndTags();
      PopUntil(kHeadingNames);
      return false;
    }
    if (IsOneOf(name, kFormattingNames)) {
      ProcessFormattingEndTag(name);
      return false;
    }
    if (IsOneOf(name, {u"applet", u"marquee", u"object"})) {
      if (!InScope({name})) {
        return false;
      }
      GenerateImpliedEndTags();
      PopUntil({name});
      return false;
    }
    if (name == u"br") {
      HtmlToken br;
      br.type = HtmlToken::Type::kStartTag;
      br.name = u"br";
      ReconstructActiveFormattingElements();
      InsertVoidElement(br);
      frameset_ok_ = false;
      return false;
    }
    if (IsOneOf(name, kTableNames) || name == u"select") {
      // As the builder took them for ordinary elements.
      Skip(name == u"select" ? "select" : "tables");
      if (StackContains({name})) {
        PopUntil({name});
      }
      return false;
    }
    if (name == u"svg" || name == u"math") {
      Skip("foreign content");
    }
    // Any other end tag.
    for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
      Element& node = **it;
      if (node.LocalName() == name &&
          (node.IsHTMLElement() || name == u"svg" || name == u"math")) {
        GenerateImpliedEndTags(name);
        PopUntilElement(node);
        return false;
      }
      if (IsHTMLElementNamed(node, kSpecialNames)) {
        return false;
      }
    }
    return false;
  }

  // The adoption agency algorithm, as far as it goes without reparenting: an
  // end tag that closes its element where it is.
  void ProcessFormattingEndTag(const std::u16string& name) {
    if (CurrentNodeIs(name)) {
      Pop(/*implied=*/false);
      return;
    }
    Element* formatting = nullptr;
    bool special_above = false;
    for (auto it = open_elements_.rbegin(); it != open_elements_.rend(); ++it) {
      if ((*it)->HasHTMLTagName(name)) {
        formatting = it->Get();
        break;
      }
      if (IsHTMLElementNamed(**it, kSpecialNames)) {
        special_above = true;
      }
    }
    if (!formatting || !ElementInScope(*formatting)) {
      return;
    }
    if (special_above) {
      // The furthest block: the algorithm moves nodes, which the builder
      // does not do.
      Skip("the adoption agency algorithm");
    }
    PopUntilElement(*formatting);
  }

  void ProcessEndOfFile() {
    while (true) {
      switch (mode_) {
        case Mode::kInitial:
        case Mode::kBeforeHtml:
        case Mode::kBeforeHead:
        case Mode::kInHead:
        case Mode::kAfterHead:
          AnythingElse();
          continue;
        case Mode::kText:
          Pop();
          SetMode(original_mode_);
          continue;
        case Mode::kInBody:
        case Mode::kAfterBody:
        case Mode::kAfterAfterBody:
          // The standard's "stop parsing".
          open_elements_.clear();
          return;
      }
    }
  }

  const cppgc::Persistent<Document> document_;
  const raw_ref<HtmlTokenizer> tokenizer_;
  const cppgc::Persistent<Element> fragment_context_;
  cppgc::Persistent<Element> root_;
  Mode mode_ = Mode::kInitial;
  Mode original_mode_ = Mode::kInitial;
  std::vector<cppgc::Persistent<Element>> open_elements_;
  cppgc::Persistent<Element> head_element_;
  cppgc::Persistent<Element> form_element_;
  bool frameset_ok_ = true;
  bool ignore_next_line_feed_ = false;
  bool formatting_elements_closed_ = false;
  cppgc::Persistent<HTMLScriptElement> ended_script_;
  std::vector<std::string> skipped_rules_;
};

namespace {

HtmlTokenizer::Options TokenizerOptions() {
  return HtmlTokenizer::Options();
}

}  // namespace

HtmlDocumentParser::HtmlDocumentParser(Document& document)
    : tokenizer_(std::make_unique<HtmlTokenizer>(TokenizerOptions())),
      tree_builder_(std::make_unique<HtmlTreeBuilder>(document, *tokenizer_)) {
  CHECK(!document.HasChildren());
}

HtmlDocumentParser::~HtmlDocumentParser() = default;

void HtmlDocumentParser::Append(std::u16string_view markup) {
  tokenizer_->Append(markup);
}

void HtmlDocumentParser::Finish() {
  tokenizer_->Finish();
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
  HtmlTreeBuilder tree_builder(context, tokenizer);
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
