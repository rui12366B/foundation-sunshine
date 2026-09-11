/** Console-scoped display API adapter. Opt-in, SYSTEM hosts only. */
#pragma once
#ifdef _WIN32
#include "win32_support.h"
#include <mutex>
namespace display_session_bridge {
  using log_callback = void (*)(const std::string &);
  void set_log_callback(log_callback callback);
  void set_enabled(bool enabled);
  bool enabled();
  void shutdown();
  // Pin one high-level APPLY/RESTORE to a console and login identity.
  class transaction_scope {
  public:
    transaction_scope();
    ~transaction_scope();
    transaction_scope(const transaction_scope &) = delete;
    transaction_scope &operator=(const transaction_scope &) = delete;
  private:
    std::unique_lock<std::recursive_mutex> transaction_lock_;
    bool had_pin_ {};
    DWORD previous_ {0xffffffffu};
  };
  LONG get_buffer_sizes(UINT32 flags, UINT32 *paths, UINT32 *modes);
  LONG query_config(UINT32 flags, UINT32 *path_count, DISPLAYCONFIG_PATH_INFO *paths,
                    UINT32 *mode_count, DISPLAYCONFIG_MODE_INFO *modes, DISPLAYCONFIG_TOPOLOGY_ID *topology);
  LONG set_config(UINT32 path_count, DISPLAYCONFIG_PATH_INFO *paths, UINT32 mode_count, DISPLAYCONFIG_MODE_INFO *modes, UINT32 flags);
  LONG get_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *packet);
  LONG set_device_info(DISPLAYCONFIG_DEVICE_INFO_HEADER *packet);
}
#endif
