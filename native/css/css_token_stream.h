// A stream of CSS tokens with Blink's CSSParserTokenStream's rules: a block
// is entered only through a guard, which consumes the whole block when it
// goes, the end of the current block reads as the end of the stream, and the
// stream can be saved and restored within a block.
//
// Blink's stream tokenizes as it goes; this one reads the tokens that
// TokenizeCss() made, which gives a parser the same tokens and the same
// offsets into the input.
//
// Upstream: third_party/blink/renderer/core/css/parser/css_parser_token_stream.h
// Upstream: third_party/blink/renderer/core/css/parser/css_parser_token_stream.cc

#ifndef MITTO_CSS_CSS_TOKEN_STREAM_H_
#define MITTO_CSS_CSS_TOKEN_STREAM_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ref.h"
#include "mitto/css/css_token.h"

namespace mitto {

class CssTokenStream {
 public:
  // Where the stream is, to go back to with Restore().
  struct State {
    size_t index;
    size_t offset;
  };

  // Enters the block that starts at the next token, and when it goes,
  // consumes the rest of the block and its end.
  class BlockGuard {
   public:
    explicit BlockGuard(CssTokenStream& stream);
    BlockGuard(const BlockGuard&) = delete;
    BlockGuard& operator=(const BlockGuard&) = delete;
    ~BlockGuard();

   private:
    const raw_ref<CssTokenStream> stream_;
  };

  // Enters the block that starts at the next token, and when it goes, puts
  // the stream back where it was before it, unless Release() succeeded.
  class RestoringBlockGuard {
   public:
    explicit RestoringBlockGuard(CssTokenStream& stream);
    RestoringBlockGuard(const RestoringBlockGuard&) = delete;
    RestoringBlockGuard& operator=(const RestoringBlockGuard&) = delete;
    ~RestoringBlockGuard();

    // At the end of the block, consumes its end and returns true; elsewhere
    // returns false, and the guard still restores the stream.
    bool Release();

   private:
    const raw_ref<CssTokenStream> stream_;
    const State state_;
    bool released_ = false;
  };

  explicit CssTokenStream(std::u16string_view input);
  CssTokenStream(const CssTokenStream&) = delete;
  CssTokenStream& operator=(const CssTokenStream&) = delete;
  ~CssTokenStream();

  const CssToken& Peek() const { return tokens_[index_]; }
  // At the end of the input, or of the block that the stream is in.
  bool AtEnd() const;
  // Consumes the next token, which must not start or end a block.
  const CssToken& Consume();
  void ConsumeWhitespace();
  CssToken ConsumeIncludingWhitespace();
  // Skips tokens, and whole blocks, until the next is of one of `types`
  // outside any block, or the current block or the input ends.
  template <typename... Types>
  void SkipUntilPeekedTypeIs(Types... types) {
    SkipUntil([&](CssTokenType type) { return ((type == types) || ...); });
  }

  // Where in the input the next character to consume is: the end of the last
  // token consumed, which comes before any comment that follows it.
  size_t Offset() const { return offset_; }
  // Where the next token starts.
  size_t LookAheadOffset() const { return Peek().start; }
  std::u16string_view StringRangeAt(size_t start, size_t length) const {
    return input_.substr(start, length);
  }

  State Save() const { return {index_, offset_}; }
  void Restore(State state);

 private:
  template <typename Predicate>
  void SkipUntil(Predicate stop) {
    if (Peek().type == CssTokenType::kEndOfFile || stop(Peek().type) ||
        Peek().block_type == CssBlockType::kBlockEnd) {
      return;
    }
    size_t nesting_level = 0;
    while (true) {
      const CssToken& token = tokens_[index_];
      if (token.type == CssTokenType::kEndOfFile ||
          (nesting_level == 0 &&
           (stop(token.type) || token.block_type == CssBlockType::kBlockEnd))) {
        offset_ = token.start;
        return;
      }
      if (token.block_type == CssBlockType::kBlockStart) {
        ++nesting_level;
      } else if (token.block_type == CssBlockType::kBlockEnd) {
        --nesting_level;
      }
      ++index_;
    }
  }

  // Consumes the token at index_, block starts and ends included.
  void ConsumeAny();
  // Consumes the rest of the block the stream is in, and its end.
  void SkipToEndOfBlock();

  const std::u16string_view input_;
  const std::vector<CssToken> tokens_;
  size_t index_ = 0;
  size_t offset_ = 0;
};

}  // namespace mitto

#endif  // MITTO_CSS_CSS_TOKEN_STREAM_H_
