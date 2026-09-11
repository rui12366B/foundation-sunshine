/** GPL-3.0. Console-helper adapter for Foundation, following Vibepollo's architecture.
 * No password capture, automatic unlocking, or arbitrary user-session takeover.
 * Foundation remains the owner of display settings and persistence.
 */
#ifdef _WIN32
#include "client.h"
#include "protocol.h"
#include "transport.h"
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>

namespace display_session_bridge {
  namespace {
    std::atomic_bool feature_enabled {false};
    std::atomic<log_callback> logger {nullptr};
    thread_local std::optional<DWORD> pinned_session;
    std::recursive_mutex transaction_mutex;
    void note(const std::string &message) {
      if (const auto sink = logger.load()) sink("[Display Session Helper] " + message);
    }
    struct identity {
      DWORD session {wire::invalid_session};
      LUID authentication {};
      std::wstring sid;
      bool system_fallback {};
      bool operator==(const identity &other) const {
        return session == other.session && authentication.LowPart == other.authentication.LowPart &&
          authentication.HighPart == other.authentication.HighPart && sid == other.sid && system_fallback == other.system_fallback;
      }
    };
    thread_local std::optional<identity> pinned_identity;
    struct environment {
      void *data {};
      ~environment() { if (data) DestroyEnvironmentBlock(data); }
    };
    // A single console is selected. A missing existing-user token is a hard error,
    // not permission to operate as SYSTEM or to select another logged-in user.
    DWORD acquire_identity(DWORD session, identity &who, handle &token) {
      if (session == wire::invalid_session || WTSGetActiveConsoleSessionId() != session) return ERROR_NO_SUCH_LOGON_SESSION;
      LPWSTR name = nullptr; DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &name, &bytes)) return GetLastError();
      const bool has_user = name && bytes >= sizeof(wchar_t) && name[0] != L'\0';
      if (name) WTSFreeMemory(name);
      HANDLE raw = nullptr; who.session = session;
      if (has_user) {
        if (!WTSQueryUserToken(session, &raw)) return GetLastError();
      } else {
        if (!process_is_system()) return ERROR_ACCESS_DENIED;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) return GetLastError();
        who.system_fallback = true;
      }
      handle source(raw); raw = nullptr;
      if (!DuplicateTokenEx(source.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &raw)) return GetLastError();
      token.reset(raw);
      if (who.system_fallback && !SetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session))) return GetLastError();
      DWORD actual_session = wire::invalid_session, required = 0;
      if (!GetTokenInformation(token.get(), TokenSessionId, &actual_session, sizeof(actual_session), &required)) return GetLastError();
      if (actual_session != session) return ERROR_NO_SUCH_LOGON_SESSION;
      TOKEN_STATISTICS stats {};
      if (!GetTokenInformation(token.get(), TokenStatistics, &stats, sizeof(stats), &required)) return GetLastError();
      who.authentication = stats.AuthenticationId;
      required = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
      if (!required) return ERROR_INVALID_DATA;
      std::vector<std::uint8_t> data(required);
      if (!GetTokenInformation(token.get(), TokenUser, data.data(), required, &required)) return GetLastError();
      LPWSTR sid = nullptr;
      if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) return GetLastError();
      who.sid = sid; LocalFree(sid);
      return WTSGetActiveConsoleSessionId() == session ? ERROR_SUCCESS : ERROR_NO_SUCH_LOGON_SESSION;
    }
    std::wstring make_pipe_name() {
      std::array<unsigned char, 16> random {};
      if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
      constexpr wchar_t hex[] = L"0123456789abcdef";
      std::wstring name = L"\\\\.\\pipe\\FoundationDisplay-" + std::to_wstring(GetCurrentProcessId()) + L"-";
      for (auto value : random) { name += hex[value >> 4]; name += hex[value & 15]; }
      return name;
    }
    class bridge_client {
    public:
      ~bridge_client() { std::lock_guard lock(mutex_); stop_locked(); }
      LONG request(wire::operation op, const std::vector<std::uint8_t> &payload, std::vector<std::uint8_t> &response) {
        std::lock_guard transaction_lock(transaction_mutex);
        std::lock_guard lock(mutex_);
        response.clear();
        const DWORD session = pinned_session.value_or(WTSGetActiveConsoleSessionId());
        if (session == wire::invalid_session || WTSGetActiveConsoleSessionId() != session) return ERROR_NO_SUCH_LOGON_SESSION;
        const DWORD ready_error = ensure_locked(session);
        if (ready_error) return static_cast<LONG>(ready_error);
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) { stop_locked(); return ERROR_RETRY; }
        const wire::header request {op, static_cast<std::uint32_t>(payload.size()), ++sequence_, generation_, session, 0};
        const auto deadline = GetTickCount64() + 5000;
        DWORD error = send_packet(pipe_.get(), request, payload, deadline, process_.get());
        wire::header reply;
        if (!error) error = receive_packet(pipe_.get(), reply, response, deadline, process_.get());
        if (!error && !wire::matches(request, reply)) error = ERROR_INVALID_DATA;
        if (!error && WTSGetActiveConsoleSessionId() != session) error = ERROR_NO_SUCH_LOGON_SESSION;
        if (error) {
          note("request failed: winerr=" + std::to_string(error) + "; quarantining helper before retry");
          poisoned_ = true; stop_locked(); response.clear(); return static_cast<LONG>(error);
        }
        return static_cast<LONG>(reply.status);
      }
      void stop() { std::lock_guard lock(mutex_); stop_locked(); }
    private:
      // A timed-out modeset cannot finish behind a newer APPLY. Retain and block
      // if the tracked child does not exit; never kill unrelated processes.
      bool stop_locked() {
        pipe_.reset();
        if (process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
          TerminateProcess(process_.get(), ERROR_OPERATION_ABORTED);
          if (WaitForSingleObject(process_.get(), 2000) != WAIT_OBJECT_0) {
            poisoned_ = true; note("helper has not exited; further requests are blocked"); return false;
          }
        }
        process_.reset(); job_.reset(); identity_.reset(); poisoned_ = false; return true;
      }
      DWORD ensure_locked(DWORD session) {
        if (poisoned_ && !stop_locked()) return ERROR_BUSY;
        identity desired; handle token;
        DWORD error = acquire_identity(session, desired, token);
        if (error) { note("console identity unavailable: winerr=" + std::to_string(error)); return error; }
        if (pinned_session) {
          if (pinned_identity && !(*pinned_identity == desired)) return ERROR_NO_SUCH_LOGON_SESSION;
          pinned_identity = desired;
        }
        if (process_ && identity_ && *identity_ == desired && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT && pipe_) return ERROR_SUCCESS;
        if (!stop_locked()) return ERROR_BUSY;
        const auto exe = std::filesystem::path(module_path()).parent_path() / L"tools" / L"foundation_display_helper.exe";
        std::error_code fs_error;
        if (!std::filesystem::is_regular_file(exe, fs_error)) {
          note("tools/foundation_display_helper.exe is missing; no fallback to the wrong desktop"); return ERROR_FILE_NOT_FOUND;
        }
        const auto pipe_name = make_pipe_name();
        if (pipe_name.empty()) return ERROR_GEN_FAILURE;
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + desired.sid + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
        SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        pipe_.reset(CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 5000, &attributes));
        error = pipe_ ? ERROR_SUCCESS : GetLastError(); LocalFree(descriptor);
        if (error) return error;
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) { error = GetLastError(); stop_locked(); return error; }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
          error = GetLastError(); stop_locked(); return error;
        }
        environment env;
        if (!CreateEnvironmentBlock(&env.data, token.get(), FALSE)) { error = GetLastError(); stop_locked(); return error; }
        const std::wstring command = L"\"" + exe.wstring() + L"\" --pipe \"" + pipe_name + L"\" --parent " +
          std::to_wstring(GetCurrentProcessId()) + L" --session " + std::to_wstring(session);
        STARTUPINFOW si {}; si.cb = sizeof(si); PROCESS_INFORMATION pi {};
        const auto launch = [&](const wchar_t *desktop) {
          std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
          si.lpDesktop = const_cast<wchar_t *>(desktop); pi = {};
          return CreateProcessAsUserW(token.get(), exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data, exe.parent_path().c_str(), &si, &pi);
        };
        BOOL started = launch(L"winsta0\\default");
        if (!started && desired.system_fallback) started = launch(L"winsta0\\winlogon");
        if (!started) { error = GetLastError(); stop_locked(); return error; }
        process_.reset(pi.hProcess); handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) { error = GetLastError(); stop_locked(); return error; }
        if (ResumeThread(thread.get()) == MAXDWORD) { error = GetLastError(); stop_locked(); return error; }
        const auto deadline = GetTickCount64() + 5000; bool connected = false;
        while (GetTickCount64() < deadline) {
          handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
          if (!event) { error = GetLastError(); break; }
          OVERLAPPED ov {}; ov.hEvent = event.get();
          const BOOL immediate = ConnectNamedPipe(pipe_.get(), &ov);
          error = immediate ? ERROR_SUCCESS : GetLastError();
          if (error == ERROR_IO_PENDING) {
            HANDLE events[] = {event.get(), process_.get()}; const auto now = GetTickCount64();
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, now < deadline ? static_cast<DWORD>(deadline - now) : 0);
            if (wait != WAIT_OBJECT_0) {
              error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE;
              CancelIoEx(pipe_.get(), &ov); DWORD unused = 0; GetOverlappedResult(pipe_.get(), &ov, &unused, TRUE); break;
            }
            DWORD unused = 0;
            error = GetOverlappedResult(pipe_.get(), &ov, &unused, FALSE) ? ERROR_SUCCESS : GetLastError();
          }
          if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) break;
          ULONG client_pid = 0;
          if (GetNamedPipeClientProcessId(pipe_.get(), &client_pid) && client_pid == pi.dwProcessId) { connected = true; break; }
          DisconnectNamedPipe(pipe_.get()); error = ERROR_ACCESS_DENIED;
        }
        if (!connected) { stop_locked(); return error ? error : ERROR_TIMEOUT; }
        if (WTSGetActiveConsoleSessionId() != session) { stop_locked(); return ERROR_NO_SUCH_LOGON_SESSION; }
        identity_ = desired; ++generation_; if (!generation_) ++generation_; sequence_ = 0;
        note("started console=" + std::to_string(session) + ", pid=" + std::to_string(pi.dwProcessId) +
          (desired.system_fallback ? ", context=SYSTEM-prelogin" : ", context=interactive-user"));
        return ERROR_SUCCESS;
      }
      std::mutex mutex_;
      handle process_, job_, pipe_;
      std::optional<identity> identity_;
      std::uint64_t generation_ {}, sequence_ {};
      bool poisoned_ {};
    };
    bridge_client &client() { static bridge_client instance; return instance; }
    LONG device_info(wire::operation op, DISPLAYCONFIG_DEVICE_INFO_HEADER *packet) {
      if (!packet || packet->size < sizeof(*packet) || packet->size > wire::max_device_info) return ERROR_INVALID_PARAMETER;
      const auto expected_size = packet->size;
      wire::writer out;
      if (!out.bytes(packet, expected_size)) return ERROR_INVALID_PARAMETER;
      std::vector<std::uint8_t> response;
      const LONG status = client().request(op, out.data(), response);
      if (status != ERROR_SUCCESS) return status;
      if (response.size() != expected_size) return ERROR_INVALID_DATA;
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {}; std::memcpy(&header, response.data(), sizeof(header));
      if (header.size != expected_size || header.type != packet->type || header.id != packet->id ||
          header.adapterId.LowPart != packet->adapterId.LowPart || header.adapterId.HighPart != packet->adapterId.HighPart) return ERROR_INVALID_DATA;
      std::memcpy(packet, response.data(), response.size()); return ERROR_SUCCESS;
    }
  }
  void set_log_callback(log_callback callback) { logger.store(callback); }
  void set_enabled(bool value) { std::lock_guard lock(transaction_mutex); feature_enabled.store(value); }
  bool enabled() { static const bool system = process_is_system(); return feature_enabled.load() && system; }
  void shutdown() { client().stop(); }
  transaction_scope::transaction_scope(): transaction_lock_(transaction_mutex), had_pin_(pinned_session.has_value()),
      previous_(pinned_session.value_or(wire::invalid_session)) {
    if (!had_pin_ && enabled()) { pinned_session = WTSGetActiveConsoleSessionId(); pinned_identity.reset(); }
  }
  transaction_scope::~transaction_scope() {
    if (had_pin_) pinned_session = previous_;
    else { pinned_session.reset(); pinned_identity.reset(); }
  }
  LONG get_buffer_sizes(UINT32 flags, UINT32 *paths, UINT32 *modes) {
    if (!enabled()) return ::GetDisplayConfigBufferSizes(flags, paths, modes);
    if (!paths || !modes) return ERROR_INVALID_PARAMETER;
    wire::writer out; out.pod(flags); std::vector<std::uint8_t> response;
    const LONG status = client().request(wire::operation::sizes, out.data(), response);
    if (status != ERROR_SUCCESS) return status;
    UINT32 n = 0, m = 0; wire::reader in(response);
    if (!in.pod(n) || !in.pod(m) || !in.done() || n > wire::max_paths || m > wire::max_modes) return ERROR_INVALID_DATA;
    *paths = n; *modes = m; return ERROR_SUCCESS;
  }
  LONG query_config(UINT32 flags, UINT32 *path_count, DISPLAYCONFIG_PATH_INFO *paths,
                    UINT32 *mode_count, DISPLAYCONFIG_MODE_INFO *modes, DISPLAYCONFIG_TOPOLOGY_ID *topology) {
    if (!enabled()) return ::QueryDisplayConfig(flags, path_count, paths, mode_count, modes, topology);
    if (!path_count || !mode_count || (*path_count && !paths) || (*mode_count && !modes) ||
        *path_count > wire::max_paths || *mode_count > wire::max_modes) return ERROR_INVALID_PARAMETER;
    wire::writer out; out.pod(flags); out.pod(*path_count); out.pod(*mode_count);
    const UINT32 has_topology = topology ? 1 : 0; out.pod(has_topology);
    std::vector<std::uint8_t> response;
    const LONG status = client().request(wire::operation::query, out.data(), response);
    if (status != ERROR_SUCCESS && status != ERROR_INSUFFICIENT_BUFFER) return status;
    wire::reader in(response); UINT32 n = 0, m = 0, t = 0;
    if (!in.pod(n) || !in.pod(m) || !in.pod(t) || n > wire::max_paths || m > wire::max_modes) return ERROR_INVALID_DATA;
    if (status == ERROR_INSUFFICIENT_BUFFER) {
      if (!in.done()) return ERROR_INVALID_DATA;
      *path_count = n; *mode_count = m; return status;
    }
    if (n > *path_count || m > *mode_count) return ERROR_INVALID_DATA;
    std::vector<DISPLAYCONFIG_PATH_INFO> p; std::vector<DISPLAYCONFIG_MODE_INFO> q;
    if (!in.array(p, n, wire::max_paths) || !in.array(q, m, wire::max_modes) || !in.done()) return ERROR_INVALID_DATA;
    if (n) std::memcpy(paths, p.data(), n * sizeof(p[0]));
    if (m) std::memcpy(modes, q.data(), m * sizeof(q[0]));
    *path_count = n; *mode_count = m;
    if (topology) *topology = static_cast<DISPLAYCONFIG_TOPOLOGY_ID>(t);
    return status;
  }
  LONG set_config(UINT32 path_count, DISPLAYCONFIG_PATH_INFO *paths, UINT32 mode_count, DISPLAYCONFIG_MODE_INFO *modes, UINT32 flags) {
    if (!enabled()) return ::SetDisplayConfig(path_count, paths, mode_count, modes, flags);
    wire::writer out; out.pod(flags); out.pod(path_count); out.pod(mode_count);
    if (!out.array(paths, path_count, wire::max_paths) || !out.array(modes, mode_count, wire::max_modes)) return ERROR_INVALID_PARAMETER;
    std::vector<std::uint8_t> response;
    const LONG status = client().request(wire::operation::set_config, out.data(), response);
    return status == ERROR_SUCCESS && !response.empty() ? ERROR_INVALID_DATA : status;
  }
  LONG get_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *packet) {
    return enabled() ? device_info(wire::operation::get_info, packet) : ::DisplayConfigGetDeviceInfo(packet);
  }
  LONG set_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *packet) {
    return enabled() ? device_info(wire::operation::set_info, packet) : ::DisplayConfigSetDeviceInfo(packet);
  }
}
#endif
