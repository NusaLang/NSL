#pragma once

#include <cmath>

// Python's `%`: the result takes the sign of the divisor (-7 % 3 == 2), unlike C's fmod.
inline double pyModulo(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
