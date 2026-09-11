#pragma once
#ifdef _WIN32
#include "protocol.h"
#include "win32_support.h"
namespace display_session_bridge {
  // Typed CCD calls only. No command execution, filenames, or registry requests.
  LONG dispatch(wire::operation op, const std::vector<std::uint8_t> &request,
                std::vector<std::uint8_t> &response, bool temporary_system_apply);
}
#endif
