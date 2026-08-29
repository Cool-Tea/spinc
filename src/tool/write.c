#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "log.h"
#include "tool/tool.h"
#include "tool/utils.h"

/**
 * Create a new file or overwrite an existing file.
 * @param path The path to the file to write.
 * @param contents The contents to write to the file.
 * @return A JSON string with the result of the operation.
 */
err_t write_tool(const char* args, size_t args_len, char** result,
                 size_t* result_len) {
  log(INFO, "Calling Write tool");
  log(DEBUG, "Write tool args: %.*s", (int)args_len, args);

  err_t err = ERROR_NONE;
  jnode_t* args_node = NULL;
  jnode_t* res = NULL;
  FILE* f = NULL;

  if (!args || !result || !result_len) return ERROR_NULLPTR;
  *result = NULL;
  *result_len = 0;

  res = jobject_new();
  if (!res) return ERROR_OUT_OF_MEMORY;
  args_node = jfrom_string(args, (int)args_len);
  TOOL_CHECK(write, args_node, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args_node, "path");
  jnode_t* contents_node = jobject_get(args_node, "contents");
  TOOL_CHECK(write, path_node, "Missing required parameter: path");
  TOOL_CHECK(write, contents_node, "Missing required parameter: contents");
  TOOL_CHECK(write, jis_string(path_node), "Parameter 'path' must be a string");
  TOOL_CHECK(write, jis_string(contents_node),
             "Parameter 'contents' must be a string");

  const char* path = jstring_content(path_node);
  const char* contents = jstring_content(contents_node);
  f = fopen(path, "wb");
  TOOL_CHECK(write, f, "Failed to open file for writing: %s", path);

  size_t len = strlen(contents);
  TOOL_CHECK(write, fwrite(contents, 1, len, f) == len,
             "Failed to write to file: %s", path);
  jobject_put(res, "success", jbool_new(1));

write_end:
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
