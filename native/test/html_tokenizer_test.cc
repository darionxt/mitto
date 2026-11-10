// Runs html5lib's tokenizer tests against the HTML tokenizer, and holds what
// fails to the list of failures that is kept with their reasons:
//
//   html_tokenizer_test [--print-failures]
//       [--without-processing-instructions] TESTS FAILURES
//
// TESTS is a directory of html5lib's .test files, and FAILURES the list,
// native/test/html5lib_tokenizer_failures.txt. Each test runs once for each
// of its initial states, with its whole input in one piece and again one code
// unit at a time, and the two must give the same tokens and parse errors. It
// exits with an error if a test that the list names passes, or one that it
// does not name fails. --print-failures prints every test that fails in the
// form of the list, without its reasons. --without-processing-instructions
// turns off Blink's processing instructions, which shows the tests that the
// list says fail for them pass without them.

#include "mitto/html/html_tokenizer.h"

#include <stddef.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "mitto/html/html_parse_error.h"
#include "mitto/html/html_token.h"

namespace mitto {
namespace {

using State = HtmlTokenizer::State;

struct InitialState {
  std::string_view name;
  State state;
};

constexpr auto kInitialStates = std::to_array<InitialState>({
    {"Data state", State::kData},
    {"PLAINTEXT state", State::kPlaintext},
    {"RCDATA state", State::kRcdata},
    {"RAWTEXT state", State::kRawtext},
    {"Script data state", State::kScriptData},
    {"CDATA section state", State::kCdataSection},
});

// One test in one initial state.
struct Run {
  std::string id;
  std::string description;
  std::u16string input;
  State state;
  std::optional<std::u16string> last_start_tag;
  std::string expected_tokens;
  std::string expected_errors;
};

struct Result {
  friend bool operator==(const Result&, const Result&) = default;

