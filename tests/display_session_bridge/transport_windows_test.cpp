/** Win32 pipe and parser tests: no display modes are changed. */
#include "src/platform/windows/display_session_bridge/transport.h"
#include "src/platform/windows/display_session_bridge/dispatch.h"
#include <thread>
#include <iostream>
#include <cstdlib>
using namespace display_session_bridge;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x " winerr=" << GetLastError() << '\n'; std::exit(1); } } while(0)
struct pipe_pair {
  handle server,client;
  pipe_pair() {
    static unsigned index=0;
    const std::wstring name=L"\\\\.\\pipe\\FoundationTransportTest-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++index);
    server.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr)); CHECK(server);
    handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); CHECK(event); OVERLAPPED ov{}; ov.hEvent=event.get();
    BOOL immediate=ConnectNamedPipe(server.get(),&ov); DWORD error=immediate?ERROR_SUCCESS:GetLastError();
    CHECK(immediate || error==ERROR_IO_PENDING || error==ERROR_PIPE_CONNECTED);
    client.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr)); CHECK(client);
    if(error==ERROR_IO_PENDING) { CHECK(WaitForSingleObject(event.get(),2000)==WAIT_OBJECT_0); DWORD n=0; CHECK(GetOverlappedResult(server.get(),&ov,&n,FALSE)); }
  }
};
int main() {
  { pipe_pair p; std::vector<std::uint8_t> payload(128*1024,0x5a), received;
    wire::header h{wire::operation::ping,static_cast<std::uint32_t>(payload.size()),1,1,1,0}, reply;
    DWORD written=ERROR_GEN_FAILURE;
    std::thread writer([&]{written=send_packet(p.client.get(),h,payload,GetTickCount64()+5000);});
    const DWORD read=receive_packet(p.server.get(),reply,received,GetTickCount64()+5000); writer.join();
    CHECK(written==ERROR_SUCCESS && read==ERROR_SUCCESS && wire::matches(h,reply) && payload==received);
  }
  { pipe_pair p; std::uint8_t byte=0; CHECK(pipe_io(p.server.get(),&byte,1,false,GetTickCount64()+25)==ERROR_TIMEOUT);
    std::uint8_t value=42; CHECK(pipe_io(p.client.get(),&value,1,true,GetTickCount64()+1000)==ERROR_SUCCESS);
    CHECK(pipe_io(p.server.get(),&byte,1,false,GetTickCount64()+1000)==ERROR_SUCCESS && byte==42);
  }
  { pipe_pair p; p.client.reset(); std::uint8_t byte=0;
    CHECK(pipe_io(p.server.get(),&byte,1,false,GetTickCount64()+1000)==ERROR_BROKEN_PIPE); }
  std::vector<std::uint8_t> response;
  CHECK(dispatch(wire::operation::ping,{},response,false)==ERROR_SUCCESS && response.empty());
  CHECK(dispatch(wire::operation::ping,{1},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(wire::operation::sizes,{},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(wire::operation::query,{},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(wire::operation::set_config,{},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(wire::operation::get_info,{1,2,3},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(wire::operation::set_info,{1,2,3},response,false)==ERROR_INVALID_DATA);
  CHECK(dispatch(static_cast<wire::operation>(100),{},response,false)==ERROR_INVALID_FUNCTION);
  std::cout << "Win32 transport/dispatch passed: large frame, timeout recovery, disconnect, malformed requests. No modesets.\n";
}
