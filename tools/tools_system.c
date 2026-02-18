/*
 * tools_system.c - System info tool backend
 *
 * Implements the system_info tool that reports UEFI firmware details,
 * memory statistics, and hardware information to the model.
 */

#include "tool_use.h"
#include "../efi/efi.h"

/* TODO: Implement system info tool
 *
 * - ba_tool_system_info() -> ba_tool_result with:
 *   - Firmware vendor and revision (from gST)
 *   - Available memory (via GetMemoryMap)
 *   - Model loaded and architecture
 *   - Boot device path
 * Format as readable text for model consumption.
 */
