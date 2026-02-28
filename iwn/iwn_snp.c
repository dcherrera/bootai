/*
 * iwn_snp.c -- EFI_SIMPLE_NETWORK_PROTOCOL wrapper for Intel Wireless N
 *
 * Phase 5: Wraps the IWN driver as SNP so UEFI's built-in IP4/TCP4/UDP4
 * stack binds automatically. After iwn_connect() succeeds, call
 * iwn_snp_install() then efi_net_init() to get DHCP + full networking.
 *
 * Transmit() calls iwn_encap_tx() (802.11 encap + CCMP).
 * Receive() calls iwn_rx_dequeue() (802.11 decap from ring buffer).
 * GetStatus() calls iwn_rx_poll() to drain HW frames.
 */

#include "iwn_internal.h"

/* ---- Globals ---- */

static EFI_SIMPLE_NETWORK_MODE snp_mode;
static EFI_SIMPLE_NETWORK_PROTOCOL snp_protocol;
static EFI_HANDLE snp_handle;
static int snp_installed;
static EFI_EVENT snp_wait_event;

/* TX buffer recycling ring for GetStatus() */
#define TX_RECYCLE_COUNT 8
static void *tx_recycle[TX_RECYCLE_COUNT];
static int tx_recycle_head;
static int tx_recycle_tail;

/* ---- WaitForPacket notify (EVT_NOTIFY_WAIT) ---- */

static void EFIAPI snp_wait_check(EFI_EVENT event, void *ctx) {
    (void)event;
    (void)ctx;
    /* Check if RX frames are pending — called by firmware when
     * WaitForEvent/CheckEvent is used on WaitForPacket */
    iwn_rx_poll(&g_iwn);
}

/* ---- SNP Function Implementations ---- */

