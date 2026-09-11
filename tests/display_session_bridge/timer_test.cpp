#include "src/display_device/state_retry_timer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
using namespace std::chrono_literals;
using display_device::detail::state_retry_timer;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while(0)
template<class F> bool wait_for(F f) { auto end = std::chrono::steady_clock::now()+3s; while(!f() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(1ms); return f(); }
int main() {
  int scenarios=0;
  { std::mutex m; bool threw=false; try { state_retry_timer t(m,0ms); } catch(const std::invalid_argument &) { threw=true; } CHECK(threw); ++scenarios; }
  { std::mutex m; std::atomic_int calls{0}; state_retry_timer t(m,50ms);
    {std::lock_guard lock(m); t.setup_timer([&]{++calls; return true;}); t.setup_timer(nullptr);}
    std::this_thread::sleep_for(70ms); CHECK(calls==0); ++scenarios; }
  { std::mutex m; std::atomic_int calls{0}; state_retry_timer t(m,2ms);
    {std::lock_guard lock(m); t.setup_timer([&]{return ++calls==3;});}
    CHECK(wait_for([&]{return calls>=3;})); std::this_thread::sleep_for(20ms); CHECK(calls==3); ++scenarios; }
  { std::mutex m; std::atomic_bool finished{false}; state_retry_timer t(m,2ms);
    auto object=std::make_shared<std::string>(1000,'x'); std::weak_ptr<std::string> weak=object;
    {std::lock_guard lock(m); t.setup_timer([&,object]{t.setup_timer(nullptr); CHECK(object->size()==1000); finished=true; return false;});}
    object.reset(); CHECK(wait_for([&]{return finished.load();})); CHECK(wait_for([&]{return weak.expired();})); ++scenarios; }
  { std::mutex m; std::atomic_int old_calls{0},new_calls{0}; state_retry_timer t(m,2ms);
    {std::lock_guard lock(m); t.setup_timer([&]{++old_calls; t.setup_timer([&]{++new_calls; return true;}); return false;});}
    CHECK(wait_for([&]{return new_calls==1;})); std::this_thread::sleep_for(20ms); CHECK(old_calls==1 && new_calls==1); ++scenarios; }
  { std::mutex m; std::atomic_int stale{0},current{0}; state_retry_timer t(m,2ms);
    {std::lock_guard lock(m); t.setup_timer([&]{++stale; return false;}); t.setup_timer([&]{++current; return true;});}
    CHECK(wait_for([&]{return current==1;})); CHECK(stale==0); ++scenarios; }
  { std::mutex m; std::atomic_int errors{0},calls{0}; state_retry_timer t(m,2ms,[&](std::exception_ptr e){CHECK(bool(e)); ++errors;});
    {std::lock_guard lock(m); t.setup_timer([]()->bool{throw std::runtime_error("test");});}
    CHECK(wait_for([&]{return errors==1;}));
    {std::lock_guard lock(m); t.setup_timer([&]{++calls; return true;});}
    CHECK(wait_for([&]{return calls==1;})); CHECK(errors==1); ++scenarios; }
  { std::mutex m; std::atomic_bool entered{false},exited{false}; auto t=std::make_unique<state_retry_timer>(m,1ms);
    {std::lock_guard lock(m); t->setup_timer([&]{entered=true; std::this_thread::sleep_for(30ms); exited=true; return false;});}
    CHECK(wait_for([&]{return entered.load();})); t.reset(); CHECK(exited); ++scenarios; }
  for(int i=0;i<200;++i) {std::mutex m; state_retry_timer t(m,1ms); std::lock_guard lock(m); t.setup_timer([]{return true;}); t.setup_timer(nullptr);}
  ++scenarios; std::cout << "timer passed scenarios=" << scenarios << "; construction_stress=200\n";
}
