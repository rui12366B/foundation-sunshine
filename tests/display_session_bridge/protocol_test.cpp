#include "src/platform/windows/display_session_bridge/protocol.h"
#include <cstdlib>
#include <iostream>
#include <random>
using namespace display_session_bridge::wire;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while(0)
int main() {
  header h {operation::set_config, 8192, 0x1234567890abcdefULL, 87, 2, 0};
  auto encoded = encode(h); header decoded;
  CHECK(encoded.size() == 40);
  CHECK(encoded[0] == 'F' && encoded[1] == 'S' && encoded[2] == 'S' && encoded[3] == 'B');
  CHECK(decode(encoded, decoded)); CHECK(matches(h, decoded) && decoded.size == h.size);
  for(std::size_t n = 0; n < header_size; ++n) CHECK(!decode(std::span(encoded).first(n), decoded));
  auto bad = encoded; bad[0] ^= 1; CHECK(!decode(bad, decoded));
  bad = encoded; bad[4] = 2; CHECK(!decode(bad, decoded));
  for(auto op : {0u, 8u, 0xffffffffu}) { auto b = h; b.op = static_cast<operation>(op); CHECK(!decode(encode(b), decoded)); }
  auto b = h; b.size = max_payload + 1; CHECK(!decode(encode(b), decoded));
  b = h; b.sequence = 0; CHECK(!decode(encode(b), decoded));
  b = h; b.generation = 0; CHECK(!decode(encode(b), decoded));
  b = h; b.session = invalid_session; CHECK(!decode(encode(b), decoded));
  b = h; b.sequence++; CHECK(!matches(h,b));
  b = h; b.generation++; CHECK(!matches(h,b));
  b = h; b.session++; CHECK(!matches(h,b));
  b = h; b.op = operation::ping; CHECK(!matches(h,b));
  sequence_guard guard; CHECK(guard.accept(h,2)); CHECK(!guard.accept(h,2));
  b = h; b.sequence++;
  auto altered = b; altered.session = 3; CHECK(!guard.accept(altered,2));
  altered = b; altered.generation++; CHECK(!guard.accept(altered,2));
  altered = b; altered.status = 5; CHECK(!guard.accept(altered,2));
  CHECK(!guard.accept(b,3)); CHECK(guard.accept(b,2));
  writer out; CHECK(out.pod(std::uint32_t(42)));
  const std::uint64_t values[] {1,2,3,0xffffffffffffffffULL};
  CHECK(out.array(values,4,4)); CHECK(!out.array(values,5,4));
  CHECK(!out.array(values,std::numeric_limits<std::size_t>::max(),std::numeric_limits<std::size_t>::max()));
  CHECK(!out.bytes(nullptr,1)); CHECK(out.bytes(nullptr,0));
  reader in(out.data()); std::uint32_t number = 0;
  CHECK(in.pod(number) && number == 42); std::vector<std::uint64_t> parsed;
  CHECK(!in.array(parsed,5,4)); CHECK(in.array(parsed,4,4) && parsed[3] == values[3]);
  CHECK(in.done()); CHECK(!in.pod(number));
  reader short_input(std::span(out.data()).first(2)); CHECK(!short_input.pod(number)); CHECK(short_input.remaining() == 2);
  std::vector<std::uint8_t> huge(max_payload,0x55); writer full;
  CHECK(full.bytes(huge.data(),huge.size())); CHECK(!full.pod(std::uint8_t(1)));
  std::mt19937_64 random(0x65167cf); std::uint64_t accepted = 0;
  for(int i=0; i<100000; ++i) {
    auto bytes = encoded; bytes[random()%header_size] ^= static_cast<std::uint8_t>(random()|1);
    if(decode(bytes,decoded)) { CHECK(bounded(decoded)); header again; CHECK(decode(encode(decoded),again)); CHECK(matches(decoded,again)); ++accepted; }
  }
  std::cout << "protocol passed; 100000 deterministic mutations; well_formed=" << accepted << '\n';
}
