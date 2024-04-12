#ifndef AUTOFDOLLVM_PROPELLER_CFG_MATCHERS_H_
#define AUTOFDOLLVM_PROPELLER_CFG_MATCHERS_H_
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "third_party/abseil/absl/types/span.h"

namespace devtools_crosstool_autofdo {

// Returns a vector of Pointee matchers corresponding to each matcher in
// `matchers`.
template <typename T>
std::vector<testing::Matcher<std::unique_ptr<T>>> GetPointeeMatchers(
    absl::Span<const testing::Matcher<T>> matchers) {
  std::vector<testing::Matcher<std::unique_ptr<T>>> pointee_matchers;
  for (const auto& matcher : matchers) {
    pointee_matchers.push_back(testing::Pointee(matcher));
  }
  return pointee_matchers;
}

MATCHER_P(NodeIntraIdIs, intra_cfg_id_matcher,
          "has intra_cfg_id that " +
              testing::DescribeMatcher<CFGNode::IntraCfgId>(
                  intra_cfg_id_matcher, negation)) {
  return ExplainMatchResult(
      testing::Property("intra_cfg_id", &CFGNode::intra_cfg_id,
                        intra_cfg_id_matcher),
      arg, result_listener);
}

MATCHER_P(NodeInterIdIs, inter_cfg_id_matcher,
          "has intra_cfg_id that " +
              testing::DescribeMatcher<CFGNode::InterCfgId>(
                  inter_cfg_id_matcher, negation)) {
  return ExplainMatchResult(
      testing::Property("inter_cfg_id", &CFGNode::inter_cfg_id,
                        inter_cfg_id_matcher),
      arg, result_listener);
}

MATCHER_P(NodeIndexIs, bb_index_matcher,
          "has bb_index that " +
              testing::DescribeMatcher<int>(bb_index_matcher, negation)) {
  return testing::ExplainMatchResult(
      testing::Property("bb_index", &CFGNode::bb_index, bb_index_matcher), arg,
      result_listener);
}

MATCHER_P(NodeFreqIs, freq_matcher,
          "has frequency that " +
              testing::DescribeMatcher<int>(freq_matcher, negation)) {
  return testing::ExplainMatchResult(
      testing::Property("frequency", &CFGNode::CalculateFrequency,
                        freq_matcher),
      arg, result_listener);
}

MATCHER_P4(IsCfgEdge, src_matcher, sink_matcher, weight_matcher, kind_matcher,
           "has src that " +
               testing::DescribeMatcher<CFGNode*>(src_matcher, negation) +
               (negation ? " or" : " and") + " has sink that " +
               testing::DescribeMatcher<CFGNode*>(sink_matcher, negation) +
               (negation ? " or" : " and") + " has weight that " +
               testing::DescribeMatcher<int>(weight_matcher, negation) +
               (negation ? " or" : " and") + " has kind that " +
               testing::DescribeMatcher<CFGEdge::Kind>(kind_matcher,
                                                       negation)) {
  return ExplainMatchResult(
      AllOf(testing::Property("src", &CFGEdge::src, src_matcher),
            testing::Property("sink", &CFGEdge::sink, sink_matcher),
            testing::Property("weight", &CFGEdge::weight, weight_matcher),
            testing::Property("kind", &CFGEdge::kind, kind_matcher)),
      arg, result_listener);
}
// Matches `nodes()` of a CFG against an ordered list of matchers.
class CfgNodesMatcher {
 public:
  using is_gtest_matcher = void;

  explicit CfgNodesMatcher() { matcher_ = testing::_; }
  explicit CfgNodesMatcher(absl::Span<const testing::Matcher<CFGNode>> matchers)
      : matcher_(testing::ElementsAreArray(
            GetPointeeMatchers(std::move(matchers)))) {}

  void DescribeTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/false);
  }

  void DescribeNegationTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/true);
  }

  bool MatchAndExplain(const ControlFlowGraph& cfg,
                       testing::MatchResultListener* listener) const {
    return matcher_.MatchAndExplain(cfg.nodes(), listener);
  }

 private:
  void DescribeTo(::std::ostream* os, bool negation) const {
    *os << (negation ? " does not have" : " has") << " nodes that ";
    matcher_.DescribeTo(os);
  }
  testing::Matcher<std::vector<std::unique_ptr<CFGNode>>> matcher_;
};

