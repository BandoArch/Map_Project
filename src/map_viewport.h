#pragma once

#include <algorithm>
#include <cmath>

struct MapViewport {
    double center_x = 0.5;
    double center_y = 0.5;

    static double clamp_center(double center, double viewport, double extent)
    {
        if (extent <= viewport) return 0.5;
        const double margin = viewport / (2 * extent);
        return std::max(margin, std::min(1 - margin, center));
    }

    void clamp_to_coverage(double width, double height, double extent)
    {
        center_x = clamp_center(center_x, width, extent);
        center_y = clamp_center(center_y, height, extent);
    }

    void drag(double dx, double dy, double width, double height, double extent)
    {
        if (extent <= 0) return;
        center_x -= dx / extent;
        center_y -= dy / extent;
        clamp_to_coverage(width, height, extent);
    }
};
