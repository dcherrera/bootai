/*
 * tools_fs.c - File tool backends
 *
 * Implements file operation tools (read_file, write_file, list_dir)
 * invoked by the model during agentic interaction. Delegates actual
 * I/O to the EFI filesystem layer (efi/fs.c).
 */

#include "tool_use.h"
#include "../efi/efi.h"

/* TODO: Implement file tool backends
 *
 * - ba_tool_read_file(path) -> file contents as ba_tool_result
 * - ba_tool_write_file(path, content) -> success/failure result
 * - ba_tool_list_dir(path) -> directory listing as result
 * Each validates params, calls efi_read_file/efi_write_file/efi_list_dir,
 * and formats output text for the model.
 */