// Matches the `intra_edges()` of a CFG against an unordered list of matchers.
class CfgIntraEdgesMatcher {
 public:
  using is_gtest_matcher = void;

  explicit CfgIntraEdgesMatcher() { matcher_ = testing::_; }
  explicit CfgIntraEdgesMatcher(
      absl::Span<const testing::Matcher<CFGEdge>> matchers)
      : matcher_(testing::UnorderedElementsAreArray(
            GetPointeeMatchers(std::move(matchers)))) {}

  void DescribeTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/false);
  }

  void DescribeNegationTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/true);
  }

  bool MatchAndExplain(const ControlFlowGraph& cfg,
                       testing::MatchResultListener* listener) const {
    return matcher_.MatchAndExplain(cfg.intra_edges(), listener);
  }

 private:
  void DescribeTo(::std::ostream* os, bool negation) const {
    *os << (negation ? " does not have" : " has") << " intra edges that ";
    matcher_.DescribeTo(os);
  }

  testing::Matcher<std::vector<std::unique_ptr<CFGEdge>>> matcher_;
};

// Matches the `inter_edges()` of a CFG against an unordered list of matchers.
class CfgInterEdgesMatcher {
 public:
  using is_gtest_matcher = void;

  explicit CfgInterEdgesMatcher() { matcher_ = testing::_; }

  explicit CfgInterEdgesMatcher(
      absl::Span<const testing::Matcher<CFGEdge>> matchers)
      : matcher_(testing::UnorderedElementsAreArray(
            GetPointeeMatchers(std::move(matchers)))) {}

  void DescribeTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/false);
  }

  void DescribeNegationTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/true);
  }

  bool MatchAndExplain(const ControlFlowGraph& cfg,
                       testing::MatchResultListener* listener) const {
    return matcher_.MatchAndExplain(cfg.inter_edges(), listener);
  }

 private:
  void DescribeTo(::std::ostream* os, bool negation) const {
    *os << (negation ? " does not have" : " has") << " inter edges that ";
    matcher_.DescribeTo(os);
  }

  testing::Matcher<std::vector<std::unique_ptr<CFGEdge>>> matcher_;
};

// Matches `nodes()`, `intra_edges()` and `inter_edges()` of a CFG.
class CfgMatcher {
 public:
  using is_gtest_matcher = void;

  explicit CfgMatcher(
      CfgNodesMatcher nodes_matcher = CfgNodesMatcher(),
      CfgIntraEdgesMatcher intra_edges_matcher = CfgIntraEdgesMatcher(),
      CfgInterEdgesMatcher inter_edges_matcher = CfgInterEdgesMatcher())
      : nodes_matcher_(std::move(nodes_matcher)),
        intra_edges_matcher_(std::move(intra_edges_matcher)),
        inter_edges_matcher_(std::move(inter_edges_matcher)) {}

  void DescribeTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/false);
  }

  void DescribeNegationTo(std::ostream* os) const {
    DescribeTo(os, /*negation=*/true);
  }

  bool MatchAndExplain(const ControlFlowGraph& cfg,
                       testing::MatchResultListener* listener) const {
    return nodes_matcher_.MatchAndExplain(cfg, listener) &&
           intra_edges_matcher_.MatchAndExplain(cfg, listener) &&
           inter_edges_matcher_.MatchAndExplain(cfg, listener);
  }

 private:
  void DescribeTo(::std::ostream* os, bool negation) const {
    *os << "is" << (negation ? " not" : "") << " a ControlFlowGraph which";
    nodes_matcher_.DescribeTo(os);
    *os << ", and";
    intra_edges_matcher_.DescribeTo(os);
    *os << ", and";
    inter_edges_matcher_.DescribeTo(os);
  }

  CfgNodesMatcher nodes_matcher_;
  CfgIntraEdgesMatcher intra_edges_matcher_;
  CfgInterEdgesMatcher inter_edges_matcher_;
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDOLLVM_PROPELLER_CFG_MATCHERS_H_
