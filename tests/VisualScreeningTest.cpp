#include "VisualScreening.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    constexpr int w = 160, h = 90;
    std::vector<std::uint8_t> black(w*h, 0), white(w*h, 255), flat(w*h, 100), texture(w*h), inverse(w*h);
    for (int i=0; i<w*h; ++i) { texture[i] = (i%w/4+i/w/4)%2 ? 180 : 60; inverse[i] = 240-texture[i]; }
    require(!screening::usableFrame(black.data(), nullptr, w,h), "black frame retained");
    require(!screening::usableFrame(white.data(), nullptr, w,h), "white frame retained");
    require(!screening::usableFrame(flat.data(), nullptr, w,h), "flat frame retained");
    require(screening::usableFrame(texture.data(), texture.data(), w,h), "static detail rejected");
    require(!screening::usableFrame(inverse.data(), texture.data(), w,h), "large changes retained");
    auto spans = screening::mergeSpans({{4,8},{-1,2},{1,3},{8,12},{6,5}},10);
    require(spans == std::vector<screening::Span>{{0,3},{4,10}}, "merge crossed a gap or duration boundary");
    std::cout << "Visual screening checks passed\n";
}
