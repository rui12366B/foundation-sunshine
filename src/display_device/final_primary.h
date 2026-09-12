/**
 * @file src/display_device/final_primary.h
 * @brief Bounded primary-display reconciliation after resolution/HDR changes.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <cstdint>

namespace display_device::detail {
  enum class primary_observation { primary, secondary, unavailable };
  enum class primary_completion { verified, unavailable, apply_failed, not_converged };

  struct primary_result {
    primary_completion result {primary_completion::not_converged};
    std::uint32_t observations {};
    std::uint32_t corrections {};
    explicit operator bool() const { return result == primary_completion::verified; }
  };

  /**
   * Run synchronously within the display transaction, BEFORE capture starts.
   * A modeset or HDR transition may settle after SetDisplayConfig returns.
   * Observe fresh state, reassert the requested primary, then require three
   * consecutive observations. Never recreate VDD, bypass a session mismatch,
   * change the original restore snapshot, or turn an accepted write into success
   * without readback. The caller remains responsible for normal rollback.
   *
   * pause() supplies 100 ms in production; injected callbacks make the exact
   * policy testable without changing a developer's physical display layout.
   */
  template<class Observe, class Apply, class Pause>
  primary_result reconcile_primary(Observe observe, Apply apply, Pause pause) {
    constexpr std::uint32_t max_observations = 20;
    constexpr std::uint32_t max_corrections = 3;
    constexpr std::uint32_t stable_observations = 3;
    constexpr std::uint32_t correction_spacing = 5;
    primary_result result;
    std::uint32_t stable = 0, next_correction = 0;
    for (std::uint32_t i = 0; i < max_observations; ++i) {
      const auto state = observe();
      ++result.observations;
      if (state == primary_observation::unavailable) {
        result.result = primary_completion::unavailable;
        return result;
      }
      if (state == primary_observation::primary) {
        if (++stable == stable_observations) {
          result.result = primary_completion::verified;
          return result;
        }
      } else {
        stable = 0;
        if (result.corrections < max_corrections && i >= next_correction) {
          ++result.corrections;
          if (!apply()) {
            result.result = primary_completion::apply_failed;
            return result;
          }
          next_correction = i + correction_spacing;
        }
      }
      if (i + 1 < max_observations) pause();
    }
    return result;
  }
}  // namespace display_device::detail
