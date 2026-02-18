/*
 * tool_use.c - Tool call parser and dispatcher
 *
 * Detects <tool_call> tags in model output, parses JSON arguments,
 * dispatches to the appropriate backend (fs, net, system), and
 * returns formatted results for the conversation context.
 */

#include "tool_use.h"

/* TODO: Implement tool call parsing and dispatch
 *
 * - ba_tool_detect(text, len) -> offset of <tool_call> or -1
 * - ba_tool_parse(text, len) -> ba_tool_call with type + args
 * - ba_tool_execute(call) -> ba_tool_result
 *   Dispatch: read_file/write_file/list_dir -> tools_fs
 *             web_search/web_fetch          -> tools_net
 *             system_info                   -> tools_system
 * - ba_tool_definitions() -> system prompt text
 */
