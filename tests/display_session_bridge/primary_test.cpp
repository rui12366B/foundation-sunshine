#include "src/display_device/final_primary.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace display_device::detail;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)

struct simulated_desktop {
  primary_observation state {primary_observation::secondary};
  int reads {}, writes {}, pauses {};
  bool write_ok {true}, changes_take_effect {true};
  primary_result run() {
    return reconcile_primary([&]() { ++reads; return state; },
      [&]() { ++writes; if (write_ok && changes_take_effect) state = primary_observation::primary; return write_ok; },
      [&]() { ++pauses; });
  }
};
int main() {
  int cases = 0;
  {
    simulated_desktop d; d.state = primary_observation::primary;
    const auto r = d.run(); CHECK(r); CHECK(r.observations == 3); CHECK(d.writes == 0); CHECK(d.pauses == 2); ++cases;
  }
  {
    // Reproduce the rejected launch: PRIMARY set earlier, modes/HDR restores the
    // physical primary. Old code only read three times and never corrected it.
    simulated_desktop d;
    bool old_accepted = false;
    for (int i = 0; i < 3; ++i) if (d.state == primary_observation::primary) old_accepted = true;
    CHECK(!old_accepted); CHECK(d.writes == 0);
    const auto r = d.run(); CHECK(r); CHECK(r.corrections == 1); CHECK(d.state == primary_observation::primary); ++cases;
  }
  {
    simulated_desktop d; d.changes_take_effect = false;
    const auto r = d.run(); CHECK(!r); CHECK(r.result == primary_completion::not_converged);
    CHECK(d.reads == 20); CHECK(d.writes == 3); CHECK(d.pauses == 19); ++cases;
  }
  {
    simulated_desktop d; d.write_ok = false;
    const auto r = d.run(); CHECK(!r); CHECK(r.result == primary_completion::apply_failed); CHECK(d.reads == 1); CHECK(d.writes == 1); ++cases;
  }
  {
    simulated_desktop d; d.state = primary_observation::unavailable;
    const auto r = d.run(); CHECK(!r); CHECK(r.result == primary_completion::unavailable); CHECK(d.writes == 0); CHECK(d.pauses == 0); ++cases;
  }
  {
    // Delayed publication is observed without repeatedly recreating the display.
    int reads = 0, writes = 0, pauses = 0;
    const auto r = reconcile_primary([&]() { return ++reads >= 5 ? primary_observation::primary : primary_observation::secondary; },
      [&]() { ++writes; return true; }, [&]() { ++pauses; });
    CHECK(r); CHECK(writes == 1); CHECK(reads == 7); CHECK(pauses == 6); ++cases;
  }
  {
    // Two positive readbacks followed by a reset must not satisfy the condition.
    const std::vector states {primary_observation::primary, primary_observation::primary,
      primary_observation::secondary, primary_observation::primary, primary_observation::primary, primary_observation::primary};
    std::size_t i = 0; int writes = 0;
    const auto r = reconcile_primary([&]() { CHECK(i < states.size()); return states[i++]; },
      [&]() { ++writes; return true; }, []() {});
    CHECK(r); CHECK(i == 6); CHECK(writes == 1); ++cases;
  }
  {
    int i = 0, writes = 0;
    const auto r = reconcile_primary([&]() { return i++ ? primary_observation::unavailable : primary_observation::secondary; },
      [&]() { ++writes; return true; }, []() {});
    CHECK(!r); CHECK(r.result == primary_completion::unavailable); CHECK(writes == 1); ++cases;
  }
  {
    // Repeated primary/secondary flicker never turns an accepted API call into
    // a successful stream. Each correction is separated by five observations.
    int i = 0; std::vector<int> when;
    const auto r = reconcile_primary([&]() { return ++i % 3 == 0 ? primary_observation::secondary : primary_observation::primary; },
      [&]() { when.push_back(i); return true; }, []() {});
    CHECK(!r); CHECK(when.size() == 3); CHECK(when[1] - when[0] >= 5); CHECK(when[2] - when[1] >= 5); ++cases;
  }
  {
    // A fresh launch has its own budget; failed prior requests leave no static
    // state, worker, detached retry, or ownership of the virtual monitor here.
    simulated_desktop first; first.changes_take_effect = false; CHECK(!first.run());
    simulated_desktop second; CHECK(second.run()); CHECK(second.writes == 1); ++cases;
  }
  {
    int reads = 0;
    const auto r = reconcile_primary([&]() { return ++reads < 18 ? primary_observation::secondary : primary_observation::primary; },
      []() { return true; }, []() {});
    CHECK(r); CHECK(r.observations == 20); ++cases;
  }
  {
    int reads = 0;
    const auto r = reconcile_primary([&]() { return ++reads < 19 ? primary_observation::secondary : primary_observation::primary; },
      []() { return true; }, []() {});
    CHECK(!r); CHECK(r.observations == 20); ++cases;
  }
  std::cout << "primary reconciliation: " << cases << "/" << cases << " passed; old poll-only regression reproduced\n";
}
