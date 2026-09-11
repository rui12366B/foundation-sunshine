/** Bounded local CCD RPC. GPL-3.0; Foundation/Vibepollo architecture. */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace display_session_bridge::wire {
  constexpr std::uint32_t magic = 0x42535346;
  constexpr std::uint32_t version = 1;
  constexpr std::size_t header_size = 40;
  constexpr std::size_t max_payload = 1024 * 1024;
  constexpr std::uint32_t max_paths = 512;
  constexpr std::uint32_t max_modes = 4096;
  constexpr std::uint32_t max_device_info = 4096;
  constexpr std::uint32_t invalid_session = 0xffffffffu;
  enum class operation : std::uint32_t {
    sizes = 1, query = 2, set_config = 3, get_info = 4, set_info = 5, ping = 6, stop = 7,
  };
  struct header {
    operation op {operation::ping};
    std::uint32_t size {};
    std::uint64_t sequence {};
    std::uint64_t generation {};
    std::uint32_t session {invalid_session};
    std::uint32_t status {};
  };
  inline bool known(operation op) {
    const auto n = static_cast<std::uint32_t>(op);
    return n >= 1 && n <= 7;
  }
  inline bool bounded(const header &h) {
    return known(h.op) && h.size <= max_payload && h.sequence != 0 &&
      h.generation != 0 && h.session != invalid_session;
  }
  inline std::array<std::uint8_t, header_size> encode(const header &h) {
    std::array<std::uint8_t, header_size> out {};
    auto put = [&](std::size_t at, std::uint64_t value, std::size_t size) {
      for (std::size_t i = 0; i < size; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    put(0, magic, 4); put(4, version, 4);
    put(8, static_cast<std::uint32_t>(h.op), 4); put(12, h.size, 4);
    put(16, h.sequence, 8); put(24, h.generation, 8);
    put(32, h.session, 4); put(36, h.status, 4);
    return out;
  }
  inline bool decode(std::span<const std::uint8_t> in, header &h) {
    if (in.size() != header_size) return false;
    auto get = [&](std::size_t at, std::size_t size) {
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t(in[at + i]) << (i * 8);
      return value;
    };
    if (get(0, 4) != magic || get(4, 4) != version) return false;
    h.op = static_cast<operation>(get(8, 4));
    h.size = static_cast<std::uint32_t>(get(12, 4));
    h.sequence = get(16, 8); h.generation = get(24, 8);
    h.session = static_cast<std::uint32_t>(get(32, 4));
    h.status = static_cast<std::uint32_t>(get(36, 4));
    return bounded(h);
  }
  inline bool matches(const header &request, const header &reply) {
    return bounded(reply) && request.sequence == reply.sequence &&
      request.generation == reply.generation && request.op == reply.op && request.session == reply.session;
  }
  class sequence_guard {
  public:
    bool accept(const header &h, std::uint32_t console_session) {
      if (!bounded(h) || h.status != 0 || h.session != console_session ||
          (generation_ && generation_ != h.generation) || h.sequence <= last_) return false;
      generation_ = h.generation; last_ = h.sequence; return true;
    }
  private:
    std::uint64_t generation_ {};
    std::uint64_t last_ {};
  };
  class writer {
  public:
    template<class T> bool pod(const T &value) {
      static_assert(std::is_trivially_copyable_v<T>);
      return bytes(&value, sizeof(T));
    }
    bool bytes(const void *src, std::size_t n) {
      if (n > max_payload - data_.size() || (n && !src)) return false;
      if (!n) return true;
      const auto *p = static_cast<const std::uint8_t *>(src);
      data_.insert(data_.end(), p, p + n); return true;
    }
    template<class T> bool array(const T *p, std::size_t n, std::size_t limit) {
      static_assert(std::is_trivially_copyable_v<T>);
      if (n > limit || n > (max_payload - data_.size()) / sizeof(T)) return false;
      return bytes(p, n * sizeof(T));
    }
    const std::vector<std::uint8_t> &data() const { return data_; }
    std::vector<std::uint8_t> take() { return std::move(data_); }
  private:
    std::vector<std::uint8_t> data_;
  };
  class reader {
  public:
    explicit reader(std::span<const std::uint8_t> data): data_(data) {}
    template<class T> bool pod(T &value) {
      static_assert(std::is_trivially_copyable_v<T>);
      return bytes(&value, sizeof(T));
    }
    bool bytes(void *dst, std::size_t n) {
      if (n > data_.size() - pos_ || (n && !dst)) return false;
      if (n) std::memcpy(dst, data_.data() + pos_, n);
      pos_ += n; return true;
    }
    template<class T> bool array(std::vector<T> &out, std::size_t count, std::size_t limit) {
      static_assert(std::is_trivially_copyable_v<T>);
      if (count > limit || count > (data_.size() - pos_) / sizeof(T)) return false;
      out.resize(count); return bytes(out.data(), count * sizeof(T));
    }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool done() const { return pos_ == data_.size(); }
  private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ {};
  };
}