  std::string tokens;
  std::string errors;
};

// A string in the notation of the tests, every code unit outside printable
// ASCII as \uXXXX, so that lone surrogates show.
std::string Quote(std::u16string_view string) {
  std::string quoted = "\"";
  for (const char16_t unit : string) {
    if (unit >= 0x20 && unit < 0x7F && unit != '"' && unit != '\\') {
      quoted.push_back(static_cast<char>(unit));
    } else {
      base::StringAppendF(&quoted, "\\u%04X", static_cast<unsigned>(unit));
    }
  }
  quoted.push_back('"');
  return quoted;
}

// What doubleEscaped asks for: \uXXXX becomes that code unit.
std::u16string Unescape(std::u16string_view string) {
  std::u16string unescaped;
  while (!string.empty()) {
    if (string.size() >= 6 && string.starts_with(u"\\u") &&
        std::ranges::all_of(string.substr(2, 4), [](char16_t unit) {
          return base::IsHexDigit(unit);
        })) {
      char16_t unit = 0;
      for (const char16_t digit : string.substr(2, 4)) {
        unit = static_cast<char16_t>(unit * 16 + base::HexDigitToInt(digit));
      }
      unescaped.push_back(unit);
      string.remove_prefix(6);
    } else {
      unescaped.push_back(string.front());
      string.remove_prefix(1);
    }
  }
  return unescaped;
}

std::u16string StringOfTest(const std::string& string, bool double_escaped) {
  std::u16string utf16 = base::UTF8ToUTF16(string);
  return double_escaped ? Unescape(utf16) : utf16;
}

std::string Join(const std::vector<std::string>& items,
                 std::string_view open,
                 std::string_view close) {
  return base::StrCat({open, base::JoinString(items, ", "), close});
}

std::string Attributes(std::vector<std::pair<std::string, std::string>> pairs) {
  std::ranges::sort(pairs);
  std::vector<std::string> items;
  for (const auto& [name, value] : pairs) {
    items.push_back(name + ": " + value);
  }
  return Join(items, "{", "}");
}

// The expected tokens, in the notation that TokensOf() writes.
std::optional<std::string> ExpectedTokens(const base::ListValue& output,
                                          bool double_escaped) {
  std::vector<std::string> tokens;
  std::optional<std::u16string> characters;
  const auto flush = [&] {
    if (characters) {
      tokens.push_back(Join({"\"Character\"", Quote(*characters)}, "[", "]"));
      characters.reset();
    }
  };
  for (const base::Value& token : output) {
    const base::ListValue* list = token.GetIfList();
    if (!list || list->empty() || !(*list)[0].is_string()) {
      return std::nullopt;
    }
    const std::string& type = (*list)[0].GetString();
    if (type == "Character" && list->size() == 2 && (*list)[1].is_string()) {
      // The tests run a token's adjacent characters together.
      characters = characters.value_or(u"") +
                   StringOfTest((*list)[1].GetString(), double_escaped);
      continue;
    }
    flush();
    std::vector<std::string> items = {Quote(base::UTF8ToUTF16(type))};
    for (const base::Value& item : base::span(*list).subspan(1u)) {
      if (item.is_string()) {
        items.push_back(Quote(StringOfTest(item.GetString(), double_escaped)));
      } else if (item.is_none()) {
        items.push_back("null");
      } else if (item.is_bool()) {
        items.push_back(item.GetBool() ? "true" : "false");
      } else if (item.is_dict()) {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (const auto [name, value] : item.GetDict()) {
          if (!value.is_string()) {
            return std::nullopt;
          }
          pairs.emplace_back(
              Quote(StringOfTest(name, double_escaped)),
              Quote(StringOfTest(value.GetString(), double_escaped)));
        }
        items.push_back(Attributes(std::move(pairs)));
      } else {
        return std::nullopt;
      }
    }
    tokens.push_back(Join(items, "[", "]"));
  }
  flush();
  return Join(tokens, "[", "]");
}

std::optional<std::string> ExpectedErrors(const base::ListValue* errors) {
  std::vector<std::string> items;
  if (errors) {
    for (const base::Value& error : *errors) {
      const base::DictValue* dict = error.GetIfDict();
      const std::string* code = dict ? dict->FindString("code") : nullptr;
      const std::optional<int> line =
          dict ? dict->FindInt("line") : std::nullopt;
      const std::optional<int> column =
          dict ? dict->FindInt("col") : std::nullopt;
      if (!code || !line || !column) {
        return std::nullopt;
      }
      items.push_back(
          base::StringPrintf("%s %d:%d", code->c_str(), *line, *column));
    }
  }
  return Join(items, "[", "]");
}

std::string TokensOf(const std::vector<HtmlToken>& tokens) {
  std::vector<std::string> items;
  std::optional<std::u16string> characters;
  const auto flush = [&] {
    if (characters) {
      items.push_back(Join({"\"Character\"", Quote(*characters)}, "[", "]"));
      characters.reset();
    }
  };
  for (const HtmlToken& token : tokens) {
    if (token.type == HtmlToken::Type::kCharacter) {
      characters = characters.value_or(u"") + token.data;
      continue;
    }
    flush();
    switch (token.type) {
      case HtmlToken::Type::kDoctype:
        items.push_back(Join(
            {"\"DOCTYPE\"", token.name_missing ? "null" : Quote(token.name),
             token.public_identifier ? Quote(*token.public_identifier) : "null",
             token.system_identifier ? Quote(*token.system_identifier) : "null",
             token.force_quirks ? "false" : "true"},
            "[", "]"));
        break;
      case HtmlToken::Type::kStartTag: {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (const HtmlAttribute& attribute : token.attributes) {
          pairs.emplace_back(Quote(attribute.name), Quote(attribute.value));
        }
        std::vector<std::string> start_tag = {"\"StartTag\"", Quote(token.name),
                                              Attributes(std::move(pairs))};
        if (token.self_closing) {
          start_tag.push_back("true");
        }
        items.push_back(Join(start_tag, "[", "]"));
        break;
      }
      case HtmlToken::Type::kEndTag:
        items.push_back(Join({"\"EndTag\"", Quote(token.name)}, "[", "]"));
        break;
      case HtmlToken::Type::kComment:
        items.push_back(Join({"\"Comment\"", Quote(token.data)}, "[", "]"));
        break;
      case HtmlToken::Type::kProcessingInstruction:
        items.push_back(Join(
            {"\"ProcessingInstruction\"", Quote(token.name), Quote(token.data)},
            "[", "]"));
        break;
      case HtmlToken::Type::kCharacter:
      case HtmlToken::Type::kEndOfFile:
      case HtmlToken::Type::kUninitialized:
        break;
    }
  }
  flush();
  return Join(items, "[", "]");
}

// Tokenizes the run's input in pieces of `piece_size` code units, or in one
// piece if it is 0.
Result Tokenize(const Run& run,
                size_t piece_size,
                bool processing_instructions) {
  HtmlTokenizer tokenizer({.processing_instructions = processing_instructions,
                           .collect_parse_errors = true});
  tokenizer.SetState(run.state);
  if (run.last_start_tag) {
    tokenizer.SetLastStartTagName(*run.last_start_tag);
  }
  // As Blink's tree builder sets them for these states: RCDATA, RAWTEXT and
  // script data come in its text insertion mode, and a CDATA section in
  // foreign content.
  tokenizer.SetForceNullCharacterReplacement(
      run.state == State::kRcdata || run.state == State::kRawtext ||
      run.state == State::kScriptData || run.state == State::kCdataSection);
  tokenizer.SetAllowCdata(run.state == State::kCdataSection);

  std::vector<HtmlToken> tokens;
  bool end_of_file = false;
  const auto take_tokens = [&] {
    HtmlToken token;
    while (tokenizer.NextToken(token)) {
      CHECK(!end_of_file);
      end_of_file = token.type == HtmlToken::Type::kEndOfFile;
      tokens.push_back(std::move(token));
    }
  };
  std::u16string_view input = run.input;
  while (!input.empty()) {
    const size_t size =
        piece_size ? std::min(piece_size, input.size()) : input.size();
    tokenizer.Append(input.substr(0, size));
    input.remove_prefix(size);
    take_tokens();
  }
  CHECK(!end_of_file);
  tokenizer.Finish();
  take_tokens();
  CHECK(end_of_file);

  std::vector<std::string> errors;
  for (const HtmlTokenizer::ParseError& error : tokenizer.TakeParseErrors()) {
    errors.push_back(base::StringPrintf(
        "%s %d:%d", std::string(HtmlParseErrorName(error.code)).c_str(),
        error.line, error.column));
  }
  return {TokensOf(tokens), Join(errors, "[", "]")};
}

// Reads every run of a .test file, or says what is wrong with it.
bool ReadRuns(const base::FilePath& path, std::vector<Run>& runs) {
  const std::string file = path.BaseName().AsUTF8Unsafe();
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    fprintf(stderr, "cannot read %s\n", path.AsUTF8Unsafe().c_str());
    return false;
  }
  const std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  const base::ListValue* tests = nullptr;
  if (root) {
    // xmlViolation.test names its list otherwise, and its tests expect the
    // tokens to be made fit for XML.
    tests = root->FindList("tests");
    if (!tests) {
      tests = root->FindList("xmlViolationTests");
    }
  }
  if (!tests) {
    fprintf(stderr, "%s holds no tests\n", file.c_str());
    return false;
  }
  for (size_t index = 0; index < tests->size(); ++index) {
    const base::DictValue* test = (*tests)[index].GetIfDict();
    const std::string* description =
        test ? test->FindString("description") : nullptr;
    const std::string* input = test ? test->FindString("input") : nullptr;
    const base::ListValue* output = test ? test->FindList("output") : nullptr;
    if (!description || !input || !output) {
      fprintf(stderr, "%s:%zu is not a test\n", file.c_str(), index);
      return false;
    }
    const bool double_escaped = test->FindBool("doubleEscaped").value_or(false);
    const std::optional<std::string> expected_tokens =
        ExpectedTokens(*output, double_escaped);
    const std::optional<std::string> expected_errors =
        ExpectedErrors(test->FindList("errors"));
    if (!expected_tokens || !expected_errors) {
      fprintf(stderr, "%s:%zu has output of another form\n", file.c_str(),
              index);
      return false;
    }
    std::optional<std::u16string> last_start_tag;
    if (const std::string* tag = test->FindString("lastStartTag")) {
      last_start_tag = base::UTF8ToUTF16(*tag);
    }
    std::vector<std::string> state_names = {"Data state"};
    if (const base::ListValue* states = test->FindList("initialStates")) {
      state_names.clear();
      for (const base::Value& state : *states) {
        state_names.push_back(state.is_string() ? state.GetString() : "");
      }
    }
    for (const std::string& state_name : state_names) {
      const auto initial =
          std::ranges::find(kInitialStates, state_name, &InitialState::name);
      if (initial == kInitialStates.end()) {
        fprintf(stderr, "%s:%zu starts in a state of another name\n",
                file.c_str(), index);
        return false;
      }
      runs.push_back({
          .id = base::StringPrintf("%s:%zu:%s", file.c_str(), index,
                                   state_name.c_str()),
          .description = *description,
          .input = StringOfTest(*input, double_escaped),
          .state = initial->state,
          .last_start_tag = last_start_tag,
          .expected_tokens = *expected_tokens,
          .expected_errors = *expected_errors,
      });
    }
  }
  return true;
}

// The tests that the list names: each line that does not start with # is
// one, up to a # that starts a comment.
std::optional<std::set<std::string>> ReadFailures(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    fprintf(stderr, "cannot read %s\n", path.AsUTF8Unsafe().c_str());
    return std::nullopt;
  }
  std::set<std::string> failures;
  for (std::string_view line : base::SplitStringPiece(
           contents, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    line = line.substr(0, line.find('#'));
    line = base::TrimWhitespaceASCII(line, base::TRIM_ALL);
    if (!line.empty() && !failures.emplace(line).second) {
      fprintf(stderr, "%s names %s twice\n", path.AsUTF8Unsafe().c_str(),
              std::string(line).c_str());
      return std::nullopt;
    }
  }
  return failures;
}

int Main() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  const base::CommandLine::StringVector arguments = command_line.GetArgs();
  if (arguments.size() != 2) {
    fprintf(stderr,
            "usage: html_tokenizer_test [--print-failures] "
            "[--without-processing-instructions] TESTS FAILURES\n");
    return 2;
  }
  const bool print_failures = command_line.HasSwitch("print-failures");
  const bool processing_instructions =
      !command_line.HasSwitch("without-processing-instructions");

