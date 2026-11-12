#include "mitto/css/css_token_stream.h"

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/css/css_tokenizer.h"

namespace mitto {

CssTokenStream::BlockGuard::BlockGuard(CssTokenStream& stream)
    : stream_(stream) {
  DCHECK_EQ(stream.Peek().block_type, CssBlockType::kBlockStart);
  stream.ConsumeAny();
}

CssTokenStream::BlockGuard::~BlockGuard() {
  stream_->SkipToEndOfBlock();
}

CssTokenStream::RestoringBlockGuard::RestoringBlockGuard(CssTokenStream& stream)
    : stream_(stream), state_(stream.Save()) {
  DCHECK_EQ(stream.Peek().block_type, CssBlockType::kBlockStart);
  stream.ConsumeAny();
}

CssTokenStream::RestoringBlockGuard::~RestoringBlockGuard() {
  if (!released_) {
    stream_->Restore(state_);
  }
}

bool CssTokenStream::RestoringBlockGuard::Release() {
  DCHECK(!released_);
  const CssToken& next = stream_->Peek();
  if (next.type == CssTokenType::kEndOfFile ||
      next.block_type == CssBlockType::kBlockEnd) {
    stream_->ConsumeAny();
    released_ = true;
    return true;
  }
  return false;
}

CssTokenStream::CssTokenStream(std::u16string_view input)
    : input_(input), tokens_(TokenizeCss(input)) {}

CssTokenStream::~CssTokenStream() = default;

bool CssTokenStream::AtEnd() const {
  const CssToken& next = Peek();
  return next.type == CssTokenType::kEndOfFile ||
         next.block_type == CssBlockType::kBlockEnd;
}

const CssToken& CssTokenStream::Consume() {
  DCHECK_NE(Peek().block_type, CssBlockType::kBlockStart);
  DCHECK_NE(Peek().block_type, CssBlockType::kBlockEnd);
  const size_t index = index_;
  ConsumeAny();
  return tokens_[index];
}

void CssTokenStream::ConsumeWhitespace() {
  while (Peek().type == CssTokenType::kWhitespace) {
    ConsumeAny();
  }
}

CssToken CssTokenStream::ConsumeIncludingWhitespace() {
  CssToken result = Consume();
  ConsumeWhitespace();
  return result;
}

void CssTokenStream::Restore(State state) {
  index_ = state.index;
  offset_ = state.offset;
}

void CssTokenStream::ConsumeAny() {
  const CssToken& token = tokens_[index_];
  offset_ = token.end;
  // The end of the input stays next, as a tokenizer at its end gives it
  // again.
  if (token.type != CssTokenType::kEndOfFile) {
    ++index_;
  }
}

void CssTokenStream::SkipToEndOfBlock() {
  size_t nesting_level = 1;
  while (true) {
    const CssToken& token = tokens_[index_];
    if (token.type == CssTokenType::kEndOfFile) {
      offset_ = token.end;
      return;
    }
    if (token.block_type == CssBlockType::kBlockStart) {
      ++nesting_level;
    } else if (token.block_type == CssBlockType::kBlockEnd) {
      --nesting_level;
    }
    ConsumeAny();
    if (nesting_level == 0) {
      return;
    }
  }
}

}  // namespace mitto
