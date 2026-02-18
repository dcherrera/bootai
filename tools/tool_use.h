/*
 * tool_use.h — Tool call parser and dispatcher
 *
 * Detects <tool_call> tags in model output, parses the JSON,
 * dispatches to the appropriate backend, returns the result.
 */

#ifndef BOOTAI_TOOL_USE_H
#define BOOTAI_TOOL_USE_H

#include <stddef.h>

/* Tool types */
typedef enum {
    TOOL_READ_FILE,
    TOOL_WRITE_FILE,
    TOOL_LIST_DIR,
    TOOL_WEB_SEARCH,
    TOOL_WEB_FETCH,
    TOOL_SYSTEM_INFO,
    TOOL_UNKNOWN
} ba_tool_type;

/* Parsed tool call */
typedef struct {
    ba_tool_type type;
    char args[4096];  /* Raw JSON arguments string */
} ba_tool_call;

/* Tool result */
typedef struct {
    char  *output;    /* Result text (caller must free) */
    size_t length;
    int    success;   /* 1 = success, 0 = error */
} ba_tool_result;

/* Check if text contains a <tool_call> tag. Returns offset or -1. */
int ba_tool_detect(const char *text, int len);

/* Parse a <tool_call>...</tool_call> block */
ba_tool_call ba_tool_parse(const char *text, int len);

/* Execute a tool call — dispatches to the appropriate backend */
ba_tool_result ba_tool_execute(const ba_tool_call *call);

/* Free a tool result */
void ba_tool_result_free(ba_tool_result *result);

/* Format tool definitions for system prompt */
const char* ba_tool_definitions(void);

/* Backend implementations (in separate .c files) */
ba_tool_result ba_tool_read_file(const char *path);
ba_tool_result ba_tool_write_file(const char *path, const char *content);
ba_tool_result ba_tool_list_dir(const char *path);
ba_tool_result ba_tool_web_search(const char *query);
ba_tool_result ba_tool_web_fetch(const char *url);
ba_tool_result ba_tool_system_info(void);

#endif /* BOOTAI_TOOL_USE_H */
