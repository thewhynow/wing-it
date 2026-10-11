#include "common.hpp"

size_t FlowField::cellIndex(const Grid &g, size_t x, size_t y, size_t z) {
    return (z * g.ny + y) * g.nx + x;
}

