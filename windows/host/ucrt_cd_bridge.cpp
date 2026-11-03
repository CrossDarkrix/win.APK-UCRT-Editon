#include "ucrt_cd_bridge.h"

#include <windows.h>
#include <winioctl.h>
#include <devioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <ntddscsi.h>
#include <initguid.h>
#include <ntddstor.h>
#include <devpkey.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>

namespace {
struct BridgeHandle { HANDLE value = INVALID_HANDLE_VALUE; };

struct SptdPacket {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler;
    UCHAR sense[UCRT_CD_MAX_SENSE];
};

void clear_error(ucrt_cd_error* e) {
    if (e != nullptr) std::memset(e, 0, sizeof(*e));
}

std::string win32_message(DWORD code) {
    if (code == 0) return {};
    LPSTR p = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageA(flags, nullptr, code, 0, reinterpret_cast<LPSTR>(&p), 0, nullptr);
    std::string s = (n && p) ? std::string(p, n) : ("Win32 error " + std::to_string(code));
    if (p) LocalFree(p);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

int32_t fail(ucrt_cd_error* e, int32_t result, DWORD win32, const std::string& msg,
             uint8_t status=0, uint8_t key=0, uint8_t asc=0, uint8_t ascq=0) {
    if (e) {
        clear_error(e);
        e->result=result; e->win32_error=win32; e->scsi_status=status;
        e->sense_key=key; e->asc=asc; e->ascq=ascq;
        std::snprintf(e->message, sizeof(e->message), "%s", msg.c_str());
    }
    return result;
}

void copy_wide(wchar_t* dst, size_t cap, const std::wstring& src) {
    if (!dst || cap == 0) return;
    const size_t n = std::min(cap - 1, src.size());
    std::wmemcpy(dst, src.data(), n); dst[n]=L'\0';
}

std::wstring property_string(HDEVINFO set, SP_DEVINFO_DATA& data, const DEVPROPKEY& key) {
    DEVPROPTYPE type=0; DWORD required=0;
    SetupDiGetDevicePropertyW(set, &data, &key, &type, nullptr, 0, &required, 0);
    if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER || required < sizeof(wchar_t)) return {};
    std::vector<BYTE> buf(required);
    if (!SetupDiGetDevicePropertyW(set, &data, &key, &type, buf.data(), required, nullptr, 0)) return {};
    return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
}

void parse_sense(const uint8_t* s, uint32_t n, uint8_t& key, uint8_t& asc, uint8_t& ascq) {
    key=asc=ascq=0;
    if (!s || n < 2) return;
    const uint8_t response=s[0] & 0x7f;
    if ((response==0x70 || response==0x71) && n>=14) { key=s[2]&0x0f; asc=s[12]; ascq=s[13]; }
    else if ((response==0x72 || response==0x73) && n>=4) { key=s[1]&0x0f; asc=s[2]; ascq=s[3]; }
}

int32_t execute_internal(BridgeHandle* h, const uint8_t* cdb, uint32_t cdb_length, int32_t direction,
                         const uint8_t* out, uint32_t out_len, uint8_t* in, uint32_t in_cap,
                         uint32_t timeout_ms, ucrt_cd_scsi_response* response, ucrt_cd_error* error) {
    clear_error(error);
    if (!h || h->value==INVALID_HANDLE_VALUE || !cdb || cdb_length==0 || cdb_length>16)
        return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"Invalid handle or CDB");
    if (direction<0 || direction>2) return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"Invalid transfer direction");
    if (direction==UCRT_CD_DATA_FROM_DEVICE && in_cap && !in) return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"Input buffer is null");
    if (direction==UCRT_CD_DATA_TO_DEVICE && out_len && !out) return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"Output buffer is null");

    const uint32_t data_len = direction==UCRT_CD_DATA_FROM_DEVICE ? in_cap : (direction==UCRT_CD_DATA_TO_DEVICE ? out_len : 0u);
    void* aligned = nullptr;
    if (data_len) {
        aligned=_aligned_malloc(data_len, 4096);
        if (!aligned) return fail(error,UCRT_CD_INTERNAL_ERROR,0,"Aligned buffer allocation failed");
        std::memset(aligned,0,data_len);
        if (direction==UCRT_CD_DATA_TO_DEVICE) std::memcpy(aligned,out,data_len);
    }

    SptdPacket packet{};
    packet.sptd.Length=sizeof(SCSI_PASS_THROUGH_DIRECT);
    packet.sptd.CdbLength=static_cast<UCHAR>(cdb_length);
    packet.sptd.SenseInfoLength=static_cast<UCHAR>(sizeof(packet.sense));
    packet.sptd.DataIn = direction==UCRT_CD_DATA_FROM_DEVICE ? SCSI_IOCTL_DATA_IN :
                         direction==UCRT_CD_DATA_TO_DEVICE ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_UNSPECIFIED;
    packet.sptd.DataTransferLength=data_len;
    packet.sptd.TimeOutValue=std::max<DWORD>(1u,(timeout_ms+999u)/1000u);
    packet.sptd.DataBuffer=aligned;
    packet.sptd.SenseInfoOffset=offsetof(SptdPacket,sense);
    std::memcpy(packet.sptd.Cdb,cdb,cdb_length);

    DWORD returned=0;
    const BOOL ok=DeviceIoControl(h->value,IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &packet,sizeof(packet),&packet,sizeof(packet),&returned,nullptr);
    const DWORD we=ok ? 0 : GetLastError();

    uint8_t key=0,asc=0,ascq=0;
    const uint32_t sense_len=std::min<uint32_t>(packet.sptd.SenseInfoLength,sizeof(packet.sense));
    parse_sense(packet.sense,sense_len,key,asc,ascq);
    if (response) {
        std::memset(response,0,sizeof(*response));
        response->scsi_status=packet.sptd.ScsiStatus;
        response->sense_length=sense_len;
        response->transferred_length=packet.sptd.DataTransferLength;
        std::memcpy(response->sense,packet.sense,sense_len);
    }
    if (ok && direction==UCRT_CD_DATA_FROM_DEVICE && in && aligned) {
        const uint32_t n = std::min<uint32_t>(
            in_cap, static_cast<uint32_t>(packet.sptd.DataTransferLength));
        std::memcpy(in,aligned,n);
    }
    if (aligned) _aligned_free(aligned);

    if (!ok) return fail(error, we==ERROR_ACCESS_DENIED?UCRT_CD_ACCESS_DENIED:UCRT_CD_IO_FAILED,
                          we,"DeviceIoControl failed: "+win32_message(we),packet.sptd.ScsiStatus,key,asc,ascq);
    if (packet.sptd.ScsiStatus != 0)
        return fail(error,UCRT_CD_SCSI_FAILED,0,"SCSI CHECK CONDITION or non-zero status",
                    packet.sptd.ScsiStatus,key,asc,ascq);
    return UCRT_CD_OK;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]); }
