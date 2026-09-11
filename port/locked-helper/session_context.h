/** GPL-3.0. Pin CCD execution to the intended console and Windows logon.
 * A locked console needs its service's own SYSTEM worker on the input desktop.
 * This never unlocks the workstation or obtains another session's credentials.
 */
#pragma once
#ifdef _WIN32
#include "win32_support.h"
#include <sddl.h>
#include <wtsapi32.h>
namespace display_session_bridge {
  enum class execution_context { user, prelogin, locked_console };
  struct console_principal {
    DWORD session {0xffffffffu};
    LUID authentication {};
    std::wstring sid;
    bool same_login(const console_principal &other) const {
      return session == other.session && authentication.LowPart == other.authentication.LowPart &&
        authentication.HighPart == other.authentication.HighPart && sid == other.sid;
    }
  };
  inline bool uses_system(execution_context context) { return context != execution_context::user; }
  inline const wchar_t *context_argument(execution_context context) {
    switch (context) {
      case execution_context::user: return L"user";
      case execution_context::prelogin: return L"prelogin";
      case execution_context::locked_console: return L"locked";
    }
    return L"invalid";
  }
  inline const char *context_description(execution_context context) {
    switch (context) {
      case execution_context::user: return "interactive-user";
      case execution_context::prelogin: return "SYSTEM-prelogin";
      case execution_context::locked_console: return "SYSTEM-locked-console";
    }
    return "invalid";
  }
  inline DWORD read_token_principal(HANDLE token, console_principal &who) {
    DWORD required = 0;
    TOKEN_STATISTICS stats {};
    if (!GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &required)) return GetLastError();
    who.authentication = stats.AuthenticationId;
    required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (!required) return ERROR_INVALID_DATA;
    std::vector<std::uint8_t> bytes(required);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), required, &required)) return GetLastError();
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &text)) return GetLastError();
    who.sid = text; LocalFree(text);
    return ERROR_SUCCESS;
  }
  inline DWORD inspect_console(DWORD session, bool &has_user, bool &locked) {
    has_user = false; locked = false;
    if (session == 0xffffffffu || WTSGetActiveConsoleSessionId() != session) return ERROR_NO_SUCH_LOGON_SESSION;
    LPWSTR data = nullptr; DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &data, &bytes)) return GetLastError();
    has_user = data && bytes >= sizeof(wchar_t) && data[0] != L'\0';
    if (data) WTSFreeMemory(data);
    if (has_user) {
      data = nullptr; bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &data, &bytes)) return GetLastError();
      bool known = false;
      if (data && bytes >= sizeof(WTSINFOEXW)) {
        const auto *info = reinterpret_cast<const WTSINFOEXW *>(data);
        if (info->Level == 1) {
          const auto flags = info->Data.WTSInfoExLevel1.SessionFlags;
          known = flags == WTS_SESSIONSTATE_LOCK || flags == WTS_SESSIONSTATE_UNLOCK;
          locked = flags == WTS_SESSIONSTATE_LOCK;
        }
      }
      if (data) WTSFreeMemory(data);
      if (!known) return ERROR_NOT_READY;
    }
    return WTSGetActiveConsoleSessionId() == session ? ERROR_SUCCESS : ERROR_NO_SUCH_LOGON_SESSION;
  }
  inline DWORD verify_console_context(execution_context context, const console_principal &expected, bool child) {
    const bool system = process_is_system();
    if (child && system != uses_system(context)) return ERROR_ACCESS_DENIED;
    bool has_user = false, locked = false;
    DWORD error = inspect_console(expected.session, has_user, locked);
    if (error) return error;
    if (context == execution_context::prelogin) {
      if (!system || has_user) return ERROR_NO_SUCH_LOGON_SESSION;
    } else {
      if (!has_user || locked != (context == execution_context::locked_console)) return ERROR_RETRY;
    }
    HANDLE raw = nullptr;
    if (has_user && system) {
      // SYSTEM requests are still bound to the selected user's actual logon ID.
      // An unavailable user token is never permission to choose a different user.
      if (!WTSQueryUserToken(expected.session, &raw)) return GetLastError();
    } else {
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return GetLastError();
    }
    handle token(raw);
    console_principal actual; actual.session = expected.session;
    error = read_token_principal(token.get(), actual);
    if (error) return error;
    if (!expected.same_login(actual)) return ERROR_NO_SUCH_LOGON_SESSION;
    return WTSGetActiveConsoleSessionId() == expected.session ? ERROR_SUCCESS : ERROR_NO_SUCH_LOGON_SESSION;
  }
}
#endif
