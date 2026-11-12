// Runs the cases of Blink's media_query_set_test.cc and
// media_query_evaluator_test.cc that need no layout against the media query
// parser and evaluator in native/css:
//
//   media_query_test
//
// It prints each case that fails, with what it expected and what it got, and
// exits with an error if any does. The tables are Blink's own, copied as the
// Engine Release has them, and each check below sets up MediaValues as the
// Blink test of the same name sets up MediaValuesCachedData. Blink's unit tests
// run with the runtime-enabled features of status "experimental" and "test"
// on, so these parse inverted-colors, prefers-reduced-data,
// video-dynamic-range, display-state and resizable.
//
// Left out: Dynamic and DynamicNoView, which need a frame and its layout;
// DependentResults, which checks what a style sheet's cache depends on; and
// the custom media tests, whose @custom-media rules need style sheets.
//
// Upstream: third_party/blink/renderer/core/css/media_query_set_test.cc
// Upstream: third_party/blink/renderer/core/css/media_query_evaluator_test.cc

#include "mitto/css/media_query.h"

#include <stdio.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/media_query_evaluator.h"
#include "mitto/css/media_query_parser.h"
#include "mitto/css/media_values.h"

namespace mitto {
namespace {

struct SetCase {
  const char* input;
  const char* output;
};

struct EvalCase {
  const char* input;
  bool output;
};

constexpr SetCase kSetBasicCases[] = {
    {"", nullptr},
    {" ", ""},
    {"screen", nullptr},
    {"screen and (color)", nullptr},
    {"all and (min-width:500px)", "(min-width: 500px)"},
    {"all and (min-width:/*bla*/500px)", "(min-width: 500px)"},
    {"(min-width:500px)", "(min-width: 500px)"},
    {"screen and (color), projection and (color)", nullptr},
    {"not screen and (color)", nullptr},
    {"only screen and (color)", nullptr},
    {"screen and (color), projection and (color)", nullptr},
    {"aural and (device-aspect-ratio: 16 / 9)", nullptr},
    {"speech and (min-device-width: 800px)", nullptr},
    {"example", nullptr},
    {"screen and (max-weight: 3kg) and (color), (monochrome)",
     "not all, (monochrome)"},
    {"(min-width: -100px)", "(min-width: -100px)"},
    {"(width:100gil)", "not all"},
    {"(example, all,), speech", "not all, speech"},
    {"&test, screen", "not all, screen"},
    {"print and (min-width: 25cm)", nullptr},
    {"screen and (min-width: 400px) and (max-width: 700px)", nullptr},
    {"screen and (device-width: 800px)", nullptr},
    {"screen and (device-height: 60em)", nullptr},
    {"screen and (device-height: 60rem)", nullptr},
    {"screen and (device-height: 60ch)", nullptr},
    {"screen and (device-aspect-ratio: 16 / 9)", nullptr},
    {"(device-aspect-ratio: 16.1/9.0)", "(device-aspect-ratio: 16.1 / 9)"},
    {"(device-aspect-ratio: 16.0)", "(device-aspect-ratio: 16 / 1)"},
    {"(device-aspect-ratio: 16/ 9)", "(device-aspect-ratio: 16 / 9)"},
    {"(device-aspect-ratio: 16/\r9)", "(device-aspect-ratio: 16 / 9)"},
    {"all and (color)", "(color)"},
    {"all and (min-color: 1)", "(min-color: 1)"},
    {"all and (min-color: 1.0)", "not all"},
    {"all and (min-color: 2)", "(min-color: 2)"},
    {"all and (color-index)", "(color-index)"},
    {"all and (min-color-index: 1)", "(min-color-index: 1)"},
    {"all and (monochrome)", "(monochrome)"},
    {"all and (min-monochrome: 1)", "(min-monochrome: 1)"},
    {"all and (min-monochrome: 2)", "(min-monochrome: 2)"},
    {"print and (monochrome)", nullptr},
    {"handheld and (grid) and (max-width: 15em)", nullptr},
    {"handheld and (grid) and (max-device-height: 7em)", nullptr},
    {"screen and (max-width: 50%)", "not all"},
    {"screen and (max-WIDTH: 500px)", "screen and (max-width: 500px)"},
    {"screen and (max-width: 24.4em)", nullptr},
    {"screen and (max-width: 24.4EM)", "screen and (max-width: 24.4em)"},
    {"screen and (max-width: blabla)", "not all"},
    {"screen and (max-width: 1)", "not all"},
    {"screen and (max-width: 0)", "screen and (max-width: 0)"},
    {"screen and (max-width: 1deg)", "not all"},
    {"handheld and (min-width: 20em), \nscreen and (min-width: 20em)",
     "handheld and (min-width: 20em), screen and (min-width: 20em)"},
    {"print and (min-resolution: 300dpi)", nullptr},
    {"print and (min-resolution: 118dpcm)", nullptr},
    {"(resolution: 0.83333333333333333333dppx)", "(resolution: 0.833333dppx)"},
    {"(resolution: 2.4dppx)", nullptr},
    {"(resolution: calc(1dppx))", "(resolution: calc(1dppx))"},
    {"(resolution: calc(1x))", "(resolution: calc(1dppx))"},
    {"(resolution: calc(96dpi))", "(resolution: calc(1dppx))"},
    {"(resolution: calc(1x + 2x))", "(resolution: calc(3dppx))"},
    {"(resolution: calc(3x - 2x))", "(resolution: calc(1dppx))"},
    {"(resolution: calc(1x * 3))", "(resolution: calc(3dppx))"},
    {"(resolution: calc(6x / 2))", "(resolution: calc(3dppx))"},
    {"all and(color)", "not all"},
    {"all and (", "not all"},
    {"test;,all", "not all, all"},
    {"(color:20example)", "not all"},
    {"not braille", nullptr},
    {",screen", "not all, screen"},
    {",all", "not all, all"},
    {",,all,,", "not all, not all, all, not all, not all"},
    {",,all,, ", "not all, not all, all, not all, not all"},
    {",screen,,&invalid,,",
     "not all, screen, not all, not all, not all, not all"},
    {",screen,,(invalid,),,",
     "not all, screen, not all, not all, not all, not all"},
    {",(all,),,", "not all, not all, not all, not all"},
    {",", "not all, not all"},
    {"  ", ""},
    {"(color", "(color)"},
    {"(min-color: 2", "(min-color: 2)"},
    {"(orientation: portrait)", nullptr},
    {"tv and (scan: progressive)", nullptr},
    {"(pointer: coarse)", nullptr},
    {"(min-orientation:portrait)", "not all"},
    {"all and (orientation:portrait)", "(orientation: portrait)"},
    {"all and (orientation:landscape)", "(orientation: landscape)"},
    {"NOT braille, tv AND (max-width: 200px) and (min-WIDTH: 100px) and "
     "(orientation: landscape), (color)",
     "not braille, tv and (max-width: 200px) and (min-width: 100px) and "
     "(orientation: landscape), (color)"},
    {"(m\\61x-width: 300px)", "(max-width: 300px)"},
    {"(max-width: 400\\70\\78)", "(max-width: 400px)"},
    {"(max-width: 500\\0070\\0078)", "(max-width: 500px)"},
    {"(max-width: 600\\000070\\000078)", "(max-width: 600px)"},
    {"(max-width: 700px), (max-width: 700px)",
     "(max-width: 700px), (max-width: 700px)"},
    {"(max-width: 800px()), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px(()), (max-width: 900px)", "not all"},
    {"(max-width: 600px(())))), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px(((((((((())))), (max-width: 500px)", "not all"},
    {"(max-width: 800px[]), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px[[]), (max-width: 900px)", "not all"},
    {"(max-width: 600px[[]]]]), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px[[[[[[[[[[]]]]), (max-width: 500px)", "not all"},
    {"(max-width: 800px{}), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px{{}), (max-width: 900px)", "not all"},
    {"(max-width: 600px{{}}}}), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px{{{{{{{{{{}}}}), (max-width: 500px)", "not all"},
    {"[(), (max-width: 400px)", "not all"},
    {"[{}, (max-width: 500px)", "not all"},
    {"[{]}], (max-width: 900px)", "not all, (max-width: 900px)"},
    {"[{[]{}{{{}}}}], (max-width: 900px)", "not all, (max-width: 900px)"},
    {"[{[}], (max-width: 900px)", "not all"},
    {"[({)}], (max-width: 900px)", "not all"},
    {"[]((), (max-width: 900px)", "not all"},
    {"((), (max-width: 900px)", "not all"},
    {"(foo(), (max-width: 900px)", "not all"},
    {"[](()), (max-width: 900px)", "not all, (max-width: 900px)"},
    {"all an[isdfs bla())()]icalc(i)(()), (max-width: 400px)",
     "not all, (max-width: 400px)"},
    {"all an[isdfs bla())(]icalc(i)(()), (max-width: 500px)", "not all"},
    {"all an[isdfs bla())(]icalc(i)(())), (max-width: 600px)", "not all"},
    {"all an[isdfs bla())(]icalc(i)(()))], (max-width: 800px)",
     "not all, (max-width: 800px)"},
    {"(max-width: '40px')", "not all"},
    {"('max-width': 40px)", "not all"},
    {"'\"'\", (max-width: 900px)", "not all"},
    {"'\"\"\"', (max-width: 900px)", "not all, (max-width: 900px)"},
    {"\"'\"', (max-width: 900px)", "not all"},
    {"\"'''\", (max-width: 900px)", "not all, (max-width: 900px)"},
    {"not not", "not all"},
    {"not and", "not all"},
    {"not only", "not all"},
    {"not or", "not all"},
    {"only not", "not all"},
    {"only and", "not all"},
    {"only only", "not all"},
    {"only or", "not all"},
    {"layer", "not all"},
    {"not layer", "not all"},
    {"not (orientation)", nullptr},
    {"only (orientation)", "not all"},
    {"(max-width: 800px()), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px(()), (max-width: 900px)", "not all"},
    {"(max-width: 600px(())))), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px(((((((((())))), (max-width: 500px)", "not all"},
    {"(max-width: 800px[]), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px[[]), (max-width: 900px)", "not all"},
    {"(max-width: 600px[[]]]]), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px[[[[[[[[[[]]]]), (max-width: 500px)", "not all"},
    {"(max-width: 800px{}), (max-width: 800px)", "not all, (max-width: 800px)"},
    {"(max-width: 900px{{}), (max-width: 900px)", "not all"},
    {"(max-width: 600px{{}}}}), (max-width: 600px)",
     "not all, (max-width: 600px)"},
    {"(max-width: 500px{{{{{{{{{{}}}}), (max-width: 500px)", "not all"},
    {"[(), (max-width: 400px)", "not all"},
    {"[{}, (max-width: 500px)", "not all"},
    {"[{]}], (max-width: 900px)", "not all, (max-width: 900px)"},
    {"[{[]{}{{{}}}}], (max-width: 900px)", "not all, (max-width: 900px)"},
    {"[{[}], (max-width: 900px)", "not all"},
    {"[({)}], (max-width: 900px)", "not all"},
    {"[]((), (max-width: 900px)", "not all"},
    {"((), (max-width: 900px)", "not all"},
    {"(foo(), (max-width: 900px)", "not all"},
    {"[](()), (max-width: 900px)", "not all, (max-width: 900px)"},
    {"all an[isdfs bla())(i())]icalc(i)(()), (max-width: 400px)",
     "not all, (max-width: 400px)"},
    {"all an[isdfs bla())(]icalc(i)(()), (max-width: 500px)", "not all"},
    {"all an[isdfs bla())(]icalc(i)(())), (max-width: 600px)", "not all"},
    {"all an[isdfs bla())(]icalc(i)(()))], (max-width: 800px)",
     "not all, (max-width: 800px)"},
    {"(inline-size > 0px)", "not all"},
    {"(min-inline-size: 0px)", "not all"},
    {"(max-inline-size: 0px)", "not all"},
    {"(block-size > 0px)", "not all"},
    {"(min-block-size: 0px)", "not all"},
    {"(max-block-size: 0px)", "not all"},
    {"(device-aspect-ratio: calc(16.1)/calc(9.0))",
     "(device-aspect-ratio: calc(16.1) / calc(9))"},
    {"(device-aspect-ratio: calc(16.1)/9.0)",
     "(device-aspect-ratio: calc(16.1) / 9)"},
};

constexpr SetCase kSetMediaQueries4Cases[] = {
    {"(width: 100px) or (width: 200px)", nullptr},
    {"(width: 100px)or (width: 200px)", "(width: 100px) or (width: 200px)"},
    {"(width: 100px) or (width: 200px) or (color)", nullptr},
    {"screen and (width: 100px) or (width: 200px)", "not all"},
    {"(height: 100px) and (width: 100px) or (width: 200px)", "not all"},
    {"(height: 100px) or (width: 100px) and (width: 200px)", "not all"},
    {"((width: 100px))", nullptr},
    {"(((width: 100px)))", nullptr},
    {"(   (   (width: 100px) ) )", "(((width: 100px)))"},
    {"(width: 100px) or ((width: 200px) or (width: 300px))", nullptr},
    {"(width: 100px) and ((width: 200px) or (width: 300px))", nullptr},
    {"(width: 100px) or ((width: 200px) and (width: 300px))", nullptr},
    {"(width: 100px) or ((width: 200px) and (width: 300px)) and (width: "
     "400px)",
     "not all"},
    {"(width: 100px) and ((width: 200px) and (width: 300px)) or (width: "
     "400px)",
     "not all"},
    {"(width: 100px) or ((width: 200px) and (width: 300px)) or (width: "
     "400px)",
     nullptr},
    {"(width: 100px) and ((width: 200px) and (width: 300px)) and (width: "
     "400px)",
     nullptr},
    {"not (width: 100px)", nullptr},
    {"(width: 100px) and (not (width: 200px))", nullptr},
    {"(width: 100px) and not (width: 200px)", "not all"},
    {"(width < 100px)", nullptr},
    {"(width <= 100px)", nullptr},
    {"(width > 100px)", nullptr},
    {"(width >= 100px)", nullptr},
    {"(width = 100px)", nullptr},
    {"(100px < width)", nullptr},
    {"(100px <= width)", nullptr},
    {"(100px > width)", nullptr},
    {"(100px >= width)", nullptr},
    {"(100px = width)", nullptr},
    {"(100px < width < 200px)", nullptr},
    {"(100px <= width <= 200px)", nullptr},
    {"(100px < width <= 200px)", nullptr},
    {"(100px <= width < 200px)", nullptr},
    {"(200px > width > 100px)", nullptr},
    {"(200px >= width >= 100px)", nullptr},
    {"(200px > width >= 100px)", nullptr},
    {"(200px >= width > 100px)", nullptr},
    {"(not (width < 100px)) and (height > 200px)", nullptr},
    {"(width<100px)", "(width < 100px)"},
    {"(width>=100px)", "(width >= 100px)"},
    {"(width=100px)", "(width = 100px)"},
    {"(200px>=width > 100px)", "(200px >= width > 100px)"},
    {"(200px>=width>100px)", "(200px >= width > 100px)"},
};

constexpr const char* kGeneralEnclosedUnknownCases[] = {
    "()",
    "( )",
    "(1)",
    "( 1 )",
    "(1px)",
    "(unknown)",
    "(unknown: 50kg)",
    "unknown()",
    "unknown(1)",
    "(a b c)",
    "(width <> height)",
    "( a! b; )",
    "not screen and (unknown)",
    "not all and (unknown)",
    "not all and (width) and (unknown)",
    "not all and (not ((width) or (unknown)))",
    "(width: 100px) or (max-width: 50%)",
    "(width: 100px) or ((width: 200px) and (width: 300px) or (width: "
    "400px))",
    "(width: 100px) or ((width: 200px) or (width: 300px) and (width: "
    "400px))",
    "(width < 50%)",
    "(width < 100px nonsense)",
    "(100px nonsense < 100px)",
    "(width == 100px)",
    "(width << 100px)",
    "(width <> 100px)",
    "(100px == width)",
    "(100px < = width)",
    "(100px > = width)",
    "(100px==width)",
    "(100px , width)",
    "(100px,width)",
    "(100px ! width)",
    "(1px < width > 2px)",
    "(1px > width < 2px)",
    "(1px <= width > 2px)",
    "(1px > width <= 2px)",
    "(1px = width = 2px)",
    "(min-width < 10px)",
    "(max-width < 10px)",
    "(10px < min-width)",
    "(10px < min-width < 20px)",
    "(100px ! width < 200px)",
    "(100px < width ! 200px)",
    "(100px <)",
    "(100px < )",
    "(100px < width <)",
    "(100px < width < )",
    "(50% < width < 200px)",
    "(100px < width < 50%)",
    "(100px nonsense < width < 200px)",
    "(100px < width < 200px nonsense)",
    "(100px < width : 200px)",
};

constexpr const char* kGeneralEnclosedInvalidCases[] = {
    "(])",
    "(url(as'df))",
};

constexpr EvalCase kScreenTestCases[] = {
    {"", true},
    {" ", true},
    {"screen", true},
    {"screen and (color)", true},
    {"not screen and (color)", false},
    {"screen and (device-aspect-ratio: 16/9)", false},
    {"screen and (device-aspect-ratio: 0.5/0.5)", true},
    {"screen and (device-aspect-ratio: 1.5)", false},
    {"screen and (device-aspect-ratio: 1/1)", true},
    {"screen and (device-aspect-ratio: calc(1/1))", true},
    {"all and (min-color: 2)", true},
    {"all and (min-color: 32)", false},
    {"all and (min-color-index: 0)", true},
    {"all and (min-color-index: 1)", false},
    {"all and (monochrome)", false},
    {"all and (min-monochrome: 0)", true},
    {"all and (grid: 0)", true},
    {"(resolution: 2dppx)", true},
    {"(resolution: 1dppx)", false},
    {"(resolution: calc(2x))", true},
    {"(resolution: calc(1x))", false},
    {"(resolution: calc(1x + 1x))", true},
    {"(resolution: calc(1x + 0x))", false},
    {"(resolution: calc(1x + 96dpi))", true},
    {"(resolution: calc(0x + 37.79532dpcm))", false},
    {"(resolution: calc(3x - 1x))", true},
    {"(resolution: calc(3x - 2x))", false},
    {"(resolution: calc(3x - 96dpi))", true},
    {"(resolution: calc(2x - 37.79532dpcm))", false},
    {"(resolution: calc(1x * 2))", true},
    {"(resolution: calc(0.5x * 2))", false},
    {"(resolution: calc(4x / 2))", true},
    {"(resolution: calc(2x / 2))", false},
    {"(orientation: portrait)", true},
    {"(orientation: landscape)", false},
    {"(orientation: url(portrait))", false},
    {"(orientation: #portrait)", false},
    {"(orientation: @portrait)", false},
    {"(orientation: 'portrait')", false},
    {"(orientation: @junk portrait)", false},
    {"screen and (orientation: @portrait) and (max-width: 1000px)", false},
    {"screen and (orientation: @portrait), (max-width: 1000px)", true},
    {"tv and (scan: progressive)", false},
    {"(pointer: coarse)", false},
    {"(pointer: fine)", true},
    {"(hover: hover)", true},
    {"(hover: on-demand)", false},
    {"(hover: none)", false},
    {"(display-mode)", true},
    {"(display-mode: fullscreen)", false},
    {"(display-mode: standalone)", false},
    {"(display-mode: minimal-ui)", false},
    {"(display-mode: window-controls-overlay)", false},
    {"(display-mode: unframed)", false},
    {"(display-mode: browser)", true},
    {"(display-mode: min-browser)", false},
    {"(display-mode: url(browser))", false},
    {"(display-mode: #browser)", false},
    {"(display-mode: @browser)", false},
    {"(display-mode: 'browser')", false},
    {"(display-mode: @junk browser)", false},
    {"(display-mode: tabbed)", false},
    {"(display-mode: picture-in-picture)", false},
    {"(max-device-aspect-ratio: 4294967295/1)", true},
    {"(min-device-aspect-ratio: 1/4294967296)", true},
    {"(max-device-aspect-ratio: 0.5)", false},
    {"(max-device-aspect-ratio: 0.6/0.5)", true},
    {"(min-device-aspect-ratio: 1/2)", true},
    {"(max-device-aspect-ratio: 1.5)", true},
};

constexpr EvalCase kDisplayStateTestCases[] = {
    {"(display-state)", true},
    {"(display-state: fullscreen)", false},
    {"(display-state: minimized)", false},
    {"(display-state: maximized)", false},
    {"(display-state: normal)", true},
    {"(display-state: #normal)", false},
    {"(display-state: @normal)", false},
    {"(display-state: 'normal')", false},
    {"(display-state: @junk normal)", false},
};

constexpr EvalCase kResizableTestCases[] = {
    {"(resizable)", true},
    {"not (resizable)", false},
    {"(resizable: true)", true},
    {"(resizable: false)", false},
    {"(resizable: #true)", false},
    {"(resizable: @true)", false},
    {"(resizable: 'true')", false},
    {"(resizable: \"true\")", false},
    {"(resizable: @junk true)", false},
};

constexpr EvalCase kNonResizableTestCases[] = {
    {"(resizable)", false},
    {"not (resizable)", true},
    {"(resizable: true)", false},
    {"(resizable: false)", true},
    {"(resizable: #true)", false},
    {"(resizable: @true)", false},
    {"(resizable: 'true')", false},
    {"(resizable: \"true\")", false},
    {"(resizable: @junk true)", false},
};

constexpr EvalCase kMonochromeTestCases[] = {
    {"(color)", false},
    {"(monochrome)", true},
};

constexpr EvalCase kViewportTestCases[] = {
    {"all and (min-width: 500px)", true},
    {"(min-width: 500px)", true},
    {"(min-width: 501px)", false},
    {"(max-width: 500px)", true},
    {"(max-width: 499px)", false},
    {"(width: 500px)", true},
    {"(width: 501px)", false},
    {"(min-height: 500px)", true},
    {"(min-height: 501px)", false},
    {"(min-height: 500.02px)", false},
    {"(max-height: 500px)", true},
    {"(max-height: calc(500px))", true},
    {"(max-height: 499.98px)", false},
    {"(max-height: 499px)", false},
    {"(height: 500px)", true},
    {"(height: calc(500px))", true},
    {"(height: 500.001px)", true},
    {"(height: 499.999px)", true},
    {"(height: 500.02px)", false},
    {"(height: 499.98px)", false},
    {"(height: 501px)", false},
    {"(height)", true},
    {"(width)", true},
    {"(width: whatisthis)", false},
    {"screen and (min-width: 400px) and (max-width: 700px)", true},
    {"(max-aspect-ratio: 4294967296/1)", true},
    {"(max-aspect-ratio: calc(4294967296) / calc(1)", true},
    {"(min-aspect-ratio: 1/4294967295)", true},
};

constexpr EvalCase kFloatViewportTestCases[] = {
    {"all and (min-width: 600.5px)", true},
    {"(min-width: 600px)", true},
    {"(min-width: 600.5px)", true},
    {"(min-width: 601px)", false},
    {"(max-width: 600px)", false},
    {"(max-width: 600.5px)", true},
    {"(max-width: 601px)", true},
    {"(width: 600.5px)", true},
    {"(width: 601px)", false},
    {"(min-height: 700px)", true},
    {"(min-height: 700.125px)", true},
    {"(min-height: 701px)", false},
    {"(min-height: 700.141px)", false},
    {"(max-height: 701px)", true},
    {"(max-height: 700.125px)", true},
    {"(max-height: 700px)", false},
    {"(height: 700.125px)", true},
    {"(height: 700.141px)", false},
    {"(height: 700.109px)", false},
    {"(height: 701px)", false},
};

constexpr EvalCase kFloatNonFriendlyViewportTestCases[] = {
    {"(min-width: 821px)", true},  {"(max-width: 821px)", true},
    {"(width: 821px)", true},      {"(min-height: 821px)", true},
    {"(max-height: 821px)", true}, {"(height: 821px)", true},
    {"(width: 100vw)", true},      {"(height: 100vh)", true},
};

constexpr EvalCase kPrintTestCases[] = {
    {"print and (min-resolution: 1dppx)", true},
    {"print and (min-resolution: calc(100dpi - 4dpi))", true},
    {"print and (min-resolution: 118dpcm)", true},
    {"print and (min-resolution: 119dpcm)", false},
};

constexpr EvalCase kUpdateWithPrintDeviceTestCases[] = {
    {"(update)", false},       {"(update: none)", true},
    {"(update: slow)", false}, {"(update: fast)", false},
    {"update: fast", false},   {"(update: ?)", false},
};

constexpr EvalCase kUpdateWithSlowDeviceTestCases[] = {
    {"(update)", true},       {"(update: none)", false},
    {"(update: slow)", true}, {"(update: fast)", false},
    {"update: fast", false},  {"(update: ?)", false},
};

constexpr EvalCase kUpdateWithFastDeviceTestCases[] = {
    {"(update)", true},        {"(update: none)", false},
    {"(update: slow)", false}, {"(update: fast)", true},
    {"update: fast", false},   {"(update: ?)", false},
};

constexpr EvalCase kForcedcolorsActiveCases[] = {
    {"(forced-colors: active)", true},
    {"(forced-colors: none)", false},
};

constexpr EvalCase kForcedcolorsNoneCases[] = {
    {"(forced-colors: active)", false},
    {"(forced-colors: none)", true},
};

constexpr EvalCase kPreferscontrastNopreferenceCases[] = {
    {"(prefers-contrast)", false},
    {"(prefers-contrast: more)", false},
    {"(prefers-contrast: less)", false},
    {"(prefers-contrast: no-preference)", true},
    {"(prefers-contrast: custom)", false},
};

constexpr EvalCase kPreferscontrastMoreCases[] = {
    {"(prefers-contrast)", true},
    {"(prefers-contrast: more)", true},
    {"(prefers-contrast: less)", false},
    {"(prefers-contrast: no-preference)", false},
    {"(prefers-contrast: custom)", false},
};

constexpr EvalCase kPreferscontrastLessCases[] = {
    {"(prefers-contrast)", true},
    {"(prefers-contrast: more)", false},
    {"(prefers-contrast: less)", true},
    {"(prefers-contrast: no-preference)", false},
    {"(prefers-contrast: custom)", false},
};

constexpr EvalCase kPreferscontrastCustomCases[] = {
    {"(prefers-contrast)", true},
    {"(prefers-contrast: more)", false},
    {"(prefers-contrast: less)", false},
    {"(prefers-contrast: no-preference)", false},
    {"(prefers-contrast: custom)", true},
};

constexpr EvalCase kPrefersreducedtransparencyNopreferenceCases[] = {
    {"(prefers-reduced-transparency)", false},
    {"(prefers-reduced-transparency: reduce)", false},
    {"(prefers-reduced-transparency: no-preference)", true},
};

constexpr EvalCase kPrefersreducedtransparencyReduceCases[] = {
    {"(prefers-reduced-transparency)", true},
    {"(prefers-reduced-transparency: reduce)", true},
    {"(prefers-reduced-transparency: no-preference)", false},
};

constexpr EvalCase kNavigationcontrolsBackButtonCases[] = {
    {"(navigation-controls: back-button)", true},
    {"(navigation-controls: none)", false},
};

constexpr EvalCase kNavigationcontrolsNoneCases[] = {
    {"(navigation-controls: back-button)", false},
    {"(navigation-controls: none)", true},
};

constexpr EvalCase kSingleHorizontalViewportSegmentCases[] = {
    {"(horizontal-viewport-segments)", true},
    {"(horizontal-viewport-segments: 1)", true},
    {"(horizontal-viewport-segments > 1)", false},
    {"(horizontal-viewport-segments: 2)", false},
    {"(horizontal-viewport-segments: none)", false},
    {"(horizontal-viewport-segments: 1px)", false},
    {"(horizontal-viewport-segments: 16/9)", false},
};

constexpr EvalCase kDoubleHorizontalViewportSegmentCases[] = {
    {"(horizontal-viewport-segments)", true},
    {"(horizontal-viewport-segments: 1)", false},
    {"(horizontal-viewport-segments: 2)", true},
    {"(horizontal-viewport-segments: 3)", false},
};

constexpr EvalCase kSingleVerticalViewportSegmentCases[] = {
    {"(vertical-viewport-segments)", true},
    {"(vertical-viewport-segments: 1)", true},
    {"(vertical-viewport-segments: 2)", false},
    {"(vertical-viewport-segments: none)", false},
    {"(vertical-viewport-segments: 1px)", false},
    {"(vertical-viewport-segments: 16/9)", false},
};

constexpr EvalCase kDoubleVerticalViewportSegmentCases[] = {
    {"(vertical-viewport-segments)", true},
    {"(vertical-viewport-segments: 1)", false},
    {"(vertical-viewport-segments: 2)", true},
    {"(vertical-viewport-segments: 3)", false},
};

constexpr EvalCase kDevicePostureNoneCases[] = {
    {"(device-posture)", true},          {"(device-posture: continuous)", true},
    {"(device-posture: folded)", false}, {"(device-posture: 15)", false},
    {"(device-posture: 2px)", false},    {"(device-posture: 16/9)", false},
};

constexpr EvalCase kDevicePostureFoldedCases[] = {
    {"(device-posture)", true},
    {"(device-posture: continuous)", false},
    {"(device-posture: folded)", true},
};

constexpr EvalCase kDynamicRangeStandardCases[] = {
    {"(dynamic-range)", true},
    {"(dynamic-range), not all and (dynamic-range)", true},
    {"(dynamic-range: standard)", true},
    {"(dynamic-range: high)", false},
    {"(dynamic-range: invalid)", false},
};

constexpr EvalCase kDynamicRangeHighCases[] = {
    {"(dynamic-range)", true},
    {"(dynamic-range), not all and (dynamic-range)", true},
    {"(dynamic-range: standard)", true},
    {"(dynamic-range: high)", true},
    {"(dynamic-range: invalid)", false},
};

constexpr EvalCase kVideoDynamicRangeStandardCases[] = {
    {"(video-dynamic-range)", true},
    {"(video-dynamic-range), not all and (video-dynamic-range)", true},
    {"(video-dynamic-range: standard)", true},
    {"(video-dynamic-range: high)", false},
    {"(video-dynamic-range: invalid)", false},
};

constexpr EvalCase kVideoDynamicRangeHighCases[] = {
    {"(video-dynamic-range)", true},
    {"(video-dynamic-range), not all and (video-dynamic-range)", true},
    {"(video-dynamic-range: standard)", true},
    {"(video-dynamic-range: high)", true},
    {"(video-dynamic-range: invalid)", false},
};

constexpr EvalCase kVideoDynamicRangeFeatureDisabledCases[] = {
    {"(video-dynamic-range)", false},
    {"(video-dynamic-range), not all and (video-dynamic-range)", false},
    {"(video-dynamic-range: standard)", false},
    {"(video-dynamic-range: high)", false},
    {"(video-dynamic-range: invalid)", false},
};

constexpr EvalCase kOverflowWithPrintDeviceTestCases[] = {
    {"(overflow-inline)", false},        {"(overflow-block)", true},
    {"(overflow-inline: none)", true},   {"(overflow-block: none)", false},
    {"(overflow-block: paged)", true},   {"(overflow-inline: scroll)", false},
    {"(overflow-block: scroll)", false},
};

constexpr EvalCase kOverflowWithScrollableDeviceTestCases[] = {
    {"(overflow-inline)", true},        {"(overflow-block)", true},
    {"(overflow-inline: none)", false}, {"(overflow-block: none)", false},
    {"(overflow-block: paged)", false}, {"(overflow-inline: scroll)", true},
    {"(overflow-block: scroll)", true},
};

constexpr EvalCase kInvertedcolorsNoneCases[] = {
    {"(inverted-colors)", false},
    {"(inverted-colors: inverted)", false},
    {"(inverted-colors: none)", true},
};

constexpr EvalCase kInvertedcolorsInvertedCases[] = {
    {"(inverted-colors)", true},
    {"(inverted-colors: inverted)", true},
    {"(inverted-colors: none)", false},
};

constexpr EvalCase kScriptingNoneCases[] = {
    {"(scripting)", false},
    {"(scripting: none)", true},
    {"(scripting: initial-only)", false},
    {"(scripting: enabled)", false},
};

constexpr EvalCase kScriptingInitialOnlyCases[] = {
    {"(scripting)", false},
    {"(scripting: none)", false},
    {"(scripting: initial-only)", true},
    {"(scripting: enabled)", false},
};

constexpr EvalCase kScriptingEnabledCases[] = {
    {"(scripting)", true},
    {"(scripting: none)", false},
    {"(scripting: initial-only)", false},
    {"(scripting: enabled)", true},
};

constexpr EvalCase kFloatCastOverflowCases[] = {
    {"(color: 3.0e38)", false},
    {"(color-index: 3.0e38)", false},
    {"(monochrome: 3.0e38)", false},
    {"(grid: 3.0e38)", false},
    {"(-webkit-transform-3d: 3.0e38)", false},
    {"(horizontal-viewport-segments: 3.0e38)", false},
    {"(vertical-viewport-segments: 3.0e38)", false},
};

constexpr EvalCase kEvalMediaQueries4Cases[] = {
    {"(width: 1px) or (width: 2px)", false},
    {"(width: 1px) or (width: 2px) or (width: 3px)", false},
    {"(width: 500px) or (width: 2px) or (width: 3px)", true},
    {"(width: 1px) or (width: 500px) or (width: 3px)", true},
    {"(width: 1px) or (width: 2px) or (width: 500px)", true},
    {"((width: 1px))", false},
    {"((width: 500px))", true},
    {"(((width: 500px)))", true},
    {"((width: 1px) or (width: 2px)) or (width: 3px)", false},
    {"(width: 1px) or ((width: 2px) or (width: 500px))", true},
    {"(width = 500px)", true},
    {"(width >= 500px)", true},
    {"(width <= 500px)", true},
    {"(width < 500px)", false},
    {"(500px = width)", true},
    {"(500px >= width)", true},
    {"(500px <= width)", true},
    {"(499px < width)", true},
    {"(499px > width)", false},
    {"(499px < width < 501px)", true},
    {"(499px < width <= 500px)", true},
    {"(499px < width < 500px)", false},
    {"(500px < width < 501px)", false},
    {"(501px > width > 499px)", true},
    {"(500px >= width > 499px)", true},
    {"(501px > width >= 500px)", true},
    {"(502px > width >= 501px)", false},
    {"not (499px > width)", true},
    {"(not (499px > width))", true},
    {"(width >= 500px) and (not (499px > width))", true},
    {"(width >= 500px) and ((499px > width) or (not (width = 500px)))", false},
};

constexpr EvalCase kEvalGeneralEnclosedCases[] = {
    {"(unknown)", false},
    {"((unknown: 1px))", false},
    {"not (unknown: 1px)", false},
    {"(width) or (unknown: 1px)", true},
    {"(unknown: 1px) or (width)", true},
    {"(width: 42px) or (unknown: 1px)", false},
    {"(unknown: 1px) or (width: 42px)", false},
    {"not ((width: 42px) or (unknown: 1px))", false},
    {"not ((unknown: 1px) or (width: 42px))", false},
    {"not ((width) or (unknown: 1px))", false},
    {"not ((unknown: 1px) or (width))", false},
    {"(width) and (unknown: 1px)", false},
    {"(unknown: 1px) and (width)", false},
    {"(width: 42px) and (unknown: 1px)", false},
    {"(unknown: 1px) and (width: 42px)", false},
    {"not ((width: 42px) and (unknown: 1px))", true},
    {"not ((unknown: 1px) and (width: 42px))", true},
    {"not ((width) and (unknown: 1px))", false},
    {"not ((unknown: 1px) and (width))", false},
};

int g_cases = 0;
int g_failures = 0;

void Report(const std::string& message) {
  fprintf(stdout, "%s", message.c_str());
}

MediaQueryFeatures TestFeatures() {
  MediaQueryFeatures features;
  features.forced_colors = true;
  features.inverted_colors = true;
  features.prefers_reduced_data = true;
  features.video_dynamic_range = true;
  features.navigation_controls = true;
  features.viewport_segments = true;
  features.device_posture = true;
  features.additional_windowing_controls = true;
  features.scrolled_container_queries = true;
  return features;
}

std::unique_ptr<MediaQuerySet> Parse(std::string_view input,
                                     const MediaQueryFeatures& features) {
  return ParseMediaQuerySet(base::UTF8ToUTF16(input), features);
}

// As TestMediaQuery() in media_query_set_test.cc: each query written back,
// or `unknown_substitute` for one with a <general-enclosed> in it.
void TestMediaQuery(const char* test_name,
                    const char* input,
                    const char* output,
                    const char* unknown_substitute = nullptr) {
  ++g_cases;
  const std::unique_ptr<MediaQuerySet> query_set = Parse(input, TestFeatures());
  std::u16string actual;
  bool first = true;
  for (const std::unique_ptr<MediaQuery>& query : query_set->Queries()) {
    if (!first) {
      actual += u", ";
    }
    first = false;
    if (unknown_substitute && query->ExpNode() &&
        query->ExpNode()->ContainsUnknown()) {
      actual += base::UTF8ToUTF16(unknown_substitute);
    } else {
      actual += query->CssText();
    }
  }
  const std::string expected = output ? output : input;
  if (base::UTF16ToUTF8(actual) != expected) {
    ++g_failures;
    Report(base::StrCat({"FAIL ", test_name, ": \"", input, "\"\n  expected \"",
                         expected, "\"\n  got      \"",
                         base::UTF16ToUTF8(actual), "\"\n"}));
  }
}

// As TestMQEvaluator() in media_query_evaluator_test.cc.
void TestMQEvaluator(const char* test_name,
                     base::span<const EvalCase> test_cases,
                     const MediaValues& media_values,
                     const MediaQueryFeatures& features = TestFeatures()) {
  const MediaQueryEvaluator evaluator(media_values);
  for (const EvalCase& test_case : test_cases) {
    ++g_cases;
    const std::unique_ptr<MediaQuerySet> query_set =
        Parse(test_case.input, features);
    const bool result = evaluator.Eval(*query_set);
    if (result != test_case.output) {
      ++g_failures;
      Report(
          base::StrCat({"FAIL ", test_name, ": \"", test_case.input,
                        "\"\n  expected ", test_case.output ? "true" : "false",
                        ", got ", result ? "true" : "false", "\n"}));
    }
  }
}

void MediaQuerySetTests() {
  for (const SetCase& test : kSetBasicCases) {
    // Written for mediaqueries-3, which knows no <general-enclosed>.
    TestMediaQuery("MediaQuerySetTest.Basic", test.input, test.output,
                   "not all");
  }
  for (const SetCase& test : kSetMediaQueries4Cases) {
    TestMediaQuery("MediaQuerySetTest.CSSMediaQueries4", test.input,
                   test.output, "<unknown>");
  }
  for (const char* input : kGeneralEnclosedUnknownCases) {
    TestMediaQuery("MediaQuerySetTest.GeneralEnclosed", input, input);
    TestMediaQuery("MediaQuerySetTest.GeneralEnclosed", input, "<unknown>",
                   "<unknown>");
  }
  for (const char* input : kGeneralEnclosedInvalidCases) {
    TestMediaQuery("MediaQuerySetTest.GeneralEnclosed", input, "not all");
  }
}

void CachedTests() {
  MediaValues data;
  data.viewport_width = 500;
  data.viewport_height = 500;
  data.device_width = 500;
  data.device_height = 500;
  data.device_pixel_ratio = 2.0;
  data.color_bits_per_component = 24;
  data.monochrome_bits_per_component = 0;
  data.primary_pointer_type = kPointerTypeFine;
  data.primary_hover_type = kHoverTypeHover;
  data.update = MediaUpdate::kFast;
  data.three_d_enabled = true;
  data.media_type = "screen";
  data.strict_mode = true;
  data.display_mode = MediaDisplayMode::kBrowser;

  // Default values.
  TestMQEvaluator("Cached", kScreenTestCases, data);
  TestMQEvaluator("Cached", kViewportTestCases, data);

  // Default display-state values.
  data.window_show_state = MediaWindowShowState::kDefault;
  TestMQEvaluator("Cached", kDisplayStateTestCases, data);

  // Default resizable values.
  TestMQEvaluator("Cached", kResizableTestCases, data);

  // Non-resizable resizable values.
  data.resizable = false;
  TestMQEvaluator("Cached", kNonResizableTestCases, data);
  data.resizable = true;

  // Print values.
  data.media_type = "print";
  TestMQEvaluator("Cached", kPrintTestCases, data);
  data.media_type = "screen";

  // Update values with print device.
  data.media_type = "print";
  TestMQEvaluator("Cached", kUpdateWithPrintDeviceTestCases, data);
  data.media_type = "screen";

  // Update values with slow device.
  data.update = MediaUpdate::kSlow;
  TestMQEvaluator("Cached", kUpdateWithSlowDeviceTestCases, data);

  // Update values with fast device.
  data.update = MediaUpdate::kFast;
  TestMQEvaluator("Cached", kUpdateWithFastDeviceTestCases, data);

  // Monochrome values.
  data.color_bits_per_component = 0;
  data.monochrome_bits_per_component = 8;
  TestMQEvaluator("Cached", kMonochromeTestCases, data);
  data.color_bits_per_component = 24;
  data.monochrome_bits_per_component = 0;

  // Overflow values with printing.
  data.media_type = "print";
  TestMQEvaluator("Cached", kOverflowWithPrintDeviceTestCases, data);
  data.media_type = "screen";

  // Overflow values with scrolling.
  TestMQEvaluator("Cached", kOverflowWithScrollableDeviceTestCases, data);
}

void CachedFloatViewportTests() {
  MediaValues data;
  data.viewport_width = 600.5;
  data.viewport_height = 700.125;
  TestMQEvaluator("CachedFloatViewport", kFloatViewportTestCases, data);

  MediaValues friendly;
  friendly.viewport_width = 821;
  friendly.viewport_height = 821;
  TestMQEvaluator("CachedFloatViewportNonFloatFriendly",
                  kFloatNonFriendlyViewportTestCases, friendly);
}

void CachedPreferenceTests() {
  MediaValues data;
  data.forced_colors = false;
  TestMQEvaluator("CachedForcedColors", kForcedcolorsNoneCases, data);
  data.forced_colors = true;
  TestMQEvaluator("CachedForcedColors", kForcedcolorsActiveCases, data);

  data.forced_colors = false;
  data.preferred_contrast = MediaContrast::kNoPreference;
  TestMQEvaluator("CachedPrefersContrast", kPreferscontrastNopreferenceCases,
                  data);
  data.preferred_contrast = MediaContrast::kMore;
  TestMQEvaluator("CachedPrefersContrast", kPreferscontrastMoreCases, data);
  data.preferred_contrast = MediaContrast::kLess;
  TestMQEvaluator("CachedPrefersContrast", kPreferscontrastLessCases, data);
  data.preferred_contrast = MediaContrast::kCustom;
  TestMQEvaluator("CachedPrefersContrast", kPreferscontrastCustomCases, data);

  MediaValues transparency;
  transparency.prefers_reduced_transparency = false;
  TestMQEvaluator("CachedPrefersReducedTransparency",
                  kPrefersreducedtransparencyNopreferenceCases, transparency);
  transparency.prefers_reduced_transparency = true;
  TestMQEvaluator("CachedPrefersReducedTransparency",
                  kPrefersreducedtransparencyReduceCases, transparency);

  MediaValues inverted;
  inverted.inverted_colors = false;
  TestMQEvaluator("CachedInvertedColors", kInvertedcolorsNoneCases, inverted);
  inverted.inverted_colors = true;
  TestMQEvaluator("CachedInvertedColors", kInvertedcolorsInvertedCases,
                  inverted);

  MediaValues scripting;
  scripting.scripting = MediaScripting::kNone;
  TestMQEvaluator("CachedScripting", kScriptingNoneCases, scripting);
  scripting.scripting = MediaScripting::kInitialOnly;
  TestMQEvaluator("CachedScripting", kScriptingInitialOnlyCases, scripting);
  scripting.scripting = MediaScripting::kEnabled;
  TestMQEvaluator("CachedScripting", kScriptingEnabledCases, scripting);

  // The navigation controls of the Blink test that has no TEST of its own.
  MediaValues navigation;
  navigation.navigation_controls_back_button = true;
  TestMQEvaluator("NavigationControls", kNavigationcontrolsBackButtonCases,
                  navigation);
  navigation.navigation_controls_back_button = false;
  TestMQEvaluator("NavigationControls", kNavigationcontrolsNoneCases,
                  navigation);
}

void CachedViewportSegmentsTests() {
  MediaValues data;
  data.horizontal_viewport_segments = 1;
  TestMQEvaluator("CachedViewportSegments",
                  kSingleHorizontalViewportSegmentCases, data);
  data.horizontal_viewport_segments = 2;
  TestMQEvaluator("CachedViewportSegments",
                  kDoubleHorizontalViewportSegmentCases, data);
  data.vertical_viewport_segments = 1;
  TestMQEvaluator("CachedViewportSegments", kSingleVerticalViewportSegmentCases,
                  data);
  data.vertical_viewport_segments = 2;
  TestMQEvaluator("CachedViewportSegments", kDoubleVerticalViewportSegmentCases,
                  data);
}

void CachedDevicePostureTests() {
  MediaValues data;
  data.device_posture = MediaDevicePosture::kContinuous;
  TestMQEvaluator("CachedDevicePosture", kDevicePostureNoneCases, data);
  data.device_posture = MediaDevicePosture::kFolded;
  TestMQEvaluator("CachedDevicePosture", kDevicePostureFoldedCases, data);
}

void CachedDynamicRangeTests() {
  MediaQueryFeatures without_video = TestFeatures();
  without_video.video_dynamic_range = false;
  // sRGB and Display P3 are standard dynamic range; extended sRGB, linear
  // sRGB, HDR10 and HLG high.
  for (const bool hdr : {false, true}) {
    MediaValues data;
    data.device_supports_hdr = hdr;
    if (hdr) {
      TestMQEvaluator("CachedDynamicRange", kDynamicRangeHighCases, data);
      TestMQEvaluator("CachedDynamicRange", kVideoDynamicRangeHighCases, data);
    } else {
      TestMQEvaluator("CachedDynamicRange", kDynamicRangeStandardCases, data);
      TestMQEvaluator("CachedDynamicRange", kVideoDynamicRangeStandardCases,
                      data);
    }
    TestMQEvaluator("CachedDynamicRange",
                    kVideoDynamicRangeFeatureDisabledCases, data,
                    without_video);
  }
}

void FloatCastOverflowTests() {
  MediaValues data;
  TestMQEvaluator("FloatCastOverflow", kFloatCastOverflowCases, data);
}

// Blink builds these expressions by hand; they are the same queries parsed.
void RangedValuesTests() {
  constexpr EvalCase kCases[] = {
      {"(width < 600px)", true},          {"(width < 501px)", true},
      {"(width < 500px)", false},         {"(width > 500px)", false},
      {"(width <= 500px)", true},         {"(400px < width)", true},
      {"(600px < width)", false},         {"(400px > width)", false},
      {"(600px > width)", true},          {"(400px <= width)", true},
      {"(600px <= width)", false},        {"(400px >= width)", false},
      {"(600px >= width)", true},         {"(width = 500px)", true},
      {"(width = 400px)", false},         {"(500px = width)", true},
      {"(400px = width)", false},         {"(400px < width < 600px)", true},
      {"(550px < width < 600px)", false}, {"(400px < width < 450px)", false},
      {"(aspect-ratio = 2/1)", true},     {"(aspect-ratio = 3/1)", false},
      {"(aspect-ratio < 1/1)", false},    {"(aspect-ratio < 3/1)", true},
      {"(aspect-ratio > 1/1)", true},     {"(aspect-ratio > 3/1)", false},
  };
  MediaValues data;
  data.viewport_width = 500;
  data.viewport_height = 250;
  TestMQEvaluator("RangedValues", kCases, data);
}

// Blink builds these nodes by hand; they are the same conditions parsed.
void ExpNodeTests() {
  constexpr EvalCase kCases[] = {
      {"(width < 600px)", true},
      {"(width < 400px)", false},
      {"((width < 600px))", true},
      {"((width < 400px))", false},
      {"not (width < 600px)", false},
      {"not (width < 400px)", true},
      {"(width < 600px) and (width < 800px)", true},
      {"(width < 600px) and (width < 400px)", false},
      {"(width < 600px) or (width < 400px)", true},
      {"(width < 400px) or (not (width < 800px))", false},
  };
  MediaValues data;
  data.viewport_width = 500;
  TestMQEvaluator("ExpNode", kCases, data);
}

void MediaQueries4AndGeneralEnclosedTests() {
  MediaValues data;
  data.viewport_width = 500;
  data.viewport_height = 500;
  TestMQEvaluator("CSSMediaQueries4", kEvalMediaQueries4Cases, data);
  TestMQEvaluator("GeneralEnclosed", kEvalGeneralEnclosedCases, data);
}

}  // namespace
}  // namespace mitto

int main() {
  mitto::MediaQuerySetTests();
  mitto::CachedTests();
  mitto::CachedFloatViewportTests();
  mitto::CachedPreferenceTests();
  mitto::CachedViewportSegmentsTests();
  mitto::CachedDevicePostureTests();
  mitto::CachedDynamicRangeTests();
  mitto::FloatCastOverflowTests();
  mitto::RangedValuesTests();
  mitto::ExpNodeTests();
  mitto::MediaQueries4AndGeneralEnclosedTests();
  if (mitto::g_failures) {
    mitto::Report(base::StrCat({base::NumberToString(mitto::g_failures), " of ",
                                base::NumberToString(mitto::g_cases),
                                " cases failed\n"}));
    return 1;
  }
  mitto::Report(base::StrCat(
      {"all ", base::NumberToString(mitto::g_cases), " cases passed\n"}));
  return 0;
}
