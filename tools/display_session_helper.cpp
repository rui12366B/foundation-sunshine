/** GPL-3.0. Local console display helper. No network listener or command execution. */
#include <string_view>
#include <new>
#ifdef _WIN32
#include "src/platform/windows/display_session_bridge/dispatch.h"
#include "src/platform/windows/display_session_bridge/transport.h"
#include <wtsapi32.h>
#include <cerrno>
#include <cstdlib>
#include <iostream>
namespace {
  using namespace display_session_bridge;
  bool parse_id(const wchar_t *text, DWORD &result) {
    if (!text || !*text) return false;
    for (auto p = text; *p; ++p) if (*p < L'0' || *p > L'9') return false;
    errno = 0; wchar_t *end = nullptr;
    const auto value = std::wcstoull(text, &end, 10);
    if (errno || *end || value >= wire::invalid_session) return false;
    result = static_cast<DWORD>(value); return true;
  }
  class input_desktop_binding {
  public:
    ~input_desktop_binding() {
      if (owned_ && SetThreadDesktop(original_)) CloseDesktop(owned_);
    }
    bool sync() {
      if (!original_) original_ = GetThreadDesktop(GetCurrentThreadId());
      HDESK next = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
      if (!next) return false;
      if (!SetThreadDesktop(next)) { CloseDesktop(next); return false; }
      if (owned_) CloseDesktop(owned_);
      owned_ = next; return true;
    }
  private:
    HDESK original_ {}, owned_ {};
  };
  int serve(const std::wstring &pipe_name, DWORD parent_id, DWORD session) {
    if (!pipe_name.starts_with(L"\\\\.\\pipe\\FoundationDisplay-") || pipe_name.size() > 256 || !parent_id) return ERROR_INVALID_PARAMETER;
    DWORD own_session = wire::invalid_session;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || own_session != session ||
        WTSGetActiveConsoleSessionId() != session) return ERROR_NO_SUCH_LOGON_SESSION;
    // An ordinary user may not open the SYSTEM parent; pipe closure also signals its death.
    handle parent(OpenProcess(SYNCHRONIZE, FALSE, parent_id));
    handle pipe; const auto connect_deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < connect_deadline) {
      pipe.reset(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
      if (pipe) break;
      const DWORD error = GetLastError();
      if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return static_cast<int>(error);
      WaitNamedPipeW(pipe_name.c_str(), 100); Sleep(10);
    }
    if (!pipe) return ERROR_TIMEOUT;
    ULONG server_id = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &server_id) || server_id != parent_id) return ERROR_ACCESS_DENIED;
    const bool system = process_is_system(); input_desktop_binding desktop; wire::sequence_guard sequence;
    for (;;) {
      wire::header h; std::vector<std::uint8_t> payload;
      const DWORD error = receive_packet(pipe.get(), h, payload, GetTickCount64() + 60000, parent.get());
      if (error == ERROR_BROKEN_PIPE || error == ERROR_TIMEOUT) return 0;
      if (error) return static_cast<int>(error);
      if (!sequence.accept(h, session) || WTSGetActiveConsoleSessionId() != session) return ERROR_NO_SUCH_LOGON_SESSION;
      std::vector<std::uint8_t> result;
      LONG status = dispatch(h.op, payload, result, system);
      // Probe the actual CCD API. SYSTEM prelogin may bind its own worker to the
      // input desktop, but no user desktop is switched or unlocked.
      if (status == ERROR_ACCESS_DENIED && system && WTSGetActiveConsoleSessionId() == session && desktop.sync())
        status = dispatch(h.op, payload, result, system);
      if (WTSGetActiveConsoleSessionId() != session) { status = ERROR_NO_SUCH_LOGON_SESSION; result.clear(); }
      h.status = static_cast<std::uint32_t>(status); h.size = static_cast<std::uint32_t>(result.size());
      const DWORD send_error = send_packet(pipe.get(), h, result, GetTickCount64() + 5000, parent.get());
      if (send_error) return static_cast<int>(send_error);
      if (h.op == wire::operation::stop) return 0;
    }
  }
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 7 || std::wstring_view(argv[1]) != L"--pipe" || std::wstring_view(argv[3]) != L"--parent" ||
      std::wstring_view(argv[5]) != L"--session") {
    std::wcerr << L"This helper must be started by Foundation Sunshine.\n"; return ERROR_INVALID_PARAMETER;
  }
  DWORD parent = 0, session = 0;
  if (!parse_id(argv[4], parent) || !parse_id(argv[6], session)) return ERROR_INVALID_PARAMETER;
  try { return serve(argv[2], parent, session); }
  catch (const std::bad_alloc &) { return ERROR_NOT_ENOUGH_MEMORY; }
  catch (...) { return ERROR_GEN_FAILURE; }
}
#endif
