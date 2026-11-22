// A fetch() that Chrome answers without its network service: a data: URL,
// which Blink's ResourceLoader answers in the renderer, from a task of its
// own, and a blob: URL, which the browser's BlobURLLoader answers from the
// blob's bytes, after a hop to the browser and back, a data pipe's worth at a
// time. A Page holds the blobs itself, so it posts the same tasks. Neither
// adds an entry to the Performance Timeline, as in Chrome.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_loader.cc
// Upstream: storage/browser/blob/blob_url_loader.cc
// Upstream: storage/browser/blob/mojo_blob_reader.cc

#ifndef MITTO_DOM_LOCAL_SCHEME_FETCH_H_
#define MITTO_DOM_LOCAL_SCHEME_FETCH_H_

#include <stddef.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/fetcher.h"
#include "v8/include/cppgc/persistent.h"

class GURL;

namespace mitto {

class ExecutionContext;

class LocalSchemeFetch final : public FetchController {
 public:
  // What answers the fetch: a response and its body, or a net error.
  struct Answer {
    Answer();
    Answer(Answer&&);
    Answer& operator=(Answer&&);
    ~Answer();

    // 0 for a response, and otherwise why it fails.
    int net_error = 0;
    FetchResponseHead head;
    std::string body;
    // Whether the browser answers it, after a hop there and back, in pieces
    // of at most `pipe_capacity` bytes, each from a task of its own.
    bool from_browser = false;
    size_t pipe_capacity = 0;
  };

  // What Blink's data: URL fetch gives for `url` and `method`: a response of
  // the URL's bytes and type, with no body for HEAD, or ERR_INVALID_URL for a
  // URL that does not parse.
  static Answer AnswerDataUrl(const GURL& url, std::string_view method);
  // What the browser's BlobURLLoader gives for `url`, which names `blob`, or
  // nothing that the Request found when it was made: ERR_FILE_NOT_FOUND for
  // no blob, ERR_METHOD_NOT_SUPPORTED for any method but GET, and for a Range
  // header, `range`, the bytes that it names with a 206 and Content-Range,
  // or ERR_REQUEST_RANGE_NOT_SATISFIABLE for one that is not one range in
  // the Fetch Standard's grammar or fits nothing.
  static Answer AnswerBlobUrl(const GURL& url,
                              scoped_refptr<BlobDataHandle> blob,
                              std::string_view method,
                              const std::optional<std::string>& range);

  // ERR_FAILED, as a blob: URL of another origin gives.
  static Answer AnswerRefusedBlobUrl();

  // Answers `client` with `answer`, from tasks of `context`. It owns itself
  // until the client's end, or until the context goes, which destroys the
  // client with no word, as Blink's FetchManager disposes its loaders.
  static void Start(Answer answer,
                    std::unique_ptr<FetchClient> client,
                    ExecutionContext& context);

  LocalSchemeFetch(const LocalSchemeFetch&) = delete;
  LocalSchemeFetch& operator=(const LocalSchemeFetch&) = delete;
  ~LocalSchemeFetch() override;

  // FetchController: the bytes are all here, so taking them lets nothing
  // more come.
  void TookBody(size_t bytes) override {}
  void Cancel() override;

 private:
  LocalSchemeFetch(Answer answer,
                   std::unique_ptr<FetchClient> client,
                   ExecutionContext& context);

  // Posts `step` to run from a later task of the context.
  void Post(void (LocalSchemeFetch::*step)());
  // The browser's answer, from the task after this one's.
  void PostRespond();
  void Respond();
  void SendResponseBody();

  Answer answer_;
  std::unique_ptr<FetchClient> client_;
  cppgc::WeakPersistent<ExecutionContext> context_;
  size_t body_sent_ = 0;
  base::WeakPtrFactory<LocalSchemeFetch> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCAL_SCHEME_FETCH_H_
