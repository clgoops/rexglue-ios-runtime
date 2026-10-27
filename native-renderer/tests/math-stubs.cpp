// SPDX-License-Identifier: BSD-3-Clause
// Platform-independent implementation for isolated SDK layout tests.
#include <bit>
#include <cstdint>
namespace rex {uint8_t lzcnt(uint32_t value) {return uint8_t(std::countl_zero(value));}}
