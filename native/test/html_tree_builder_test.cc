// Runs html5lib's tree construction tests against the tree builder, and holds
// what fails to the list of failures that is kept with their reasons:
//
//   html_tree_builder_test [--print-failures] [--print-skipped] TESTS FAILURES
//
// TESTS is a directory of html5lib's .dat files, and FAILURES the list,
// native/test/html5lib_tree_failures.txt. A test passes when the tree it
// builds is the one it expects. One that builds another tree is a failure
// unless the tree builder recorded a rule it could not follow, because the DOM
// lacks what the rule needs: such a document may get another tree, but never
// without saying which rule. It exits with an error if a test that the list
// names passes or records a rule, or if one that it does not name fails.
// --print-failures prints every failure in the form of the list, without its
// reasons, and --print-skipped each test that recorded a rule, with the rules.
//
// Tests with #script-off are left out: a Page runs scripts.

#include "mitto/dom/html_tree_builder.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/icu/source/common/unicode/utypes.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-locker.h"
#include "v8/include/v8-snapshot.h"

// The Page program's data, which the test links as the program does.
extern "C" {
extern const char mitto_icu_data[];
extern const char mitto_v8_snapshot[];
extern const uint64_t mitto_v8_snapshot_size;
}

namespace mitto {
namespace {

struct Test {
  std::string id;
  std::string description;
  std::u16string data;
  // The context element's name, with a namespace before it for SVG and
  // MathML, for a fragment test.
  std::optional<std::string> fragment_context;
  bool script_off = false;
  std::string expected;
};

// Reads a .dat file: tests start with #data, and each section with a line
// that starts with #.
std::vector<Test> ReadTests(const base::FilePath& path) {
  std::string text;
  CHECK(base::ReadFileToString(path, &text)) << path;
  CHECK(text.starts_with("#data\n")) << path;
  text.erase(0, std::string_view("#data\n").size());
  std::vector<Test> tests;
  const std::string file = path.BaseName().AsUTF8Unsafe();
  int index = 0;
  for (std::string chunk : base::SplitStringUsingSubstr(
           text, "\n\n#data\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    while (chunk.ends_with('\n')) {
      chunk.pop_back();
    }
    Test test;
    test.id = base::StrCat({file, ":", base::NumberToString(index++)});
    const std::vector<std::string> lines = base::SplitString(
        chunk, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    std::string section = "#data";
    std::vector<std::string> data_lines;
    std::vector<std::string> document_lines;
    for (const std::string& line : lines) {
      if (line.starts_with('#') &&
          (line == "#errors" || line == "#new-errors" ||
           line == "#document-fragment" || line == "#script-off" ||
           line == "#script-on" || line == "#document")) {
        section = line;
        if (line == "#script-off") {
          test.script_off = true;
        }
        continue;
      }
      if (section == "#data") {
        data_lines.push_back(line);
      } else if (section == "#document-fragment") {
        test.fragment_context = line;
      } else if (section == "#document") {
        document_lines.push_back(line);
      }
    }
    const std::string data = base::JoinString(data_lines, "\n");
    test.data = base::UTF8ToUTF16(data);
    test.description = data.substr(0, data.find('\n'));
    test.expected = base::JoinString(document_lines, "\n");
    tests.push_back(std::move(test));
  }
  return tests;
}

void DumpNode(const Node& node, int depth, std::vector<std::string>& lines) {
  const std::string indent = "| " + std::string(depth * 2, ' ');
  switch (node.GetNodeType()) {
    case Node::kDocumentTypeNode: {
      const auto& doctype = static_cast<const DocumentType&>(node);
      std::string line = base::StrCat(
          {indent, "<!DOCTYPE ", base::UTF16ToUTF8(doctype.Name())});
      if (!doctype.PublicId().empty() || !doctype.SystemId().empty()) {
        base::StrAppend(&line,
                        {" \"", base::UTF16ToUTF8(doctype.PublicId()), "\" \"",
                         base::UTF16ToUTF8(doctype.SystemId()), "\""});
      }
      lines.push_back(line + ">");
      break;
    }
    case Node::kElementNode: {
      const auto& element = static_cast<const Element&>(node);
      std::string prefix;
      if (element.NamespaceURI() == html_names::kSvgNamespace) {
        prefix = "svg ";
      } else if (element.NamespaceURI() == html_names::kMathMlNamespace) {
        prefix = "math ";
      }
      lines.push_back(base::StrCat(
          {indent, "<", prefix, base::UTF16ToUTF8(element.LocalName()), ">"}));
      std::vector<Element::Attribute> attributes = element.attributes();
      std::sort(attributes.begin(), attributes.end(),
                [](const Element::Attribute& a, const Element::Attribute& b) {
                  return a.name.ToString() < b.name.ToString();
                });
      const std::string attribute_indent =
          "| " + std::string((depth + 1) * 2, ' ');
      for (const Element::Attribute& attribute : attributes) {
        lines.push_back(base::StrCat(
            {attribute_indent, base::UTF16ToUTF8(attribute.name.ToString()),
             "=\"", base::UTF16ToUTF8(attribute.value), "\""}));
      }
      break;
    }
    case Node::kTextNode:
      lines.push_back(base::StrCat(
          {indent, "\"",
           base::UTF16ToUTF8(static_cast<const CharacterData&>(node).Data()),
           "\""}));
      break;
    case Node::kCommentNode:
      lines.push_back(base::StrCat(
          {indent, "<!-- ",
           base::UTF16ToUTF8(static_cast<const CharacterData&>(node).Data()),
           " -->"}));
      break;
    default:
      break;
  }
  for (const Node* child = node.FirstChild(); child;
       child = child->NextSibling()) {
    DumpNode(*child, depth + 1, lines);
  }
}

struct Outcome {
  std::string tree;
  std::vector<std::string> skipped_rules;
};

Outcome Run(const Test& test) {
  Outcome outcome;
  std::vector<std::string> lines;
  const cppgc::Persistent<Document> document(Document::CreateHTML());
  if (!test.fragment_context) {
    outcome.skipped_rules = ParseHtmlDocument(*document, test.data);
    for (const Node* child = document->FirstChild(); child;
         child = child->NextSibling()) {
      DumpNode(*child, 0, lines);
    }
  } else {
    const std::string& context = *test.fragment_context;
    const char16_t* namespace_uri = html_names::kXhtmlNamespace;
    std::string name = context;
    if (context.starts_with("svg ")) {
      namespace_uri = html_names::kSvgNamespace;
      name = context.substr(4);
    } else if (context.starts_with("math ")) {
      namespace_uri = html_names::kMathMlNamespace;
      name = context.substr(5);
    }
    const cppgc::Persistent<Element> element(document->CreateElement(
        namespace_uri, std::nullopt, base::UTF8ToUTF16(name)));
    const NodeVector nodes =
        ParseHtmlFragment(*element, test.data, &outcome.skipped_rules);
    for (const cppgc::Persistent<Node>& node : nodes) {
      DumpNode(*node, 0, lines);
    }
  }
  outcome.tree = base::JoinString(lines, "\n");
  return outcome;
}

// The ids of the tests that the list of failures names.
std::set<std::string> ReadFailures(const base::FilePath& path) {
  std::string text;
  CHECK(base::ReadFileToString(path, &text)) << path;
  std::set<std::string> failures;
  for (const std::string_view line : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.starts_with('#')) {
      continue;
    }
    failures.emplace(base::TrimWhitespaceASCII(line.substr(0, line.find('#')),
                                               base::TRIM_ALL));
  }
  return failures;
}

int RunTests(const base::FilePath& directory,
             const base::FilePath& failures_path,
             bool print_failures,
             bool print_skipped) {
  std::vector<base::FilePath> files;
  base::FileEnumerator enumerator(directory, /*recursive=*/false,
                                  base::FileEnumerator::FILES,
                                  FILE_PATH_LITERAL("*.dat"));
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    files.push_back(path);
  }
  std::sort(files.begin(), files.end());
  const std::set<std::string> listed = ReadFailures(failures_path);
  int passed = 0;
  int skipped = 0;
  int failed = 0;
  int problems = 0;
  std::map<std::string, int> rules;
  std::set<std::string> seen;
  for (const base::FilePath& file : files) {
    for (const Test& test : ReadTests(file)) {
      if (test.script_off) {
        continue;
      }
      seen.insert(test.id);
      const Outcome outcome = Run(test);
      const bool is_listed = listed.contains(test.id);
      for (const std::string& rule : outcome.skipped_rules) {
        ++rules[rule];
      }
      if (outcome.tree == test.expected) {
        ++passed;
        if (is_listed) {
          printf("%s passes, but the list of failures names it\n",
                 test.id.c_str());
          ++problems;
        }
        continue;
      }
      if (!outcome.skipped_rules.empty()) {
        ++skipped;
        if (print_skipped) {
          printf("%s  # %s\n", test.id.c_str(),
                 base::JoinString(outcome.skipped_rules, ", ").c_str());
        }
        if (is_listed) {
          printf("%s skips a rule, but the list of failures names it\n",
                 test.id.c_str());
          ++problems;
        }
        continue;
      }
      ++failed;
      if (print_failures) {
        printf("%s  # %s\n", test.id.c_str(), test.description.c_str());
      }
      if (!is_listed) {
        printf(
            "%s fails without skipping a rule: %s\nexpected:\n%s\ngot:\n%s\n",
            test.id.c_str(), test.description.c_str(), test.expected.c_str(),
            outcome.tree.c_str());
        ++problems;
      }
    }
  }
  for (const std::string& id : listed) {
    if (!seen.contains(id)) {
      printf("the list of failures names %s, which no test is\n", id.c_str());
      ++problems;
    }
  }
  printf("%d passed, %d skipped a rule, %d failed, %d problems\n", passed,
         skipped, failed, problems);
  for (const auto& [rule, count] : rules) {
    printf("  skipped %s in %d tests\n", rule.c_str(), count);
  }
  return problems ? 1 : 0;
}

}  // namespace
}  // namespace mitto

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  const base::CommandLine::StringVector arguments = command_line.GetArgs();
  if (arguments.size() != 2) {
    fprintf(stderr,
            "usage: html_tree_builder_test [--print-failures] "
            "[--print-skipped] TESTS FAILURES\n");
    return 2;
  }

  UErrorCode status = U_ZERO_ERROR;
  udata_setCommonData(mitto_icu_data, &status);
  CHECK(U_SUCCESS(status));
  v8::StartupData snapshot = {mitto_v8_snapshot,
                              base::checked_cast<int>(mitto_v8_snapshot_size)};
  v8::V8::SetSnapshotDataBlob(&snapshot);
  const std::unique_ptr<v8::Platform> platform =
      v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  CHECK(v8::V8::Initialize());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator_shared =
      std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  params.cpp_heap =
      v8::CppHeap::Create(platform.get(), v8::CppHeapCreateParams({}))
          .release();
  v8::Isolate* const isolate = v8::Isolate::New(params);
  int result;
  {
    const v8::Isolate::Scope isolate_scope(isolate);
    result = mitto::RunTests(base::FilePath(arguments[0]),
                             base::FilePath(arguments[1]),
                             command_line.HasSwitch("print-failures"),
                             command_line.HasSwitch("print-skipped"));
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return result;
}
