# html5lib's tree construction tests

The other files here are html5lib's tree construction tests, with their README, as the Engine Release's tree holds them, unchanged, beside the tokenizer tests in [`../html5lib_tokenizer`](../html5lib_tokenizer), from the same copy of html5lib-python 1.1. They are under the MIT licence in `LICENSE`, and `AUTHORS.rst` names their authors. The `scripted` directory, whose tests need a page to run scripts while it parses, is left out.

They are here because CI has no checkout, and the source export holds only what a build reads. `html_tree_builder_test` runs them against the tree builder in `native/dom`, and [`../html5lib_tree_failures.txt`](../html5lib_tree_failures.txt) lists each one it fails without skipping a rule, with why. Each release run copies them again from the new Engine Release's tree ([releasing.md](../../../docs/releasing.md)).

Upstream: third_party/catapult/third_party/html5lib-1.1/html5lib/tests/testdata/tree-construction
