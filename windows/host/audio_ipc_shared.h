#pragma once
#include <windows.h>
#include <cstdint>
namespace winapk::audio_ipc {
constexpr uint32_t kMagic=0x41504355u; constexpr uint32_t kAbi=2; constexpr uint64_t kCapacityFrames=768000;
constexpr uint32_t kStopped=0,kRunning=1,kShutdown=2;
struct alignas(64) Header {
 uint32_t magic,abi,header_bytes,frame_bytes; uint64_t capacity_frames;
 volatile LONG64 write_pos,read_pos,session_id,generation;
 volatile LONG rate,alt,state,host_ready,host_failed;
 volatile LONG64 producer_seq,consumer_seq,underruns,overruns,stale_drops;
 volatile LONG64 producer_gap_max_us,consumer_gap_max_us,wasapi_reconfigures,heartbeat,parent_heartbeat;
 DWORD parent_pid,host_pid,host_priority,last_error; wchar_t data_event[96],space_event[96];
};
inline float* pcm(Header* h){return reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(h)+sizeof(Header));}
inline size_t mapping_bytes(){return sizeof(Header)+size_t(kCapacityFrames)*2*sizeof(float);}
}