  std::vector<base::FilePath> files;
  base::FileEnumerator enumerator(base::FilePath(arguments[0]),
                                  /*recursive=*/false,
                                  base::FileEnumerator::FILES,
                                  FILE_PATH_LITERAL("*.test"));
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    files.push_back(path);
  }
  std::ranges::sort(files);
  if (files.empty()) {
    fprintf(stderr, "no .test files in %s\n",
            base::FilePath(arguments[0]).AsUTF8Unsafe().c_str());
    return 1;
  }
  std::vector<Run> runs;
  for (const base::FilePath& file : files) {
    if (!ReadRuns(file, runs)) {
      return 1;
    }
  }
  std::optional<std::set<std::string>> listed =
      ReadFailures(base::FilePath(arguments[1]));
  if (!listed) {
    return 1;
  }

  size_t problems = 0;
  size_t failures = 0;
  size_t listed_failures = 0;
  for (const Run& run : runs) {
    const Result whole = Tokenize(run, 0, processing_instructions);
    const Result pieces = Tokenize(run, 1, processing_instructions);
    if (pieces != whole) {
      ++problems;
      printf(
          "%s differs one code unit at a time  # %s\n  input:  %s\n"
          "  whole:  %s %s\n  pieces: %s %s\n",
          run.id.c_str(), run.description.c_str(), Quote(run.input).c_str(),
          whole.tokens.c_str(), whole.errors.c_str(), pieces.tokens.c_str(),
          pieces.errors.c_str());
    }
    const bool passes = whole.tokens == run.expected_tokens &&
                        whole.errors == run.expected_errors;
    const bool is_listed = listed->erase(run.id) > 0;
    if (!passes) {
      ++failures;
      if (print_failures) {
        printf("%s  # %s\n", run.id.c_str(), run.description.c_str());
      }
    }
    if (!passes && is_listed) {
      ++listed_failures;
    }
    if (passes && is_listed) {
      ++problems;
      printf("%s passes, but the list of failures names it\n", run.id.c_str());
    } else if (!passes && !is_listed) {
      ++problems;
      printf(
          "%s fails  # %s\n  input:    %s\n  expected: %s %s\n"
          "  actual:   %s %s\n",
          run.id.c_str(), run.description.c_str(), Quote(run.input).c_str(),
          run.expected_tokens.c_str(), run.expected_errors.c_str(),
          whole.tokens.c_str(), whole.errors.c_str());
    }
  }
  for (const std::string& id : *listed) {
    ++problems;
    printf("%s is in the list of failures, but no such test runs\n",
           id.c_str());
  }
  printf("%zu runs of %zu files: %zu pass, %zu fail as listed, %zu problems\n",
         runs.size(), files.size(), runs.size() - failures, listed_failures,
         problems);
  return problems ? 1 : 0;
}

}  // namespace
}  // namespace mitto

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  return mitto::Main();
}
