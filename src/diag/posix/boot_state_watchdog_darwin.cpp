#if defined(__APPLE__)
// Darwin heartbeat watchdog. Linux's /proc + tgkill stack dump is unavailable.
#include "boot_state_watchdog.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>
namespace BootState {
namespace {
std::atomic<bool> stopped{false},started{false},quitting{false};
std::atomic<unsigned> frames{0};
std::thread worker;
void Write(const char* state,unsigned age) {
 if(FILE* f=std::fopen("standalone_boot_state.txt","w")){std::fprintf(f,"STATE=%s pid=%d uptime=%u frame=%u stale=0\n",state,getpid(),age,frames.load());std::fclose(f);}
}
}
void Start(){stopped=false;worker=std::thread([]{unsigned age=0;while(!stopped){Write(quitting?"QUITTING":started?"RUNNING":"BOOTING",age++);std::this_thread::sleep_for(std::chrono::seconds(1));}});}
void MarkStartupComplete(){started=true;std::fprintf(stderr,"[BootState] RUNNING (Engine::Startup returned)\n");}
void Stop(){stopped=true;if(worker.joinable())worker.join();Write("EXITED",0);}
}
extern "C" void BootState_MarkFrame(){++BootState::frames;}
extern "C" void BootState_MarkQuitting(){BootState::quitting=true;}
#endif
