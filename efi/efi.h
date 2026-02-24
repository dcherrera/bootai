/*
 * efi.h — Minimal UEFI type definitions and protocol GUIDs
 *
 * Defines just enough of the UEFI spec to build a boot services application.
 * No dependency on gnu-efi or EDK2.
 *
 * All UEFI function pointers use __attribute__((ms_abi)) because UEFI
 * uses the Microsoft x64 calling convention (args in RCX, RDX, R8, R9).
 */

#ifndef BOOTAI_EFI_H
#define BOOTAI_EFI_H

#include <stdint.h>
#include <stddef.h>

/* UEFI calling convention — Microsoft x64 ABI */
#define EFIAPI __attribute__((ms_abi))

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

/* EFI_DEVICE_PATH_PROTOCOL */
typedef struct {
    uint8_t Type;
    uint8_t SubType;
    uint8_t Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

/* Status codes */
#define EFI_SUCCESS              0
#define EFI_ERROR_MASK           0x8000000000000000ULL
#define EFI_ERROR(x)             ((x) | EFI_ERROR_MASK)
#define EFI_UNSUPPORTED          EFI_ERROR(3)
#define EFI_BUFFER_TOO_SMALL     EFI_ERROR(5)
#define EFI_NOT_READY            EFI_ERROR(6)
#define EFI_DEVICE_ERROR         EFI_ERROR(7)
#define EFI_NOT_FOUND            EFI_ERROR(14)
#define EFI_NO_MAPPING           EFI_ERROR(17)
#define EFI_TIMEOUT              EFI_ERROR(18)
#define EFI_CONNECTION_FIN       EFI_ERROR(104)
#define EFI_CONNECTION_RESET     EFI_ERROR(105)
#define EFI_CONNECTION_REFUSED   EFI_ERROR(106)

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

/* IPv4 address */
typedef struct { uint8_t Addr[4]; } EFI_IPv4_ADDRESS;

/* Service Binding Protocol (used by TCP4, UDP4, DNS4, HTTP) */
typedef struct EFI_SERVICE_BINDING_PROTOCOL {
    EFI_STATUS (EFIAPI *CreateChild)(struct EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *ChildHandle);
    EFI_STATUS (EFIAPI *DestroyChild)(struct EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE ChildHandle);
} EFI_SERVICE_BINDING_PROTOCOL;

/* ---- TCP4 Protocol types ---- */

typedef struct {
    BOOLEAN          UseDefaultAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    uint16_t         StationPort;
    EFI_IPv4_ADDRESS RemoteAddress;
    uint16_t         RemotePort;
    BOOLEAN          ActiveFlag;
} EFI_TCP4_ACCESS_POINT;

typedef struct {
    uint8_t               TypeOfService;
    uint8_t               TimeToLive;
    EFI_TCP4_ACCESS_POINT AccessPoint;
    void                 *ControlOption;
} EFI_TCP4_CONFIG_DATA;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
} EFI_TCP4_COMPLETION_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
} EFI_TCP4_CONNECTION_TOKEN;

typedef struct {
    uint32_t FragmentLength;
    void    *FragmentBuffer;
} EFI_TCP4_FRAGMENT_DATA;

typedef struct {
    BOOLEAN              Push;
    BOOLEAN              Urgent;
    uint32_t             DataLength;
    uint32_t             FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_TRANSMIT_DATA;

typedef struct {
    BOOLEAN              UrgentFlag;
    uint32_t             DataLength;
    uint32_t             FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_RECEIVE_DATA;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    union {
        EFI_TCP4_RECEIVE_DATA  *RxData;
        EFI_TCP4_TRANSMIT_DATA *TxData;
    } Packet;
} EFI_TCP4_IO_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
} EFI_TCP4_CLOSE_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    EFI_HANDLE                NewChildHandle;
} EFI_TCP4_LISTEN_TOKEN;

typedef struct EFI_TCP4_PROTOCOL {
    void *GetModeData;
    EFI_STATUS (EFIAPI *Configure)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONFIG_DATA *TcpConfigData);
    void *Routes;
    EFI_STATUS (EFIAPI *Connect)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONNECTION_TOKEN *ConnectionToken);
    EFI_STATUS (EFIAPI *Accept)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_LISTEN_TOKEN *ListenToken);
    EFI_STATUS (EFIAPI *Transmit)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *Token);
    EFI_STATUS (EFIAPI *Receive)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *Token);
    EFI_STATUS (EFIAPI *Close)(struct EFI_TCP4_PROTOCOL *This, EFI_TCP4_CLOSE_TOKEN *CloseToken);
    void *Cancel;
    EFI_STATUS (EFIAPI *Poll)(struct EFI_TCP4_PROTOCOL *This);
} EFI_TCP4_PROTOCOL;

