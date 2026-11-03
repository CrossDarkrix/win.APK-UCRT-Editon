#include "host_natives.h"
#include "ucrt_cd_bridge.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace winapk::host { namespace {
struct Handle { ucrt_cd_handle cd=nullptr; uint8_t sense[64]{}; uint32_t sense_len=0; std::mutex mutex; };
std::wstring wide(JNIEnv* e,jstring s){const jchar*p=e->GetStringChars(s,nullptr);jsize n=e->GetStringLength(s);std::wstring r((const wchar_t*)p,(size_t)n);e->ReleaseStringChars(s,p);return r;}
jobjectArray JNICALL enumerate(JNIEnv* e,jclass){ucrt_cd_error er{};uint32_t n=0;int r=ucrt_cd_enumerate(nullptr,0,&n,&er);if(r!=0&&r!=7)return nullptr;std::vector<ucrt_cd_device_info>v(n);if(ucrt_cd_enumerate(v.data(),n,&n,&er)!=0)return nullptr;jclass sc=e->FindClass("java/lang/String");auto a=e->NewObjectArray((jsize)n*2,sc,nullptr);for(uint32_t i=0;i<n;i++){e->SetObjectArrayElement(a,i*2,e->NewString((jchar*)v[i].device_path,(jsize)wcslen(v[i].device_path)));e->SetObjectArrayElement(a,i*2+1,e->NewString((jchar*)v[i].friendly_name,(jsize)wcslen(v[i].friendly_name)));}return a;}
jlong JNICALL open(JNIEnv*e,jclass,jstring p){auto w=wide(e,p);auto*h=new Handle;ucrt_cd_error er{};if(ucrt_cd_open(w.c_str(),&h->cd,&er)!=0){delete h;return 0;}return(jlong)(intptr_t)h;}
void JNICALL close(JNIEnv*,jclass,jlong x){auto*h=(Handle*)(intptr_t)x;if(h){if(h->cd)ucrt_cd_close(h->cd);delete h;}}
jbyteArray JNICALL execute(JNIEnv*e,jclass,jlong x,jbyteArray jcdb,jint requested,jint timeout){auto*h=(Handle*)(intptr_t)x;if(!h||!h->cd||!jcdb)return nullptr;std::lock_guard<std::mutex>g(h->mutex);jsize cn=e->GetArrayLength(jcdb);if(cn<1||cn>16)return nullptr;uint8_t cdb[16]{};e->GetByteArrayRegion(jcdb,0,cn,(jbyte*)cdb);uint32_t cap=requested>0?(uint32_t)requested:0;std::vector<uint8_t>out(cap);ucrt_cd_error er{};int32_t r;
 if(cdb[0]==0x03&&h->sense_len){uint32_t n=std::min(cap,h->sense_len);std::copy(h->sense,h->sense+n,out.begin());h->sense_len=0;r=0;out.resize(n);}else if(cdb[0]==0xbe&&cap){int32_t lba=(int32_t)((uint32_t)cdb[2]<<24|(uint32_t)cdb[3]<<16|(uint32_t)cdb[4]<<8|cdb[5]);uint32_t sectors=(uint32_t)cdb[6]<<16|(uint32_t)cdb[7]<<8|cdb[8],len=0;r=ucrt_cd_read_audio(h->cd,lba,sectors,(cdb[9]&2)?1:0,out.data(),cap,&len,&er);out.resize(len);}else{ucrt_cd_scsi_response resp{};r=ucrt_cd_execute_scsi(h->cd,cdb,(uint32_t)cn,cap?UCRT_CD_DATA_FROM_DEVICE:UCRT_CD_DATA_NONE,nullptr,0,cap?out.data():nullptr,cap,static_cast<uint32_t>(std::max<jint>(timeout, static_cast<jint>(1000))),&resp,&er);out.resize(std::min(cap,resp.transferred_length));}
 if(r!=0){h->sense_len=18;std::memset(h->sense,0,64);h->sense[0]=0x70;h->sense[2]=er.sense_key;h->sense[7]=10;h->sense[12]=er.asc;h->sense[13]=er.ascq;return nullptr;}auto a=e->NewByteArray((jsize)out.size());if(!out.empty())e->SetByteArrayRegion(a,0,(jsize)out.size(),(jbyte*)out.data());return a;}
constexpr const char*k="com.macapk.loader.HostUsbCdBridge";const HostMethod m[]={WINAPK_HOST_NATIVE(k,"enumerate","()[Ljava/lang/String;",enumerate),WINAPK_HOST_NATIVE(k,"open","(Ljava/lang/String;)J",open),WINAPK_HOST_NATIVE(k,"close","(J)V",close),WINAPK_HOST_NATIVE(k,"execute","(J[BII)[B",execute)};
} HostMethods native_usb_cd_methods(){return{m,sizeof(m)/sizeof(m[0])};}}
