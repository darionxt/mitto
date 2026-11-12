// Media Queries' evaluation, as Blink's MediaQueryEvaluator works out each
// media feature from MediaValues:
// https://drafts.csswg.org/mediaqueries-5/#media-descriptor-table
//
// Upstream: third_party/blink/renderer/core/css/media_query_evaluator.cc

#ifndef MITTO_CSS_MEDIA_QUERY_EVALUATOR_H_
#define MITTO_CSS_MEDIA_QUERY_EVALUATOR_H_

#include "base/memory/raw_ref.h"
#include "mitto/css/media_query.h"
#include "mitto/css/media_values.h"

namespace mitto {

class MediaQueryEvaluator {
 public:
  explicit MediaQueryEvaluator(const MediaValues& media_values);
  MediaQueryEvaluator(const MediaQueryEvaluator&) = delete;
  MediaQueryEvaluator& operator=(const MediaQueryEvaluator&) = delete;
  ~MediaQueryEvaluator();

  // Whether any query of the list matches, and true for an empty list.
  bool Eval(const MediaQuerySet& query_set) const;
  KleeneValue Eval(const MediaQuery& query) const;
  KleeneValue Eval(const ConditionalExpNode& node) const;
  KleeneValue EvalFeature(const MediaQueryExp& exp) const;

 private:
  bool MediaTypeMatch(const std::u16string& media_type) const;

  const raw_ref<const MediaValues> media_values_;
};

}  // namespace mitto

#endif  // MITTO_CSS_MEDIA_QUERY_EVALUATOR_H_
