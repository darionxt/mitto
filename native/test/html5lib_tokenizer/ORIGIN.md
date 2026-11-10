# html5lib's tokenizer tests

The other files here are html5lib's tokenizer tests, with their README, as the Engine Release's tree holds them, unchanged. They are under the MIT licence in `LICENSE`, and `AUTHORS.rst` names their authors. The tree has them twice, from html5lib-python 1.1: in `third_party/catapult` and, the same, in `third_party/webgl`. It has no `third_party/html5lib-tests`, and its WPT copy has html5lib's tree construction tests only.

They are here because CI has no checkout, and the source export holds only what a build reads. `html_tokenizer_test` runs them against the HTML tokenizer, and [`../html5lib_tokenizer_failures.txt`](../html5lib_tokenizer_failures.txt) lists each one it fails, with why. Each release run copies them again from the new Engine Release's tree ([releasing.md](../../../docs/releasing.md)).

Upstream: third_party/catapult/third_party/html5lib-1.1/html5lib/tests/testdata/tokenizer