std::string trim_ascii(const uint8_t* p,size_t n) {
    std::string s(reinterpret_cast<const char*>(p),n);
    while(!s.empty() && (s.back()==' ' || s.back()=='\0')) s.pop_back();
    size_t first=0; while(first<s.size() && s[first]==' ') ++first;
    return s.substr(first);
}
}

extern "C" {
uint32_t UCRT_CD_CALL ucrt_cd_get_abi_version(void) { return UCRT_CD_ABI_VERSION; }
const char* UCRT_CD_CALL ucrt_cd_result_name(int32_t r) {
    switch(r){case 0:return "OK";case 1:return "INVALID_ARGUMENT";case 2:return "NOT_FOUND";case 3:return "ACCESS_DENIED";case 4:return "OPEN_FAILED";case 5:return "IO_FAILED";case 6:return "SCSI_FAILED";case 7:return "BUFFER_TOO_SMALL";case 8:return "PARSE_FAILED";case 9:return "UNSUPPORTED";default:return "INTERNAL_ERROR";}
}

int32_t UCRT_CD_CALL ucrt_cd_enumerate(ucrt_cd_device_info* devices,uint32_t capacity,uint32_t* count,ucrt_cd_error* error) {
    clear_error(error); if(!count) return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"count is null");
    *count=0;
    HDEVINFO set=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_CDROM,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE){DWORD e=GetLastError();return fail(error,UCRT_CD_IO_FAILED,e,"SetupDiGetClassDevsW failed: "+win32_message(e));}
    uint32_t total=0;
    for(DWORD i=0;;++i){
        SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_DEVINTERFACE_CDROM,i,&iface)){
            DWORD e=GetLastError(); if(e==ERROR_NO_MORE_ITEMS) break;
            SetupDiDestroyDeviceInfoList(set); return fail(error,UCRT_CD_IO_FAILED,e,"SetupDiEnumDeviceInterfaces failed: "+win32_message(e));
        }
        DWORD required=0; SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&required,nullptr);
        if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || required==0) continue;
        std::vector<BYTE> raw(required); auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize=sizeof(dev);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,required,nullptr,&dev)) continue;
        if(devices && total<capacity){
            std::memset(&devices[total],0,sizeof(devices[total]));
            copy_wide(devices[total].device_path,UCRT_CD_MAX_PATH,detail->DevicePath);
            std::wstring name=property_string(set,dev,DEVPKEY_Device_FriendlyName);
            if(name.empty()) name=property_string(set,dev,DEVPKEY_Device_DeviceDesc);
            if(name.empty()) name=L"Optical drive";
            copy_wide(devices[total].friendly_name,UCRT_CD_MAX_NAME,name);
        }
        ++total;
    }
    SetupDiDestroyDeviceInfoList(set); *count=total;
    if(devices && capacity<total) return fail(error,UCRT_CD_BUFFER_TOO_SMALL,0,"Device array is too small");
    return UCRT_CD_OK;
}

