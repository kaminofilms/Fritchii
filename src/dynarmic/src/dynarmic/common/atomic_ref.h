// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <type_traits>

namespace Dynarmic {

/// The parts of std::atomic_ref the exclusive monitor uses, on the compiler's __atomic builtins:
/// the Android NDK's libc++ does not provide std::atomic_ref.
template<typename T>
class AtomicRef {
    static_assert(std::is_trivially_copyable_v<T> && (sizeof(T) == 4 || sizeof(T) == 8));

public:
    explicit AtomicRef(T& object) noexcept
            : ptr{&object} {}

    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return __atomic_load_n(ptr, ToBuiltin(order));
    }

    void store(T desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
        __atomic_store_n(ptr, desired, ToBuiltin(order));
    }

    bool compare_exchange_strong(T& expected, T desired,
                                 std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange_n(ptr, &expected, desired, false, ToBuiltin(order),
                                           ToBuiltin(FailureOrder(order)));
    }

private:
    static constexpr int ToBuiltin(std::memory_order order) noexcept {
        switch (order) {
        case std::memory_order_relaxed:
            return __ATOMIC_RELAXED;
        case std::memory_order_consume:
            return __ATOMIC_CONSUME;
        case std::memory_order_acquire:
            return __ATOMIC_ACQUIRE;
        case std::memory_order_release:
            return __ATOMIC_RELEASE;
        case std::memory_order_acq_rel:
            return __ATOMIC_ACQ_REL;
        default:
            return __ATOMIC_SEQ_CST;
        }
    }

    /// A failed compare-exchange only loads, so it cannot have release semantics.
    static constexpr std::memory_order FailureOrder(std::memory_order order) noexcept {
        switch (order) {
        case std::memory_order_acq_rel:
            return std::memory_order_acquire;
        case std::memory_order_release:
            return std::memory_order_relaxed;
        default:
            return order;
        }
    }

    T* ptr;
};

}  // namespace Dynarmic
