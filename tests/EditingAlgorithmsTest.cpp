#include "EditingAlgorithms.h"
#include <stdexcept>
#include <iostream>
static void require(bool value) { if (!value) throw std::runtime_error("editing algorithm regression"); }
int main() {
    using editing::budgetSpan;
    auto shortShot = budgetSpan(1.25, 1.75, 90, 60, false);
    require(shortShot.second - shortShot.first >= 0.5);
    auto trimmed = budgetSpan(0, 3600, 90, 60, false);
    require(trimmed.first == 0 && trimmed.second == 90);
    require(budgetSpan(0, 100, 90, 60, true).second == 0);
    require(budgetSpan(0, 0.01, 90, 60, false).second == 0);
    require(budgetSpan(0, 20, 0.1, 60, false).second == 0);
    const auto fractional = budgetSpan(1.123, 5.678, 2, 30000.0/1001, false);
    require(fractional.second > fractional.first);
    require(fractional.second - fractional.first <= 2.000001);
    require(std::abs(fractional.first * (30000.0/1001) - std::round(fractional.first * (30000.0/1001))) < 1e-6);
    require(editing::sameRetake(u"今天介绍这个功能", u"今天介绍这个功能"));
    require(editing::sameRetake(u"thisisalongrepeatedsentence", u"thisisalongrepeatedsentencf"));
    require(!editing::sameRetake(u"今天很好", u"今天不好"));
    require(!editing::sameRetake(u"", u""));
    std::cout << "Editing algorithms checks passed\n";
}
