#pragma once
#ifdef _WIN32
#include "protocol.h"
#include "win32_support.h"
namespace display_session_bridge {
  // Handles use overlapped I/O. Buffers stay alive until cancellation completes.
  inline DWORD pipe_io(HANDLE pipe, void *buffer, DWORD bytes, bool write,
                       ULONGLONG deadline, HANDLE peer = nullptr) {
    auto *data = static_cast<std::uint8_t *>(buffer);
    while (bytes) {
      const auto now = GetTickCount64();
      if (now >= deadline) return ERROR_TIMEOUT;
      handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!event) return GetLastError();
      OVERLAPPED ov {}; ov.hEvent = event.get();
      DWORD transferred = 0;
      BOOL ok = write ? WriteFile(pipe, data, bytes, &transferred, &ov) : ReadFile(pipe, data, bytes, &transferred, &ov);
      if (!ok) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) return error;
        HANDLE waiters[] = {event.get(), peer};
        const DWORD count = peer ? 2 : 1;
        const DWORD timeout = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD - 1));
        const DWORD result = WaitForMultipleObjects(count, waiters, FALSE, timeout);
        if (result != WAIT_OBJECT_0) {
          const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : (result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
          CancelIoEx(pipe, &ov);
          GetOverlappedResult(pipe, &ov, &transferred, TRUE);
          return wait_error;
        }
        if (!GetOverlappedResult(pipe, &ov, &transferred, FALSE)) return GetLastError();
      }
      if (!transferred || transferred > bytes) return ERROR_BROKEN_PIPE;
      data += transferred; bytes -= transferred;
    }
    return ERROR_SUCCESS;
  }
  inline DWORD send_packet(HANDLE pipe, const wire::header &h, const std::vector<std::uint8_t> &payload,
                           ULONGLONG deadline, HANDLE peer = nullptr) {
    if (!wire::bounded(h) || h.size != payload.size()) return ERROR_INVALID_DATA;
    auto encoded = wire::encode(h);
    DWORD error = pipe_io(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), true, deadline, peer);
    if (error || payload.empty()) return error;
    return pipe_io(pipe, const_cast<std::uint8_t *>(payload.data()), static_cast<DWORD>(payload.size()), true, deadline, peer);
  }
  inline DWORD receive_packet(HANDLE pipe, wire::header &h, std::vector<std::uint8_t> &payload,
                              ULONGLONG deadline, HANDLE peer = nullptr) {
    std::array<std::uint8_t, wire::header_size> encoded {};
    DWORD error = pipe_io(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), false, deadline, peer);
    if (error) return error;
    if (!wire::decode(encoded, h)) return ERROR_INVALID_DATA;
    payload.resize(h.size);
    if (payload.empty()) return ERROR_SUCCESS;
    return pipe_io(pipe, payload.data(), static_cast<DWORD>(payload.size()), false, deadline, peer);
  }
}
#endif
