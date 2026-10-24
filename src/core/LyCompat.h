// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers that paper over libyang API differences between versions.
#pragma once

#include <cstdint>

namespace ys {

/// Number of items in a libyang "sized array". All libyang versions store the
/// count as a uint64_t right before the first item (the macro that reads it
/// was renamed from LY_ARRAY_COUNT to LYA_COUNT in newer releases).
template<class T>
inline uint64_t lyCount(const T *array)
{
    return array ? *(reinterpret_cast<const uint64_t *>(array) - 1) : 0;
}

} // namespace ys