/* ---- UDP4 Protocol types ---- */

typedef struct {
    BOOLEAN          AcceptBroadcast;
    BOOLEAN          AcceptPromiscuous;
    BOOLEAN          AcceptAnyPort;
    BOOLEAN          AllowDuplicatePort;
    uint8_t          TypeOfService;
    uint8_t          TimeToLive;
    BOOLEAN          DoNotFragment;
    uint32_t         ReceiveTimeout;
    uint32_t         TransmitTimeout;
    BOOLEAN          UseDefaultAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    uint16_t         StationPort;
    EFI_IPv4_ADDRESS RemoteAddress;
    uint16_t         RemotePort;
} EFI_UDP4_CONFIG_DATA;

typedef struct {
    EFI_IPv4_ADDRESS SourceAddress;
    uint16_t         SourcePort;
    EFI_IPv4_ADDRESS DestinationAddress;
    uint16_t         DestinationPort;
} EFI_UDP4_SESSION_DATA;

typedef struct {
    uint32_t FragmentLength;
    void    *FragmentBuffer;
} EFI_UDP4_FRAGMENT_DATA;

typedef struct {
    EFI_UDP4_SESSION_DATA *UdpSessionData;
    EFI_IPv4_ADDRESS      *GatewayAddress;
    uint32_t               DataLength;
    uint32_t               FragmentCount;
    EFI_UDP4_FRAGMENT_DATA FragmentTable[1];
} EFI_UDP4_TRANSMIT_DATA;

typedef struct {
    uint64_t               TimeStamp;     /* EFI_TIME pad — 8 bytes */
    uint64_t               TimeStamp2;    /* Remaining 8 bytes of timestamp */
    EFI_EVENT              RecycleSignal;
    EFI_UDP4_SESSION_DATA  SessionData;
    uint32_t               DataLength;
    uint32_t               FragmentCount;
    EFI_UDP4_FRAGMENT_DATA FragmentTable[1];
} EFI_UDP4_RECEIVE_DATA;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
    union {
        EFI_UDP4_RECEIVE_DATA  *RxData;
        EFI_UDP4_TRANSMIT_DATA *TxData;
    } Packet;
} EFI_UDP4_COMPLETION_TOKEN;

typedef struct EFI_UDP4_PROTOCOL {
    void *GetModeData;
    EFI_STATUS (EFIAPI *Configure)(struct EFI_UDP4_PROTOCOL *This, EFI_UDP4_CONFIG_DATA *UdpConfigData);
    EFI_STATUS (EFIAPI *Groups)(struct EFI_UDP4_PROTOCOL *This, BOOLEAN JoinFlag, EFI_IPv4_ADDRESS *MulticastAddress);
    void *Routes;
    EFI_STATUS (EFIAPI *Transmit)(struct EFI_UDP4_PROTOCOL *This, EFI_UDP4_COMPLETION_TOKEN *Token);
    EFI_STATUS (EFIAPI *Receive)(struct EFI_UDP4_PROTOCOL *This, EFI_UDP4_COMPLETION_TOKEN *Token);
    void *Cancel;
    EFI_STATUS (EFIAPI *Poll)(struct EFI_UDP4_PROTOCOL *This);
} EFI_UDP4_PROTOCOL;

/* ---- IP4 Config2 Protocol (for DHCP) ---- */

typedef struct EFI_IP4_CONFIG2_PROTOCOL {
    EFI_STATUS (EFIAPI *SetData)(struct EFI_IP4_CONFIG2_PROTOCOL *This, uint32_t DataType, UINTN DataSize, void *Data);
    EFI_STATUS (EFIAPI *GetData)(struct EFI_IP4_CONFIG2_PROTOCOL *This, uint32_t DataType, UINTN *DataSize, void *Data);
    void *RegisterDataNotify;
    void *UnregisterDataNotify;
} EFI_IP4_CONFIG2_PROTOCOL;

/* IP4Config2 data types */
#define IP4_CONFIG2_DATA_TYPE_INTERFACEINFO 0
#define IP4_CONFIG2_DATA_TYPE_POLICY        1
#define IP4_CONFIG2_DATA_TYPE_DNSSERVER     4

/* IP4Config2 policy */
#define IP4_CONFIG2_POLICY_STATIC 0
#define IP4_CONFIG2_POLICY_DHCP   1

/* ---- Simple File System Protocol ---- */

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

/* File open modes */
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

/* File attributes */
#define EFI_FILE_READ_ONLY  0x0000000000000001ULL
#define EFI_FILE_DIRECTORY  0x0000000000000010ULL

/* EFI_FILE_INFO GUID */
#define EFI_FILE_INFO_GUID { 0x09576e92, 0x6d3f, 0x11d2, \
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} }

