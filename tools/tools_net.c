/*
 * tools_net.c - Network tool backends
 *
 * Implements network tools (web_search, web_fetch) invoked by the
 * model during agentic interaction. Delegates actual network I/O
 * to the EFI networking layer (efi/net.c).
 */

#include "tool_use.h"
#include "../efi/efi.h"

/* TODO: Implement network tool backends
 *
 * - ba_tool_web_search(query) -> search results as ba_tool_result
 *   Construct HTTP request to search API, parse JSON response
 * - ba_tool_web_fetch(url) -> page content as ba_tool_result
 *   Fetch via efi_http_get(), strip HTML, truncate for context
 * Note: Must handle missing network gracefully.
 */