int32_t UCRT_CD_CALL ucrt_cd_open(const wchar_t* path,ucrt_cd_handle* handle,ucrt_cd_error* error){
    clear_error(error); if(!path||!handle) return fail(error,UCRT_CD_INVALID_ARGUMENT,0,"path or handle is null"); *handle=nullptr;
    HANDLE h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){DWORD e=GetLastError();return fail(error,e==ERROR_ACCESS_DENIED?UCRT_CD_ACCESS_DENIED:UCRT_CD_OPEN_FAILED,e,"CreateFileW failed: "+win32_message(e));}
    auto* b=new(std::nothrow) BridgeHandle{h}; if(!b){CloseHandle(h);return fail(error,UCRT_CD_INTERNAL_ERROR,0,"Handle allocation failed");}
    *handle=b; return UCRT_CD_OK;
}
void UCRT_CD_CALL ucrt_cd_close(ucrt_cd_handle handle){auto* h=static_cast<BridgeHandle*>(handle);if(h){if(h->value!=INVALID_HANDLE_VALUE)CloseHandle(h->value);delete h;}}

int32_t UCRT_CD_CALL ucrt_cd_execute_scsi(ucrt_cd_handle handle,const uint8_t* cdb,uint32_t cdb_len,int32_t dir,const uint8_t* out,uint32_t out_len,uint8_t* in,uint32_t in_cap,uint32_t timeout,ucrt_cd_scsi_response* resp,ucrt_cd_error* err){
    return execute_internal(static_cast<BridgeHandle*>(handle),cdb,cdb_len,dir,out,out_len,in,in_cap,timeout,resp,err);
}
int32_t UCRT_CD_CALL ucrt_cd_test_unit_ready(ucrt_cd_handle h,ucrt_cd_error* e){const uint8_t cdb[6]={0};return ucrt_cd_execute_scsi(h,cdb,6,UCRT_CD_DATA_NONE,nullptr,0,nullptr,0,10000,nullptr,e);}
int32_t UCRT_CD_CALL ucrt_cd_inquiry_device(ucrt_cd_handle h,ucrt_cd_inquiry* q,ucrt_cd_error* e){
    if(!q)return fail(e,UCRT_CD_INVALID_ARGUMENT,0,"inquiry is null"); uint8_t cdb[6]={0x12,0,0,0,96,0}; uint8_t data[96]{};
    int32_t r=ucrt_cd_execute_scsi(h,cdb,6,UCRT_CD_DATA_FROM_DEVICE,nullptr,0,data,sizeof(data),10000,nullptr,e);if(r)return r;
    std::memset(q,0,sizeof(*q)); auto v=trim_ascii(data+8,8),p=trim_ascii(data+16,16),rev=trim_ascii(data+32,4);
    std::snprintf(q->vendor,sizeof(q->vendor),"%s",v.c_str());std::snprintf(q->product,sizeof(q->product),"%s",p.c_str());std::snprintf(q->revision,sizeof(q->revision),"%s",rev.c_str());return UCRT_CD_OK;
}
int32_t UCRT_CD_CALL ucrt_cd_read_toc(ucrt_cd_handle h,ucrt_cd_toc* toc,ucrt_cd_error* e){
    if(!toc)return fail(e,UCRT_CD_INVALID_ARGUMENT,0,"toc is null"); std::array<uint8_t,804> d{}; uint8_t cdb[10]={0x43,0,0,0,0,0,1,0x03,0x24,0};
    int32_t r=ucrt_cd_execute_scsi(h,cdb,10,UCRT_CD_DATA_FROM_DEVICE,nullptr,0,d.data(),static_cast<uint32_t>(d.size()),15000,nullptr,e);if(r)return r;
    const uint32_t total=std::min<uint32_t>(static_cast<uint32_t>(d.size()),uint32_t((d[0]<<8)|d[1])+2u); if(total<4)return fail(e,UCRT_CD_PARSE_FAILED,0,"TOC response is too short");
    std::memset(toc,0,sizeof(*toc));toc->first_track=d[2];toc->last_track=d[3];
    struct Entry{uint8_t track;uint8_t control;int32_t lba;};std::vector<Entry> entries;
    for(uint32_t o=4;o+8<=total;o+=8) entries.push_back({d[o+2],uint8_t((d[o+1]>>4)&0x0f),static_cast<int32_t>(be32(d.data()+o+4))});
    auto lead=std::find_if(entries.begin(),entries.end(),[](const Entry& x){return x.track==0xaa;}); if(lead==entries.end())return fail(e,UCRT_CD_PARSE_FAILED,0,"TOC lead-out descriptor is missing");toc->lead_out_lba=lead->lba;
    for(uint8_t t=toc->first_track;t<=toc->last_track && toc->track_count<UCRT_CD_MAX_TRACKS;++t){auto it=std::find_if(entries.begin(),entries.end(),[t](const Entry& x){return x.track==t;});if(it==entries.end())continue;auto& dst=toc->tracks[toc->track_count++];dst.number=t;dst.is_audio=(it->control&0x04)==0;dst.start_lba=it->lba;auto nx=std::find_if(entries.begin(),entries.end(),[t](const Entry& x){return x.track==uint8_t(t+1);});dst.end_lba=(nx!=entries.end()?nx->lba:toc->lead_out_lba)-1;if(t==255)break;}
    return toc->track_count?UCRT_CD_OK:fail(e,UCRT_CD_PARSE_FAILED,0,"No track descriptors were parsed");
}
int32_t UCRT_CD_CALL ucrt_cd_read_audio(ucrt_cd_handle h, int32_t lba,
                                               uint32_t sectors, uint8_t c2,
                                               uint8_t* out, uint32_t cap,
                                               uint32_t* out_len, ucrt_cd_error* e) {
    if (!out_len) {
        return fail(e, UCRT_CD_INVALID_ARGUMENT, 0, "output_length is null");
    }
    *out_len = 0;
    if (!out || sectors == 0 || sectors > 0x00ffffffu) {
        return fail(e, UCRT_CD_INVALID_ARGUMENT, 0,
                    "Invalid output buffer or sector count");
    }

    const uint32_t bytes_per_sector = c2 ? 2646u : 2352u;
    if (sectors > UINT32_MAX / bytes_per_sector) {
        return fail(e, UCRT_CD_INVALID_ARGUMENT, 0,
                    "Requested read is too large");
    }
    const uint32_t required = sectors * bytes_per_sector;
    if (cap < required) {
        return fail(e, UCRT_CD_BUFFER_TOO_SMALL, 0,
                    "Audio output buffer is too small");
    }

    // The tested GH24NSD1 accepts 16 CD-DA sectors per READ CD command,
    // while a larger single command fails. Keep the public request size
    // unrestricted and split it into proven device-level chunks here.
    constexpr uint32_t kMaxSectorsPerCommand = 16u;
    uint32_t completed = 0;

    while (completed < sectors) {
        const uint32_t chunk =
            std::min<uint32_t>(kMaxSectorsPerCommand, sectors - completed);
        const int32_t chunk_lba =
            static_cast<int32_t>(static_cast<uint32_t>(lba) + completed);
        const uint32_t chunk_bytes = chunk * bytes_per_sector;

        uint8_t cdb[12]{};
        cdb[0] = 0xbe;
        cdb[1] = 0x04;  // Expected Sector Type = CD-DA
        cdb[2] = static_cast<uint8_t>(static_cast<uint32_t>(chunk_lba) >> 24);
        cdb[3] = static_cast<uint8_t>(static_cast<uint32_t>(chunk_lba) >> 16);
        cdb[4] = static_cast<uint8_t>(static_cast<uint32_t>(chunk_lba) >> 8);
        cdb[5] = static_cast<uint8_t>(chunk_lba);
        cdb[6] = static_cast<uint8_t>(chunk >> 16);
        cdb[7] = static_cast<uint8_t>(chunk >> 8);
        cdb[8] = static_cast<uint8_t>(chunk);
        cdb[9] = c2 ? 0x12 : 0x10;

        ucrt_cd_scsi_response response{};
        const int32_t result = ucrt_cd_execute_scsi(
            h, cdb, 12, UCRT_CD_DATA_FROM_DEVICE, nullptr, 0,
            out + static_cast<size_t>(completed) * bytes_per_sector,
            chunk_bytes, 30000, &response, e);
        if (result != UCRT_CD_OK) {
            *out_len = completed * bytes_per_sector;
            return result;
        }
        if (response.transferred_length != chunk_bytes) {
            *out_len = completed * bytes_per_sector +
                       std::min<uint32_t>(response.transferred_length,
                                          chunk_bytes);
            return fail(e, UCRT_CD_IO_FAILED, 0,
                        "READ CD returned an incomplete chunk");
        }
        completed += chunk;
        *out_len = completed * bytes_per_sector;
    }
    return UCRT_CD_OK;
}
}