static EFI_STATUS EFIAPI iwn_snp_start(EFI_SIMPLE_NETWORK_PROTOCOL *This) {
    (void)This;
    if (snp_mode.State != EFI_SIMPLE_NETWORK_STOPPED)
        return EFI_DEVICE_ERROR;
    snp_mode.State = EFI_SIMPLE_NETWORK_STARTED;
    efi_print_ascii("  SNP: Start()\r\n");
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_stop(EFI_SIMPLE_NETWORK_PROTOCOL *This) {
    (void)This;
    if (snp_mode.State == EFI_SIMPLE_NETWORK_STOPPED)
        return EFI_DEVICE_ERROR;
    snp_mode.State = EFI_SIMPLE_NETWORK_STOPPED;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_initialize(EFI_SIMPLE_NETWORK_PROTOCOL *This,
                                             UINTN ExtraRxBufSize,
                                             UINTN ExtraTxBufSize) {
    (void)This;
    (void)ExtraRxBufSize;
    (void)ExtraTxBufSize;
    if (snp_mode.State != EFI_SIMPLE_NETWORK_STARTED)
        return EFI_DEVICE_ERROR;
    snp_mode.State = EFI_SIMPLE_NETWORK_INITIALIZED;
    efi_print_ascii("  SNP: Initialize()\r\n");
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_reset(EFI_SIMPLE_NETWORK_PROTOCOL *This,
                                        BOOLEAN ExtendedVerification) {
    (void)This;
    (void)ExtendedVerification;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_shutdown(EFI_SIMPLE_NETWORK_PROTOCOL *This) {
    (void)This;
    if (snp_mode.State != EFI_SIMPLE_NETWORK_INITIALIZED)
        return EFI_DEVICE_ERROR;
    snp_mode.State = EFI_SIMPLE_NETWORK_STOPPED;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_receive_filters(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    uint32_t Enable, uint32_t Disable,
    BOOLEAN ResetMCastFilter, UINTN MCastFilterCnt,
    EFI_MAC_ADDRESS *MCastFilter) {
    (void)This;
    (void)ResetMCastFilter;
    (void)MCastFilterCnt;
    (void)MCastFilter;
    snp_mode.ReceiveFilterSetting |= Enable;
    snp_mode.ReceiveFilterSetting &= ~Disable;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_station_address(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    BOOLEAN Reset, EFI_MAC_ADDRESS *New) {
    (void)This;
    if (Reset) {
        iwn_memcpy(snp_mode.CurrentAddress.Addr,
                   snp_mode.PermanentAddress.Addr, 6);
    } else if (New) {
        iwn_memcpy(snp_mode.CurrentAddress.Addr, New->Addr, 6);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_statistics(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    BOOLEAN Reset, UINTN *StatisticsSize, void *StatisticsTable) {
    (void)This;
    (void)Reset;
    (void)StatisticsSize;
    (void)StatisticsTable;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI iwn_snp_mcast_ip_to_mac(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    BOOLEAN IPv6, EFI_IP_ADDRESS *IP, EFI_MAC_ADDRESS *MAC) {
    (void)This;
    if (IPv6)
        return EFI_UNSUPPORTED;
    if (!IP || !MAC)
        return EFI_DEVICE_ERROR;

    /* IPv4 multicast: 01:00:5E:(IP[1] & 0x7F):IP[2]:IP[3] */
    iwn_memset(MAC->Addr, 0, 32);
    MAC->Addr[0] = 0x01;
    MAC->Addr[1] = 0x00;
    MAC->Addr[2] = 0x5E;
    MAC->Addr[3] = ((uint8_t *)&IP->v4)[1] & 0x7F;
    MAC->Addr[4] = ((uint8_t *)&IP->v4)[2];
    MAC->Addr[5] = ((uint8_t *)&IP->v4)[3];
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_nvdata(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    BOOLEAN ReadWrite, UINTN Offset, UINTN BufferSize, void *Buffer) {
    (void)This;
    (void)ReadWrite;
    (void)Offset;
    (void)BufferSize;
    (void)Buffer;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI iwn_snp_get_status(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    uint32_t *InterruptStatus, void **TxBuf) {
    (void)This;

    /* Drain any pending HW frames */
    iwn_rx_poll(&g_iwn);

    if (InterruptStatus) {
        *InterruptStatus = 0;
        if (iwn_rx_pending(&g_iwn) > 0)
            *InterruptStatus |= EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
    }

    /* Return a recycled TX buffer pointer */
    if (TxBuf) {
        if (tx_recycle_head != tx_recycle_tail) {
            *TxBuf = tx_recycle[tx_recycle_head];
            tx_recycle_head = (tx_recycle_head + 1) % TX_RECYCLE_COUNT;
        } else {
            *TxBuf = (void *)0;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_transmit(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    UINTN HeaderSize, UINTN BufferSize, void *Buffer,
    EFI_MAC_ADDRESS *SrcAddr, EFI_MAC_ADDRESS *DestAddr,
    uint16_t *Protocol) {
    (void)This;
    (void)SrcAddr;

    if (snp_mode.State != EFI_SIMPLE_NETWORK_INITIALIZED)
        return EFI_NOT_READY;
    if (!Buffer)
        return EFI_DEVICE_ERROR;

    uint8_t *pkt = (uint8_t *)Buffer;
    const uint8_t *dst;
    uint16_t ethertype;
    const uint8_t *payload;
    UINTN payload_len;

    if (HeaderSize != 0) {
        /* Caller provides header components separately —
         * Buffer contains only payload (no Ethernet header) */
        if (!DestAddr || !Protocol)
            return EFI_DEVICE_ERROR;
        dst = DestAddr->Addr;
        ethertype = *Protocol;
        payload = pkt;
        payload_len = BufferSize;
    } else {
        /* Buffer contains complete Ethernet frame (14-byte header + payload) */
        if (BufferSize < 14)
            return EFI_DEVICE_ERROR;
        dst = pkt;              /* first 6 bytes = DST MAC */
        ethertype = ((uint16_t)pkt[12] << 8) | pkt[13];
        payload = pkt + 14;
        payload_len = BufferSize - 14;
    }

    int ret = iwn_encap_tx(&g_iwn, dst, ethertype, payload, (uint16_t)payload_len);
    if (ret != 0)
        return EFI_DEVICE_ERROR;

    /* Record buffer pointer for GetStatus() to recycle */
    int next = (tx_recycle_tail + 1) % TX_RECYCLE_COUNT;
    if (next != tx_recycle_head) {
        tx_recycle[tx_recycle_tail] = Buffer;
        tx_recycle_tail = next;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI iwn_snp_receive(
    EFI_SIMPLE_NETWORK_PROTOCOL *This,
    UINTN *HeaderSize, UINTN *BufferSize, void *Buffer,
    EFI_MAC_ADDRESS *SrcAddr, EFI_MAC_ADDRESS *DestAddr,
    uint16_t *Protocol) {
    (void)This;

    if (snp_mode.State != EFI_SIMPLE_NETWORK_INITIALIZED)
        return EFI_NOT_READY;
    if (!BufferSize || !Buffer)
        return EFI_DEVICE_ERROR;

    /* Poll HW first */
    iwn_rx_poll(&g_iwn);

    uint16_t frame_len = 0;
    int ret = iwn_rx_dequeue(&g_iwn, (uint8_t *)Buffer, &frame_len);
    if (ret != 0)
        return EFI_NOT_READY;

    *BufferSize = frame_len;

    /* Parse Ethernet header for caller */
    uint8_t *pkt = (uint8_t *)Buffer;
    if (HeaderSize) *HeaderSize = 14;
    if (DestAddr)   iwn_memcpy(DestAddr->Addr, pkt, 6);
    if (SrcAddr)    iwn_memcpy(SrcAddr->Addr, pkt + 6, 6);
    if (Protocol)   *Protocol = ((uint16_t)pkt[12] << 8) | pkt[13];

    return EFI_SUCCESS;
}

/* ---- Install SNP on UEFI handle ---- */

int iwn_snp_install(void) {
    if (snp_installed) return 0;

    iwn_dev *dev = &g_iwn;

    /* Initialize Mode struct.
     * State starts as STOPPED — DXE drivers (MnpDxe etc.) manage the
     * lifecycle: Start() → Initialize() → ready for Transmit/Receive. */
    iwn_memset(&snp_mode, 0, sizeof(snp_mode));
    snp_mode.State = EFI_SIMPLE_NETWORK_STOPPED;
    snp_mode.HwAddressSize = 6;
    snp_mode.MediaHeaderSize = 14;
    snp_mode.MaxPacketSize = 1500;
    snp_mode.ReceiveFilterMask = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST
                                | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
    snp_mode.ReceiveFilterSetting = snp_mode.ReceiveFilterMask;
    snp_mode.MaxMCastFilterCount = 16;
    snp_mode.IfType = 6;  /* IANA ifType for Ethernet */
    snp_mode.MediaPresent = 1;
    snp_mode.MediaPresentSupported = 1;

    /* Set MAC addresses */
    iwn_memcpy(snp_mode.CurrentAddress.Addr, dev->mac_addr, 6);
    iwn_memcpy(snp_mode.PermanentAddress.Addr, dev->mac_addr, 6);
    iwn_memset(snp_mode.BroadcastAddress.Addr, 0xff, 6);

    /* Build protocol function table */
    iwn_memset(&snp_protocol, 0, sizeof(snp_protocol));
    snp_protocol.Revision = 0x00010000;  /* SNP revision 1.0 */
    snp_protocol.Start = iwn_snp_start;
    snp_protocol.Stop = iwn_snp_stop;
    snp_protocol.Initialize = iwn_snp_initialize;
    snp_protocol.Reset = iwn_snp_reset;
    snp_protocol.Shutdown = iwn_snp_shutdown;
    snp_protocol.ReceiveFilters = iwn_snp_receive_filters;
    snp_protocol.StationAddress = iwn_snp_station_address;
    snp_protocol.Statistics = iwn_snp_statistics;
    snp_protocol.MCastIpToMac = iwn_snp_mcast_ip_to_mac;
    snp_protocol.NvData = iwn_snp_nvdata;
    snp_protocol.GetStatus = iwn_snp_get_status;
    snp_protocol.Transmit = iwn_snp_transmit;
    snp_protocol.Receive = iwn_snp_receive;
    /* Create WaitForPacket event — MnpDxe requires this to be non-NULL.
     * EVT_NOTIFY_WAIT (0x100) fires when firmware checks the event. */
    snp_wait_event = (void *)0;
    gBS->CreateEvent(0x00000100, /* EVT_NOTIFY_WAIT */
                     8,          /* TPL_CALLBACK */
                     snp_wait_check, (void *)0, &snp_wait_event);
    snp_protocol.WaitForPacket = snp_wait_event;
    snp_protocol.Mode = &snp_mode;

    /* Reset TX recycle ring */
    tx_recycle_head = 0;
    tx_recycle_tail = 0;

    /* Install SNP on a new handle */
    snp_handle = (void *)0;
    EFI_STATUS status = gBS->InstallProtocolInterface(
        &snp_handle,
        &gEfiSimpleNetworkProtocolGuid,
        0,  /* EFI_NATIVE_INTERFACE */
        &snp_protocol);

    if (status != EFI_SUCCESS) {
        efi_print_ascii("  SNP install failed\r\n");
        return -1;
    }

    /* Install device path on same handle (needed for driver binding).
     * MAC device path: Type=3 (Messaging), SubType=37 (MAC), length=37.
     * Followed by End Entire Device Path node. */
    static uint8_t mac_devpath[41];
    iwn_memset(mac_devpath, 0, sizeof(mac_devpath));

    /* MAC Address Device Path node */
    mac_devpath[0] = 3;    /* Type: Messaging Device Path */
    mac_devpath[1] = 37;   /* SubType: MAC Address Device Path */
    mac_devpath[2] = 37;   /* Length low byte (4 + 32 + 1 = 37) */
    mac_devpath[3] = 0;    /* Length high byte */
    iwn_memcpy(mac_devpath + 4, dev->mac_addr, 6);
    /* bytes 10-35: zero (pad MAC to 32 bytes) — already zeroed */
    mac_devpath[36] = 6;   /* IfType = Ethernet (IANA) */

    /* End Entire Device Path node */
    mac_devpath[37] = 0x7F; /* Type: End */
    mac_devpath[38] = 0xFF; /* SubType: End Entire Device Path */
    mac_devpath[39] = 4;    /* Length = 4 */
    mac_devpath[40] = 0;

    gBS->InstallProtocolInterface(
        &snp_handle,
        &gEfiDevicePathProtocolGuid,
        0,
        mac_devpath);

    /* NOTE: We do NOT call ConnectController here.
     * OVMF DXE drivers (MnpDxe etc.) hang on Dell E6510 firmware during
     * binding — their internal initialization is incompatible.
     * Instead, efi_net_init() uses the custom network stack (ns_*)
     * which works directly on SNP without DXE drivers. */

    efi_print_ascii("  SNP installed\r\n");
    snp_installed = 1;
    return 0;
}

int iwn_snp_is_installed(void) { return snp_installed; }

EFI_SIMPLE_NETWORK_PROTOCOL *iwn_snp_get_protocol(void) {
    return snp_installed ? &snp_protocol : (void *)0;
}

/* ---- Direct data path for custom network stack ----
 * Bypasses the SNP protocol function pointer table, which hangs on
 * Dell E6510 firmware (likely a PIC/relocation issue with EFIAPI
 * indirect calls). The custom ns_* stack calls these instead. */

void iwn_snp_get_mac(uint8_t *mac) {
    iwn_memcpy(mac, g_iwn.mac_addr, 6);
}

int iwn_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                      const uint8_t *payload, int len) {
    return iwn_encap_tx(&g_iwn, dst_mac, ethertype,
                        payload, (uint16_t)len);
}

int iwn_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len) {
    (void)buf_size;
    iwn_rx_poll(&g_iwn);
    uint16_t flen = 0;
    int ret = iwn_rx_dequeue(&g_iwn, buf, &flen);
    if (ret == 0 && frame_len) *frame_len = (int)flen;
    return ret;
}

/* ---- Debug: dump HW state for diagnostics ---- */

void iwn_snp_debug_state(void) {
    iwn_dev *dev = &g_iwn;
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;
    uint32_t hw_idx = stat->closed_count & 0x0fff;
    int sw_pending = (dev->rx_tail - dev->rx_head + IWN_RX_BUF_COUNT)
                     % IWN_RX_BUF_COUNT;

    efi_print_ascii("  IWN: tx_ok=");
    print_dec(dev->tx_ok);
    efi_print_ascii(" hw=");
    print_dec(hw_idx);
    efi_print_ascii(" cur=");
    print_dec((uint32_t)dev->rxq.cur);
    efi_print_ascii(" sw=");
    print_dec((uint32_t)sw_pending);
    efi_print_ascii("\r\n");
}
