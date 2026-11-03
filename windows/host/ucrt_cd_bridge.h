#ifndef UCRT_CD_BRIDGE_H
#define UCRT_CD_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #if defined(UCRT_CD_BRIDGE_DLL)
    #if defined(UCRT_CD_BRIDGE_BUILD)
      #define UCRT_CD_API __declspec(dllexport)
    #else
      #define UCRT_CD_API __declspec(dllimport)
    #endif
  #else
    #define UCRT_CD_API
  #endif
  #define UCRT_CD_CALL __cdecl
#else
  #define UCRT_CD_API
  #define UCRT_CD_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UCRT_CD_ABI_VERSION 1u
#define UCRT_CD_MAX_PATH 1024u
#define UCRT_CD_MAX_NAME 256u
#define UCRT_CD_MAX_ERROR 512u
#define UCRT_CD_MAX_SENSE 64u
#define UCRT_CD_MAX_TRACKS 100u

typedef void* ucrt_cd_handle;

typedef enum ucrt_cd_result {
    UCRT_CD_OK = 0,
    UCRT_CD_INVALID_ARGUMENT = 1,
    UCRT_CD_NOT_FOUND = 2,
    UCRT_CD_ACCESS_DENIED = 3,
    UCRT_CD_OPEN_FAILED = 4,
    UCRT_CD_IO_FAILED = 5,
    UCRT_CD_SCSI_FAILED = 6,
    UCRT_CD_BUFFER_TOO_SMALL = 7,
    UCRT_CD_PARSE_FAILED = 8,
    UCRT_CD_UNSUPPORTED = 9,
    UCRT_CD_INTERNAL_ERROR = 10
} ucrt_cd_result;

typedef enum ucrt_cd_direction {
    UCRT_CD_DATA_NONE = 0,
    UCRT_CD_DATA_FROM_DEVICE = 1,
    UCRT_CD_DATA_TO_DEVICE = 2
} ucrt_cd_direction;

typedef struct ucrt_cd_error {
    int32_t result;
    uint32_t win32_error;
    uint8_t scsi_status;
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
    char message[UCRT_CD_MAX_ERROR];
} ucrt_cd_error;

typedef struct ucrt_cd_device_info {
    wchar_t device_path[UCRT_CD_MAX_PATH];
    wchar_t friendly_name[UCRT_CD_MAX_NAME];
} ucrt_cd_device_info;

typedef struct ucrt_cd_scsi_response {
    uint8_t scsi_status;
    uint8_t sense[UCRT_CD_MAX_SENSE];
    uint32_t sense_length;
    uint32_t transferred_length;
} ucrt_cd_scsi_response;

typedef struct ucrt_cd_inquiry {
    char vendor[9];
    char product[17];
    char revision[5];
} ucrt_cd_inquiry;

typedef struct ucrt_cd_track {
    uint8_t number;
    uint8_t is_audio;
    uint16_t reserved;
    int32_t start_lba;
    int32_t end_lba;
} ucrt_cd_track;

typedef struct ucrt_cd_toc {
    uint8_t first_track;
    uint8_t last_track;
    uint16_t track_count;
    int32_t lead_out_lba;
    ucrt_cd_track tracks[UCRT_CD_MAX_TRACKS];
} ucrt_cd_toc;

UCRT_CD_API uint32_t UCRT_CD_CALL ucrt_cd_get_abi_version(void);
UCRT_CD_API const char* UCRT_CD_CALL ucrt_cd_result_name(int32_t result);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_enumerate(ucrt_cd_device_info* devices, uint32_t capacity, uint32_t* count, ucrt_cd_error* error);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_open(const wchar_t* device_path, ucrt_cd_handle* handle, ucrt_cd_error* error);
UCRT_CD_API void UCRT_CD_CALL ucrt_cd_close(ucrt_cd_handle handle);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_execute_scsi(ucrt_cd_handle handle, const uint8_t* cdb, uint32_t cdb_length, int32_t direction, const uint8_t* output_data, uint32_t output_length, uint8_t* input_data, uint32_t input_capacity, uint32_t timeout_ms, ucrt_cd_scsi_response* response, ucrt_cd_error* error);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_test_unit_ready(ucrt_cd_handle handle, ucrt_cd_error* error);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_inquiry_device(ucrt_cd_handle handle, ucrt_cd_inquiry* inquiry, ucrt_cd_error* error);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_read_toc(ucrt_cd_handle handle, ucrt_cd_toc* toc, ucrt_cd_error* error);
UCRT_CD_API int32_t UCRT_CD_CALL ucrt_cd_read_audio(ucrt_cd_handle handle, int32_t lba, uint32_t sector_count, uint8_t include_c2, uint8_t* output, uint32_t output_capacity, uint32_t* output_length, ucrt_cd_error* error);

#ifdef __cplusplus
}
#endif
#endif
