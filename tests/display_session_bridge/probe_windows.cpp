/** Read-only diagnostic. Does not change display modes or unlock sessions. */
#include "src/platform/windows/display_session_bridge/client.h"
#include <iostream>
#include <string_view>
#include <wtsapi32.h>
int main(int argc,char **argv) {
  using namespace display_session_bridge;
  const bool require_helper=argc==2 && std::string_view(argv[1])=="--require-helper";
  set_log_callback([](const std::string &text){std::cerr << text << '\n';}); set_enabled(true);
  const bool active=enabled();
  std::cout << "adapter=" << (active?"console-helper":"native-nonSYSTEM") << " console=" << WTSGetActiveConsoleSessionId() << '\n';
  if(require_helper && !active) {std::cerr << "An authorized SYSTEM process is required.\n"; return 2;}
  transaction_scope transaction; UINT32 n=0,m=0;
  LONG status=get_buffer_sizes(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE,&n,&m);
  if(status!=ERROR_SUCCESS) {std::cerr << "sizes winerr=" << status << '\n'; shutdown(); return 1;}
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(n); std::vector<DISPLAYCONFIG_MODE_INFO> modes(m);
  status=query_config(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE,&n,n?paths.data():nullptr,&m,m?modes.data():nullptr,nullptr);
  std::cout << "query winerr=" << status << " paths=" << n << " modes=" << m << '\n'; shutdown();
  return status==ERROR_SUCCESS?0:1;
}
