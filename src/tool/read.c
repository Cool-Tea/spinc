#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "log.h"
#include "tool/tool.h"
#include "tool/utils.h"

#define READ_TOOL_MAX_SIZE (64 * 1024)  // 64KB

/**
 * Read a file and return its contents with line numbers.
 * @param path The path to the file to read.
 * @param offset The line number offset to start reading from (optional).
 * @param limit The number of lines to read (optional).
 * @return A JSON string with the file contents and line numbers.
 */
err_t read_tool(const char* args, size_t args_len, char** result,
                size_t* result_len) {
  log(INFO, "Calling Read tool");
  log(DEBUG, "Read tool args: %.*s", (int)args_len, args);

  err_t err = ERROR_NONE;
  jnode_t* args_node = NULL;
  jnode_t* res = NULL;
  FILE* f = NULL;
  char* line = NULL;

  if (!args || !result || !result_len) return ERROR_NULLPTR;
  *result = NULL;
  *result_len = 0;

  res = jobject_new();
  if (!res) return ERROR_OUT_OF_MEMORY;
  args_node = jfrom_string(args, (int)args_len);
  TOOL_CHECK(read, args_node, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args_node, "path");
  TOOL_CHECK(read, path_node, "Missing required parameter: path");
  TOOL_CHECK(read, jis_string(path_node), "Parameter 'path' must be a string");

  jnode_t* offset_node = jobject_get(args_node, "offset");
  if (offset_node) {
    TOOL_CHECK(read, jis_number(offset_node),
               "Parameter 'offset' must be an integer");
  }

  jnode_t* limit_node = jobject_get(args_node, "limit");
  if (limit_node) {
    TOOL_CHECK(read, jis_number(limit_node),
               "Parameter 'limit' must be an integer");
  }

  const char* path = jstring_content(path_node);
  f = fopen(path, "rb");
  TOOL_CHECK(read, f, "Failed to open file: %s", path);

  jnode_t* contents = jstring_new(0, "");
  TOOL_CHECK(read, contents, "Failed to allocate memory for file contents");
  jobject_put(res, "contents", contents);

  size_t offset = 1;
  if (offset_node) {
    offset = (size_t)jas_number(offset_node)->value;
  }

  size_t end_lineno = (size_t)-1;
  if (limit_node) {
    end_lineno = offset + (size_t)jas_number(limit_node)->value;
  }

  int line_len = 0;
  size_t lineno = 1, linecap = 0;
  // skip lines until offset
  while (lineno < offset && (line_len = getline(&line, &linecap, f)) > 0) {
    ++lineno;
  }
  TOOL_CHECK(
      read, !feof(f),
      "Parameter 'offset' exceeds the number of lines (%zu lines) in the file",
      lineno);

  char line_buf[256];
  int lineno_len = snprintf(line_buf, sizeof(line_buf), "%zu", lineno) + 4;

  bool partial_read = false;
  while (lineno < end_lineno && jstring_len(contents) < READ_TOOL_MAX_SIZE &&
         (line_len = getline(&line, &linecap, f)) > 0) {
    snprintf(line_buf, sizeof(line_buf), "%*zu: ", lineno_len, lineno);
    jstring_concat(contents, line_buf);
    size_t read_len = line_len;
    if (jstring_len(contents) + line_len >= READ_TOOL_MAX_SIZE) {
      read_len = READ_TOOL_MAX_SIZE - jstring_len(contents);
      partial_read = true;
    } else {
      ++lineno;
    }
    jstring_nconcat(contents, line, read_len);
  }

  if (partial_read) {
    jstring_concat(contents, "[Partial View]");
    jnode_t* warning =
        jstring_new(0, "Incomplete Read: Read exceeded maximum size of 64KB. ");
    TOOL_CHECK(read, warning, "Failed to allocate memory for warning message");
    jobject_put(res, "warning", warning);
    snprintf(line_buf, sizeof(line_buf), "Read %zu lines. ",
             lineno - offset + 1);
    jstring_concat(warning, line_buf);
    snprintf(line_buf, sizeof(line_buf), "Continue reading from line %zu.",
             lineno);
    jstring_concat(warning, line_buf);
  }

  log(INFO, "Read %zu lines from file: %s", lineno - offset + 1, path);

read_end:
  if (line) free(line);
  if (f) fclose(f);
  if (err == ERROR_NONE) {
    *result = jto_string(res);
    if (!*result) {
      err = ERROR_OUT_OF_MEMORY;
    } else {
      *result_len = strlen(*result);
    }
  }
  jdelete(args_node);
  jdelete(res);
  return err;
}