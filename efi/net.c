/*
 * net.c - Networking via UEFI TCP4 and HTTP protocols
 *
 * Provides network connectivity for tool use (web search, web fetch).
 * Uses EFI_TCP4_PROTOCOL and EFI_HTTP_PROTOCOL when available.
 * Functions: efi_net_init(), efi_http_get()
 */

#include "efi.h"

EFI_STATUS efi_net_init(void) {
    /* TODO: Locate TCP4/HTTP service binding, configure via DHCP4 */
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_http_get(const char *url, void **response, UINTN *size) {
    /* TODO: Create HTTP instance, send GET request, read response body */
    (void)url; (void)response; (void)size;
    return EFI_NOT_FOUND;
}
