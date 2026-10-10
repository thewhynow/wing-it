#include "common.hpp"

size_t FlowField::cellIndex(Grid &g, size_t x, size_t y, size_t z) const {
    return (z * g.ny + y) * g.nx + x;
}

