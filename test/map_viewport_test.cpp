#include "../src/map_viewport.h"
#include <cassert>
#include <cmath>

int main()
{
    MapViewport view;
    view.drag(40, -20, 220, 224, 440);
    assert(std::abs(view.center_x - (0.5 - 40.0 / 440)) < 1e-9);
    assert(std::abs(view.center_y - (0.5 + 20.0 / 440)) < 1e-9);
    view.drag(10000, -10000, 220, 224, 440);
    assert(view.center_x == 0.25);
    assert(std::abs(view.center_y - (1 - 224.0 / 880)) < 1e-9);
    view.clamp_to_coverage(220, 224, 110);
    assert(view.center_x == 0.5 && view.center_y == 0.5);
    view.drag(20, 20, 220, 224, 0);
    assert(view.center_x == 0.5 && view.center_y == 0.5);
}