typedef struct {
    uint64_t Size;          /* Size of this struct + FileName */
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint8_t  CreateTime[16];  /* EFI_TIME (16 bytes) */
    uint8_t  LastAccessTime[16];
    uint8_t  ModificationTime[16];
    uint64_t Attribute;
    CHAR16   FileName[1];   /* Null-terminated, variable length */
} EFI_FILE_INFO;

struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *GetPosition;
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 UINTN *BufferSize, void *Buffer);
    void *SetInfo;
    void *Flush;
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                     EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- DNS4 Protocol (minimal) ---- */

typedef struct {
    EFI_STATUS       Status;
    uint32_t         IpCount;
    EFI_IPv4_ADDRESS *IpList;
} EFI_DNS4_HOST_NAME_TO_IP;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
    uint32_t   RetryCount;
    uint32_t   RetryInterval;
    EFI_DNS4_HOST_NAME_TO_IP *H2AData;
} EFI_DNS4_COMPLETION_TOKEN;

typedef struct EFI_DNS4_PROTOCOL {
    void *GetModeData;
    EFI_STATUS (EFIAPI *Configure)(struct EFI_DNS4_PROTOCOL *This, void *DnsConfigData);
    EFI_STATUS (EFIAPI *HostNameToIp)(struct EFI_DNS4_PROTOCOL *This, CHAR16 *HostName, EFI_DNS4_COMPLETION_TOKEN *Token);
    void *IpToHostName;
    void *GeneralLookUp;
    void *UpdateDnsCache;
    EFI_STATUS (EFIAPI *Poll)(struct EFI_DNS4_PROTOCOL *This);
    void *Cancel;
} EFI_DNS4_PROTOCOL;

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
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* Simple Text Input Protocol */
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* Boot Services (partial — expand as needed) */
struct EFI_BOOT_SERVICES {
    char _hdr[24]; /* EFI_TABLE_HEADER */

    /* Task Priority */
    void *RaiseTPL;
    void *RestoreTPL;

    /* Memory */
    EFI_STATUS (EFIAPI *AllocatePages)(int Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, uint64_t *Memory);
    EFI_STATUS (EFIAPI *FreePages)(uint64_t Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, void *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, uint32_t *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

    /* Events */
    EFI_STATUS (EFIAPI *CreateEvent)(uint32_t Type, UINTN NotifyTpl, void (EFIAPI *NotifyFunction)(EFI_EVENT, void*), void *NotifyContext, EFI_EVENT *Event);
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    void *CheckEvent;

    /* Protocol Handlers */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    /* Image */
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
                                   EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                   void *SourceBuffer, UINTN SourceSize,
                                   EFI_HANDLE *ImageHandle);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize,
                                    CHAR16 **ExitData);
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;

    /* Misc */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    void *SetWatchdogTimer;

    /* Driver support */
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle, void *RemainingDevicePath, BOOLEAN Recursive);
    void *DisconnectController;

    /* Open/Close Protocol */
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    /* Library */
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(int SearchType, EFI_GUID *Protocol, void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
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

/* ---- 802.11 / WiFi types (UEFI 2.7+) ---- */

typedef struct {
    uint8_t Addr[6];
} EFI_80211_MAC_ADDRESS;

#define EFI_MAX_SSID_LEN 32

typedef struct {
    uint8_t SSIdLen;
    uint8_t SSId[EFI_MAX_SSID_LEN];
} EFI_80211_SSID;

typedef enum {
    IeeeInfrastructure = 0,
    IeeeIBSS           = 1,
    IeeeAny             = 2,
} EFI_80211_BSS_TYPE;

typedef enum {
    Ieee80211OpenSystem      = 0,
    Ieee80211SharedKey       = 1,
    Ieee80211WPAPSK          = 2,
    Ieee80211WPA2PSK         = 3,
} EFI_80211_AKM_SUITE_TYPE;

typedef struct {
    EFI_80211_SSID         SSId;
    EFI_80211_BSS_TYPE     BSSType;
    EFI_80211_MAC_ADDRESS  BSSId;
    int16_t                RSSI;       /* Signal strength in dBm */
    EFI_80211_AKM_SUITE_TYPE AKMSuite;
} EFI_80211_NETWORK;

typedef struct {
    uint32_t             NetworkCount;
    EFI_80211_NETWORK    Network[1];   /* Variable-length array */
} EFI_80211_NETWORK_DESCRIPTION;

/* WiFi2 completion token */
typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
} EFI_80211_COMPLETION_TOKEN;

typedef struct {
    EFI_80211_COMPLETION_TOKEN Token;
    EFI_80211_NETWORK_DESCRIPTION *Result;
} EFI_80211_GET_NETWORKS_TOKEN;

