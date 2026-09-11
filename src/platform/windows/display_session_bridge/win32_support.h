#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace display_session_bridge {
  class handle {
  public:
    handle() = default;
    explicit handle(HANDLE value): value_(value) {}
    ~handle() { reset(); }
    handle(const handle &) = delete;
    handle &operator=(const handle &) = delete;
    handle(handle &&other) noexcept: value_(other.release()) {}
    handle &operator=(handle &&other) noexcept {
      if (this != &other) reset(other.release());
      return *this;
    }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
  private:
    HANDLE value_ {};
  };
  inline bool process_is_system() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    handle token(raw);
    DWORD required = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (!required) return false;
    std::vector<std::uint8_t> data(required);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), required, &required)) return false;
    return IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, WinLocalSystemSid);
  }
  inline std::wstring module_path() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
      const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (!n) return {};
      if (n < buffer.size()) return std::wstring(buffer.data(), n);
      if (buffer.size() >= 32768) return {};
      buffer.resize(buffer.size() * 2);
    }
  }
}
#endif
