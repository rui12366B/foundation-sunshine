#ifdef _WIN32
#include "dispatch.h"
namespace display_session_bridge {
  LONG dispatch(wire::operation op, const std::vector<std::uint8_t> &request,
                std::vector<std::uint8_t> &response, bool temporary_system_apply) {
    wire::reader in(request);
    wire::writer out;
    response.clear();
    switch (op) {
      case wire::operation::ping:
      case wire::operation::stop:
        return in.done() ? ERROR_SUCCESS : ERROR_INVALID_DATA;
      case wire::operation::sizes: {
        UINT32 flags = 0, paths = 0, modes = 0;
        if (!in.pod(flags) || !in.done()) return ERROR_INVALID_DATA;
        const LONG status = ::GetDisplayConfigBufferSizes(flags, &paths, &modes);
        if (status != ERROR_SUCCESS) return status;
        if (paths > wire::max_paths || modes > wire::max_modes) return ERROR_NOT_ENOUGH_MEMORY;
        out.pod(paths); out.pod(modes); response = out.take(); return status;
      }
      case wire::operation::query: {
        UINT32 flags = 0, n = 0, m = 0, want_topology = 0;
        if (!in.pod(flags) || !in.pod(n) || !in.pod(m) || !in.pod(want_topology) || !in.done() ||
            n > wire::max_paths || m > wire::max_modes || want_topology > 1) return ERROR_INVALID_DATA;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(n);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(m);
        const auto capacity_n = n, capacity_m = m;
        DISPLAYCONFIG_TOPOLOGY_ID topology {};
        const LONG status = ::QueryDisplayConfig(flags, &n, n ? paths.data() : nullptr,
          &m, m ? modes.data() : nullptr, want_topology ? &topology : nullptr);
        if (status != ERROR_SUCCESS && status != ERROR_INSUFFICIENT_BUFFER) return status;
        if (n > wire::max_paths || m > wire::max_modes) return ERROR_NOT_ENOUGH_MEMORY;
        out.pod(n); out.pod(m); out.pod(static_cast<UINT32>(topology));
        if (status == ERROR_SUCCESS) {
          if (n > capacity_n || m > capacity_m) return ERROR_INVALID_DATA;
          if (!out.array(paths.data(), n, wire::max_paths) || !out.array(modes.data(), m, wire::max_modes)) return ERROR_INVALID_DATA;
        }
        response = out.take(); return status;
      }
      case wire::operation::set_config: {
        UINT32 flags = 0, n = 0, m = 0;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
        if (!in.pod(flags) || !in.pod(n) || !in.pod(m) || !in.array(paths, n, wire::max_paths) ||
            !in.array(modes, m, wire::max_modes) || !in.done()) return ERROR_INVALID_DATA;
        // Pre-login preparation must not persist a user's layout under SYSTEM.
        if (temporary_system_apply) flags &= ~SDC_SAVE_TO_DATABASE;
        return ::SetDisplayConfig(n, n ? paths.data() : nullptr, m, m ? modes.data() : nullptr, flags);
      }
      case wire::operation::get_info:
      case wire::operation::set_info: {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
        if (!in.pod(header) || header.size != request.size() || header.size < sizeof(header) ||
            header.size > wire::max_device_info) return ERROR_INVALID_DATA;
        std::vector<std::uint64_t> storage((header.size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
        std::memcpy(storage.data(), request.data(), request.size());
        auto *packet = reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER *>(storage.data());
        const LONG status = op == wire::operation::get_info ? ::DisplayConfigGetDeviceInfo(packet) : ::DisplayConfigSetDeviceInfo(packet);
        if (status == ERROR_SUCCESS) {
          if (packet->size != header.size || packet->type != header.type) return ERROR_INVALID_DATA;
          if (!out.bytes(storage.data(), header.size)) return ERROR_INVALID_DATA;
          response = out.take();
        }
        return status;
      }
    }
    return ERROR_INVALID_FUNCTION;
  }
}
#endif
