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
    // The body: bytes of its own, or `blob_length` bytes of `blob` from
    // `blob_offset`, which are copied a piece at a time as they go.
    std::string body;
    scoped_refptr<BlobDataHandle> blob;
    size_t blob_offset = 0;
    size_t blob_length = 0;

    // All of the body at once, for a worker's script, which reads it whole.
    std::string TakeBody();
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

  // FetchController: taking a blob's bytes makes room in the pipe for more.
  void TookBody(size_t bytes) override;
  void Cancel() override;

 private:
  LocalSchemeFetch(Answer answer,
                   std::unique_ptr<FetchClient> client,
                   ExecutionContext& context);

  // The context's going, which cancels the fetch, once any call to the
  // client has returned.
  void OnContextDestroyed();
  // After a call to the client: whether the fetch goes on, or the context
  // went under the call, which cancelled it.
  bool ReturnedFromClient();
  // Posts `step` to run from a later task of the context.
  void Post(void (LocalSchemeFetch::*step)());
  // The browser's answer, from the task after this one's.
  void PostRespond();
  void Respond();
  void SendResponseBody();
  size_t BodySize() const {
    return answer_.blob ? answer_.blob_length : answer_.body.size();
  }

  Answer answer_;
  std::unique_ptr<FetchClient> client_;
  cppgc::WeakPersistent<ExecutionContext> context_;
  uint64_t context_destroyed_callback_ = 0;
  size_t body_sent_ = 0;
  // The bytes of a blob that the client has been given and not taken, at most
  // the pipe's capacity, and whether a piece waits for room.
  size_t in_pipe_ = 0;
  bool waiting_for_room_ = false;
  // Whether the client is being called, and whether the context went under
  // the call.
  bool calling_client_ = false;
  bool context_gone_ = false;
  base::WeakPtrFactory<LocalSchemeFetch> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCAL_SCHEME_FETCH_H_