typedef struct {
    EFI_80211_COMPLETION_TOKEN Token;
    EFI_80211_NETWORK_DESCRIPTION *Data;  /* Network to connect to */
} EFI_80211_CONNECT_NETWORK_TOKEN;

typedef struct {
    EFI_80211_COMPLETION_TOKEN Token;
} EFI_80211_DISCONNECT_NETWORK_TOKEN;

/* EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL (WiFi2) */
typedef struct EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL {
    EFI_STATUS (EFIAPI *GetNetworks)(
        struct EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL *This,
        EFI_80211_GET_NETWORKS_TOKEN *Token);
    EFI_STATUS (EFIAPI *ConnectNetwork)(
        struct EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL *This,
        EFI_80211_CONNECT_NETWORK_TOKEN *Token);
    EFI_STATUS (EFIAPI *DisconnectNetwork)(
        struct EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL *This,
        EFI_80211_DISCONNECT_NETWORK_TOKEN *Token);
} EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL;

/* Supplicant data types */
#define EFI_SUPPLICANT_DATA_TYPE_SSID  0
#define EFI_SUPPLICANT_DATA_TYPE_PSK   1

/* EFI_SUPPLICANT_PROTOCOL */
typedef struct EFI_SUPPLICANT_PROTOCOL {
    void *BuildResponsePacket;
    void *ProcessPacket;
    EFI_STATUS (EFIAPI *SetData)(
        struct EFI_SUPPLICANT_PROTOCOL *This,
        uint32_t DataType,
        void *Data,
        UINTN DataSize);
    EFI_STATUS (EFIAPI *GetData)(
        struct EFI_SUPPLICANT_PROTOCOL *This,
        uint32_t DataType,
        void **Data,
        UINTN *DataSize);
} EFI_SUPPLICANT_PROTOCOL;

/* Protocol GUIDs */
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiTcp4ServiceBindingGuid;
extern EFI_GUID gEfiTcp4ProtocolGuid;
extern EFI_GUID gEfiIp4Config2ProtocolGuid;
extern EFI_GUID gEfiUdp4ServiceBindingGuid;
extern EFI_GUID gEfiUdp4ProtocolGuid;
extern EFI_GUID gEfiDns4ServiceBindingGuid;
extern EFI_GUID gEfiDns4ProtocolGuid;
extern EFI_GUID gEfiWiFi2ProtocolGuid;
extern EFI_GUID gEfiSupplicantProtocolGuid;

/* Global system table + image handle — set in efi_main */
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImageHandle;

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
EFI_STATUS efi_file_size(const char *path, UINTN *size_out);
EFI_STATUS efi_read_file(const char *path, void **buffer, UINTN *size);
EFI_STATUS efi_write_file(const char *path, const void *data, UINTN size);
EFI_STATUS efi_list_dir(const char *path);

/* Networking */
EFI_STATUS efi_net_init(void);
int efi_net_available(void);
EFI_STATUS efi_http_get(const char *url, char *response, UINTN max_size, UINTN *out_size);
EFI_STATUS efi_dns_resolve(const char *hostname, EFI_IPv4_ADDRESS *out_ip);

/* TCP4 connection handle (used by net.c, http_server.c, api.c) */
typedef struct {
    EFI_TCP4_PROTOCOL *tcp4;
    EFI_HANDLE child_handle;
} tcp4_conn_t;

/* TCP4 server primitives */
EFI_STATUS tcp4_listen(uint16_t port, tcp4_conn_t *listener);
EFI_STATUS tcp4_accept(tcp4_conn_t *listener, tcp4_conn_t *client);
EFI_STATUS tcp4_send(tcp4_conn_t *conn, const void *data, UINTN len);
INTN       tcp4_recv(tcp4_conn_t *conn, void *buf, UINTN max_len);
void       tcp4_close(tcp4_conn_t *conn);

/* HTTP server */
void ba_http_serve(uint16_t port);

/* Memory — JACLibc malloc wired to UEFI AllocatePool */
void efi_memory_init(void);

/* Driver loading */
void efi_load_drivers(void);

/* WiFi */
int  efi_wifi_available(void);
void efi_wifi_scan(void);
EFI_STATUS efi_wifi_connect(const char *ssid, const char *password);
void efi_wifi_disconnect(void);
void efi_wifi_auto_connect(void);
void efi_wifi_save(const char *ssid, const char *password);
void efi_wifi_status(void);

/* mDNS / Bonjour discovery */
EFI_STATUS ba_mdns_init(void);
void       ba_mdns_announce(void);
void       ba_mdns_poll(void);
void       ba_mdns_stop(void);

/* Local IP retrieval */
EFI_STATUS efi_get_local_ip(EFI_IPv4_ADDRESS *out_ip);

/* Network helpers (used by drivers.c and wifi.c) */
void connect_all_controllers(void);

#endif /* BOOTAI_EFI_H */
