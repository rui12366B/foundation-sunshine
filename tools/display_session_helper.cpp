/** GPL-3.0. Console-scoped CCD helper. No unlocking, network server or command execution. */
#include <string_view>
#include <new>
#ifdef _WIN32
#include "src/platform/windows/display_session_bridge/dispatch.h"
#include "src/platform/windows/display_session_bridge/transport.h"
#include "src/platform/windows/display_session_bridge/session_context.h"
#include <cerrno>
#include <cstdlib>
#include <iostream>
namespace {
  using namespace display_session_bridge;
  bool parse_number(const wchar_t *text, DWORD &result) {
    if (!text || !*text) return false;
    for (auto p = text; *p; ++p) if (*p < L'0' || *p > L'9') return false;
    errno = 0; wchar_t *end = nullptr;
    const auto value = std::wcstoull(text, &end, 10);
    if (errno || *end || value > MAXDWORD) return false;
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
  int serve(const std::wstring &pipe_name, DWORD parent_id, execution_context context, const console_principal &expected) {
    if (!pipe_name.starts_with(L"\\\\.\\pipe\\FoundationDisplay-") || pipe_name.size() > 256 || !parent_id) return ERROR_INVALID_PARAMETER;
    DWORD own_session = wire::invalid_session;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || own_session != expected.session) return ERROR_NO_SUCH_LOGON_SESSION;
    DWORD error = verify_console_context(context, expected, true);
    if (error) return static_cast<int>(error);
    // A user worker may not open a SYSTEM parent; closure of the pipe also ends it.
    handle parent(OpenProcess(SYNCHRONIZE, FALSE, parent_id));
    handle pipe; const auto connect_deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < connect_deadline) {
      pipe.reset(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
      if (pipe) break;
      error = GetLastError();
      if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return static_cast<int>(error);
      WaitNamedPipeW(pipe_name.c_str(), 100); Sleep(10);
    }
    if (!pipe) return ERROR_TIMEOUT;
    ULONG server_id = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &server_id) || server_id != parent_id) return ERROR_ACCESS_DENIED;
    const bool system = uses_system(context);
    input_desktop_binding desktop; wire::sequence_guard sequence;
    for (;;) {
      wire::header h; std::vector<std::uint8_t> payload;
      error = receive_packet(pipe.get(), h, payload, GetTickCount64() + 60000, parent.get());
      if (error == ERROR_BROKEN_PIPE || error == ERROR_TIMEOUT) return 0;
      if (error) return static_cast<int>(error);
      if (!sequence.accept(h, expected.session)) return ERROR_INVALID_DATA;
      std::vector<std::uint8_t> result;
      LONG status = static_cast<LONG>(verify_console_context(context, expected, true));
      if (status == ERROR_SUCCESS) status = dispatch(h.op, payload, result, system);
      // Attach only this SYSTEM worker to its console's active input desktop.
      // The workstation remains locked. Real CCD errors are retained, and
      // SYSTEM modesets do not use SDC_SAVE_TO_DATABASE.
      if (status == ERROR_ACCESS_DENIED && system &&
          verify_console_context(context, expected, true) == ERROR_SUCCESS && desktop.sync() &&
          verify_console_context(context, expected, true) == ERROR_SUCCESS) {
        status = dispatch(h.op, payload, result, true);
      }
      const DWORD after = verify_console_context(context, expected, true);
      if (after) { status = static_cast<LONG>(after); result.clear(); }
      h.status = static_cast<std::uint32_t>(status); h.size = static_cast<std::uint32_t>(result.size());
      const DWORD send_error = send_packet(pipe.get(), h, result, GetTickCount64() + 5000, parent.get());
      if (send_error) return static_cast<int>(send_error);
      if (after || h.op == wire::operation::stop) return 0;
    }
  }
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 15 || std::wstring_view(argv[1]) != L"--pipe" || std::wstring_view(argv[3]) != L"--parent" ||
      std::wstring_view(argv[5]) != L"--session" || std::wstring_view(argv[7]) != L"--context" ||
      std::wstring_view(argv[9]) != L"--auth-low" || std::wstring_view(argv[11]) != L"--auth-high" ||
      std::wstring_view(argv[13]) != L"--sid") {
    std::wcerr << L"This helper must be started by the matching Foundation Sunshine build.\n";
    return ERROR_INVALID_PARAMETER;
  }
  DWORD parent = 0, high = 0; console_principal expected;
  if (!parse_number(argv[4], parent) || !parse_number(argv[6], expected.session) ||
      !parse_number(argv[10], expected.authentication.LowPart) || !parse_number(argv[12], high) ||
      !parent || parent == MAXDWORD || expected.session == wire::invalid_session) return ERROR_INVALID_PARAMETER;
  expected.authentication.HighPart = static_cast<LONG>(high);
  expected.sid = argv[14];
  if (expected.sid.empty() || expected.sid.size() > 256) return ERROR_INVALID_PARAMETER;
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(expected.sid.c_str(), &sid)) return ERROR_INVALID_PARAMETER;
  const bool valid_sid = IsValidSid(sid); LocalFree(sid);
  if (!valid_sid) return ERROR_INVALID_PARAMETER;
  execution_context context;
  const std::wstring_view mode(argv[8]);
  if (mode == L"user") context = execution_context::user;
  else if (mode == L"prelogin") context = execution_context::prelogin;
  else if (mode == L"locked") context = execution_context::locked_console;
  else return ERROR_INVALID_PARAMETER;
  try { return serve(argv[2], parent, context, expected); }
  catch (const std::bad_alloc &) { return ERROR_NOT_ENOUGH_MEMORY; }
  catch (...) { return ERROR_GEN_FAILURE; }
}
#endif
