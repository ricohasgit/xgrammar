#include <chrono>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

#include "structural_tag.h"

using namespace std::chrono;

struct Case {
  std::string label;
  std::string pattern;
  std::vector<std::string> excludes;
};

std::string BuildRegexTagJSON(const Case& c) {
  std::ostringstream os;
  os << R"({"type":"structural_tag","format":{"type":"regex","pattern":")" << c.pattern << R"(")";
  if (!c.excludes.empty()) {
    os << R"(,"excludes":[)";
    for (size_t i = 0; i < c.excludes.size(); ++i) {
      if (i) os << ",";
      os << "\"" << c.excludes[i] << "\"";
    }
    os << "]";
  }
  os << "}}";
  return os.str();
}

int main() {
  std::vector<Case> cases = {
      {"baseline_no_excludes", "[a-z]+", {}},
      {"one_short_exclude", "[a-z]+", {"bad"}},
      {"three_short_excludes", "[a-z]+", {"foo", "bar", "baz"}},
      {"one_long_exclude_20", "[a-z]+", {std::string(20, 'a')}},
      {"complex_id_with_keywords",
       "[a-zA-Z_][a-zA-Z0-9_]*",
       {"function", "return", "class", "if", "else", "while", "for"}},
  };

  constexpr int kIters = 20;
  std::cout << "regex_excludes_compile_benchmark\n";
  std::cout << "label,mean_ms,min_ms,max_ms,rules\n";

  for (const auto& c : cases) {
    std::vector<double> times_ms;
    times_ms.reserve(kIters);
    int rule_count = 0;

    for (int i = 0; i < kIters; ++i) {
      auto start = high_resolution_clock::now();
      auto res = xgrammar::StructuralTagToGrammar(BuildRegexTagJSON(c));
      auto end = high_resolution_clock::now();
      times_ms.push_back(
          duration<double, std::milli>(end - start).count()
      );
      if (res.IsOk() && i == 0) {
        // Crude rule count from string form; cheap and sufficient for comparison.
        auto printed = std::move(res).Unwrap().ToString();
        rule_count = static_cast<int>(std::count(
            printed.begin(), printed.end(), '='
        ) / 2);  // each ::= contributes two '='
      }
    }

    double sum = 0, mn = times_ms[0], mx = times_ms[0];
    for (double t : times_ms) {
      sum += t;
      mn = std::min(mn, t);
      mx = std::max(mx, t);
    }
    double mean = sum / times_ms.size();

    std::cout << c.label << "," << mean << "," << mn << "," << mx << "," << rule_count << "\n";
  }

  return 0;
}

