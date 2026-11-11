#include "audio_ipc_shared.h"
#include "audio_engine.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace winapk::audio_ipc;
struct HostBridgeQos {
 bool process_throttling_off=false;
 bool thread_throttling_off=false;
 bool mmcss_high=false;
 bool fallback_highest=false;
 bool priority_boost_disabled=false;
 DWORD process_error=ERROR_SUCCESS;
 DWORD thread_error=ERROR_SUCCESS;
 DWORD task_index=0;
 HANDLE mmcss=nullptr;
};
HostBridgeQos configure_host_bridge_qos(){
 HostBridgeQos state;
 HMODULE kernel32=GetModuleHandleW(L"kernel32.dll");
 using SetProcessInformationFn=BOOL (WINAPI*)(HANDLE,PROCESS_INFORMATION_CLASS,LPVOID,DWORD);
 using SetThreadInformationFn=BOOL (WINAPI*)(HANDLE,THREAD_INFORMATION_CLASS,LPVOID,DWORD);
 auto set_process_info=kernel32?reinterpret_cast<SetProcessInformationFn>(GetProcAddress(kernel32,"SetProcessInformation")):nullptr;
 auto set_thread_info=kernel32?reinterpret_cast<SetThreadInformationFn>(GetProcAddress(kernel32,"SetThreadInformation")):nullptr;
 if(set_process_info){
  PROCESS_POWER_THROTTLING_STATE power{};
  power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  power.StateMask=0;
  SetLastError(ERROR_SUCCESS);
  state.process_throttling_off=set_process_info(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power))!=FALSE;
  state.process_error=state.process_throttling_off?ERROR_SUCCESS:GetLastError();
 }else state.process_error=ERROR_CALL_NOT_IMPLEMENTED;
 if(set_thread_info){
  THREAD_POWER_THROTTLING_STATE power{};
  power.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  power.StateMask=0;
  SetLastError(ERROR_SUCCESS);
  state.thread_throttling_off=set_thread_info(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power))!=FALSE;
  state.thread_error=state.thread_throttling_off?ERROR_SUCCESS:GetLastError();
 }else state.thread_error=ERROR_CALL_NOT_IMPLEMENTED;
 HMODULE avrt=LoadLibraryW(L"avrt.dll");
 using AvSetFn=HANDLE (WINAPI*)(LPCWSTR,LPDWORD);
 using AvPriorityFn=BOOL (WINAPI*)(HANDLE,int);
 auto av_set=avrt?reinterpret_cast<AvSetFn>(GetProcAddress(avrt,"AvSetMmThreadCharacteristicsW")):nullptr;
 auto av_priority=avrt?reinterpret_cast<AvPriorityFn>(GetProcAddress(avrt,"AvSetMmThreadPriority")):nullptr;
 state.mmcss=av_set?av_set(L"Pro Audio",&state.task_index):nullptr;
 state.mmcss_high=state.mmcss&&av_priority&&av_priority(state.mmcss,1)!=FALSE;
 if(!state.mmcss)state.fallback_highest=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST)!=FALSE;
 state.priority_boost_disabled=SetThreadPriorityBoost(GetCurrentThread(),TRUE)!=FALSE;
 std::fprintf(stderr,"[I] Phase20-B2.3.6.4.11: host bridge QoS processThrottleOff=%d processError=%lu threadThrottleOff=%d threadError=%lu mmcss=%d mmcssHigh=%d taskIndex=%lu fallbackHighest=%d boostDisabled=%d threadPriority=%d processPriority=0x%08lX\\n",state.process_throttling_off?1:0,state.process_error,state.thread_throttling_off?1:0,state.thread_error,state.mmcss?1:0,state.mmcss_high?1:0,state.task_index,state.fallback_highest?1:0,state.priority_boost_disabled?1:0,GetThreadPriority(GetCurrentThread()),GetPriorityClass(GetCurrentProcess()));
 return state;
}
static uint64_t qpc_us(){LARGE_INTEGER q,f;QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return uint64_t(q.QuadPart*1000000ULL/f.QuadPart);}
int wmain(int argc,wchar_t** argv){
 const HostBridgeQos bridge_qos=configure_host_bridge_qos();
 (void)bridge_qos;
 if(argc<3)return 2; const wchar_t* map_name=argv[1]; DWORD parent=wcstoul(argv[2],nullptr,10);
 SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS); HANDLE ph=OpenProcess(SYNCHRONIZE,FALSE,parent);
 HANDLE mh=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,map_name); if(!mh)return 3;
 auto* h=static_cast<Header*>(MapViewOfFile(mh,FILE_MAP_ALL_ACCESS,0,0,mapping_bytes())); if(!h||h->magic!=kMagic||h->abi!=kAbi)return 4;
 HANDLE data=OpenEventW(SYNCHRONIZE,FALSE,h->data_event),space=OpenEventW(EVENT_MODIFY_STATE,FALSE,h->space_event);
 h->host_pid=GetCurrentProcessId();h->host_priority=GetPriorityClass(GetCurrentProcess());InterlockedExchange(&h->host_ready,1);
 std::fprintf(stderr,"[I] Phase20-B2.3.6.4: audio host ready pid=%lu parent=%lu priority=0x%08lx mapping=%ls sessionOwner=audio-host wasapiOwner=audio-host generationOwner=audio-host\n",h->host_pid,parent,h->host_priority,map_name);
 std::shared_ptr<winapk::audio::Stream> stream; LONG64 active_gen=-1; LONG active_rate=0; uint64_t last=qpc_us(),last_stat=last; uint64_t bridge_batches=0,bridge_frames=0,bridge_short_writes=0; std::vector<float> tmp;
 for(;;){
  if(ph&&WaitForSingleObject(ph,0)==WAIT_OBJECT_0)break; if(h->state==kShutdown)break;
  LONG64 gen=InterlockedCompareExchange64(&h->generation,0,0); LONG rate=InterlockedCompareExchange(&h->rate,0,0);
  if(h->state==kRunning&&rate>0&&(gen!=active_gen||rate!=active_rate||!stream)){
   if(stream){winapk::audio::stream_stop(stream.get());winapk::audio::stream_release(stream);stream.reset();}
   InterlockedExchange64(&h->read_pos,InterlockedCompareExchange64(&h->write_pos,0,0));
   const int source_buffer_frames=(std::max)(static_cast<int>(rate)*4,16384);
   stream=winapk::audio::usb_direct_stream_create(static_cast<int>(rate),2,4,source_buffer_frames);
   if(stream)winapk::audio::stream_play(stream.get()); else {h->last_error=GetLastError();InterlockedExchange(&h->host_failed,1);}
   active_gen=gen;active_rate=rate;InterlockedIncrement64(&h->wasapi_reconfigures);
   std::fprintf(stderr,"[I] Phase20-B2.3.6.4: CONFIGURE_COMMIT session=%lld generation=%lld rate=%ld alt=%ld stream=%s\n",(long long)h->session_id,(long long)gen,rate,h->alt,stream?"ok":"failed");
   std::fprintf(stderr,"[I] Phase20-B2.3.6.4.14: low-spec buffer sourceFrames=%d sourceMs=4000 sharedCapacityFrames=%llu sharedMaxMsAt192k=4000 prerollMs=1000 prerollTimeoutMs=3000\n",source_buffer_frames,(unsigned long long)h->capacity_frames);
  }
  LONG64 r=InterlockedCompareExchange64(&h->read_pos,0,0),w=InterlockedCompareExchange64(&h->write_pos,0,0); uint64_t avail=w>=r?uint64_t(w-r):0;
  if(stream&&avail){size_t n=size_t((std::min<uint64_t>)(avail,32768));tmp.resize(n*2);float* ring=pcm(h);for(size_t i=0;i<n;i++){size_t p=size_t((uint64_t(r)+i)%h->capacity_frames)*2;tmp[i*2]=ring[p];tmp[i*2+1]=ring[p+1];}
   size_t done=winapk::audio::stream_write(stream.get(),tmp.data(),tmp.size(),4,true)/2;++bridge_batches;bridge_frames+=done;if(done<n)++bridge_short_writes;InterlockedExchange64(&h->read_pos,r+LONG64(done));InterlockedIncrement64(&h->consumer_seq);if(space)SetEvent(space);
   uint64_t now=qpc_us(),gap=now-last;last=now;LONG64 old=h->consumer_gap_max_us;while(LONG64(gap)>old&&InterlockedCompareExchange64(&h->consumer_gap_max_us,LONG64(gap),old)!=old)old=h->consumer_gap_max_us;
  } else {if(h->state==kRunning&&stream)InterlockedIncrement64(&h->underruns);if(data)WaitForSingleObject(data,2);else Sleep(1);}
  InterlockedIncrement64(&h->heartbeat); uint64_t now=qpc_us();
  // Phase20-B2.3.6.4.17: publish the current output latency (~10 Hz) so the
  // runtime can delay URB completions by it (progress/spectrum sync).
  {static uint64_t last_latency_publish=0;if(now-last_latency_publish>=100000){last_latency_publish=now;InterlockedExchange64(&h->output_latency_frames,stream?winapk::audio::stream_output_latency_frames(stream.get()):0);}}
  if(now-last_stat>=30000000){last_stat=now;std::fprintf(stderr,"[I] Phase20-B2.3.6.4: audio host summary pid=%lu session=%lld generation=%lld rate=%ld readable=%lld underruns=%lld overruns=%lld stale=%lld consumerGapMaxUs=%lld reconfigures=%lld parentHeartbeat=%lld bridgeBatches=%llu bridgeFrames=%llu bridgeShortWrites=%llu\n",h->host_pid,(long long)h->session_id,(long long)h->generation,h->rate,(long long)(h->write_pos-h->read_pos),(long long)h->underruns,(long long)h->overruns,(long long)h->stale_drops,(long long)h->consumer_gap_max_us,(long long)h->wasapi_reconfigures,(long long)h->parent_heartbeat,(unsigned long long)bridge_batches,(unsigned long long)bridge_frames,(unsigned long long)bridge_short_writes);}
 }
 if(stream){winapk::audio::stream_stop(stream.get());winapk::audio::stream_release(stream);}InterlockedExchange(&h->host_ready,0);if(h)UnmapViewOfFile(h);if(mh)CloseHandle(mh);if(ph)CloseHandle(ph);return 0;
}
