/*
 * efi.h — Minimal UEFI type definitions and protocol GUIDs
 *
 * Defines just enough of the UEFI spec to build a boot services application.
 * No dependency on gnu-efi or EDK2.
 */

#ifndef BOOTAI_EFI_H
#define BOOTAI_EFI_H

#include <stdint.h>
#include <stddef.h>

/* Basic UEFI types */
typedef uint64_t UINTN;
typedef int64_t  INTN;
typedef uint64_t EFI_STATUS;
typedef void*    EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint8_t  BOOLEAN;
typedef void*    EFI_EVENT;

/* EFI_GUID */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* Status codes */
#define EFI_SUCCESS              0
#define EFI_ERROR_MASK           0x8000000000000000ULL
#define EFI_ERROR(x)             ((x) | EFI_ERROR_MASK)
#define EFI_NOT_READY            EFI_ERROR(6)
#define EFI_NOT_FOUND            EFI_ERROR(14)
#define EFI_BUFFER_TOO_SMALL     EFI_ERROR(5)

/* Memory types */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

/* Forward declarations */
typedef struct EFI_SYSTEM_TABLE    EFI_SYSTEM_TABLE;
typedef struct EFI_BOOT_SERVICES   EFI_BOOT_SERVICES;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL  EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* Input key */
typedef struct {
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
} EFI_INPUT_KEY;

/* Simple Text Output Protocol */
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* Simple Text Input Protocol */
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* Boot Services (partial — expand as needed) */
struct EFI_BOOT_SERVICES {
    char _hdr[24]; /* EFI_TABLE_HEADER */

    /* Task Priority */
    void *RaiseTPL;
    void *RestoreTPL;

    /* Memory */
    EFI_STATUS (*AllocatePages)(int Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, uint64_t *Memory);
    EFI_STATUS (*FreePages)(uint64_t Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, uint32_t *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);

    /* Events */
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (*WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    /* Protocol Handlers */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image */
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;

    /* Misc */
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;

    /* Driver support */
    void *ConnectController;
    void *DisconnectController;

    /* Open/Close Protocol */
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    /* Library */
    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(int SearchType, EFI_GUID *Protocol, void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
};

/* System Table */
struct EFI_SYSTEM_TABLE {
    char _hdr[24]; /* EFI_TABLE_HEADER */
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
};

/* Protocol GUIDs — defined in efi/main.c */
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiTcp4ProtocolGuid;
extern EFI_GUID gEfiHttpProtocolGuid;
extern EFI_GUID gEfiDhcp4ProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;

/* Global system table — set in efi_main */
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

/* Console */
void efi_print(const CHAR16 *str);
void efi_print_ascii(const char *str);
void efi_print_line(const char *str);
void efi_clear_screen(void);

/* Input */
EFI_STATUS efi_read_key(EFI_INPUT_KEY *key);
void efi_wait_key(void);
int efi_read_line(char *buf, int max_len);

/* Filesystem */
EFI_STATUS efi_fs_init(void);
EFI_STATUS efi_read_file(const char *path, void **buffer, UINTN *size);
EFI_STATUS efi_write_file(const char *path, const void *data, UINTN size);
EFI_STATUS efi_list_dir(const char *path);

/* Networking */
EFI_STATUS efi_net_init(void);
EFI_STATUS efi_http_get(const char *url, void **response, UINTN *size);

/* Memory — JACLibc malloc wired to UEFI AllocatePool */
void efi_memory_init(void);

#endif /* BOOTAI_EFI_H */
