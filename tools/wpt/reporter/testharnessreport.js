// mitto's report of a test's results, in place of the browser's: once the tests have completed, it posts each
// subtest's result and the harness status to /mitto/collect.py, which keeps them for the runner. See docs/wpt.md.
(function () {
  // The address that the test started at, before it can push state or change its fragment.
  var startUrl = location.href;
  setup({ output: false, explicit_timeout: true });
  add_completion_callback(function (tests, status) {
    var record = {
      url: startUrl,
      harness: { status: status.status, message: status.message },
      tests: tests.map(function (test) {
        return { name: test.name, status: test.status, message: test.message };
      }),
      visibility: typeof document === "undefined" ? null : document.visibilityState,
    };
    var text = JSON.stringify(record);
    // For the runner to read through the debugging protocol or evaluate() at the deadline, when a page's Content
    // Security Policy refuses the post.
    self.__mittoResults = text;
    fetch("/mitto/collect.py", { method: "POST", body: text }).catch(function () {});
  });
})();
