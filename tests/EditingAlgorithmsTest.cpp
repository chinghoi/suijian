#include "EditingAlgorithms.h"
#include <stdexcept>
#include <iostream>
static void require(bool value) { if (!value) throw std::runtime_error("editing algorithm regression"); }
int main() {
    using editing::budgetSpan;
    const std::vector<editing::BudgetClip> day{{0, 100, 30}, {0, 100, 30}, {0, 100, 30}};
    const auto spread = editing::spreadBudget(day, 90, false);
    double total = 0;
    for (const auto &span : spread) {
        require(span.first > 0 && span.second > span.first);
        total += span.second - span.first;
    }
    require(total <= 90 && total > 89.8);
    const auto speech = editing::spreadBudget({{0,10,30},{10,20,30},{20,30,30},{30,40,30}}, 20, true);
    require(speech[0].second == 10 && speech[2].second == 30);
    require(speech[1].second == 0 && speech[3].second == 0);
    const auto shortClips = editing::spreadBudget({{0,1.5,30},{0,1.5,30},{0,1.5,30},{0,1.5,30}}, 1.8, false);
    double shortTotal = 0;
    for (const auto &span : shortClips) shortTotal += span.second - span.first;
    require(shortTotal >= 1.0 && shortTotal <= 1.8);
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
    const auto balanced = editing::dialogueFirstBudget({
        {0,4,30,true,1}, {4,11,30,true,3}, {11,17,30,true,2}, {17,25,30,false}
    }, 16);
    require(balanced[1] == std::make_pair(4.0, 11.0) && balanced[3].second - balanced[3].first >= 4.0);
    const auto perSource = editing::dialogueFirstBudget({
        {0,100,30,false,0,0,100}, {0,50,30,false,0,1,50}
    }, 30);
    require(perSource[0].second - perSource[0].first == 20);
    require(perSource[1].second - perSource[1].first == 10);
    require(editing::sameRetake(u"今天介绍这个功能", u"今天介绍这个功能"));
    require(editing::sameRetake(u"thisisalongrepeatedsentence", u"thisisalongrepeatedsentencf"));
    require(!editing::sameRetake(u"今天很好", u"今天不好"));
    require(!editing::sameRetake(u"", u""));
    std::cout << "Editing algorithms checks passed\n";
}
