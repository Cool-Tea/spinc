#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "log.h"
#include "tool/tool.h"
#include "tool/utils.h"

/**
 * Edit an existing file by replacing exact old contents with new contents.
 * @param path The path to the file to edit.
 * @param old_string The string to replace in the file.
 * @param new_string The new string to replace with in the file.
 * @param replace_all Whether to replace all occurrences of the old string
 * (optional).
 * @return A JSON string with the result of the operation.
 */
err_t edit_tool(const char* args, size_t args_len, char** result,
                size_t* result_len) {
  log(INFO, "Calling Edit tool");
  log(DEBUG, "Edit tool args: %.*s", (int)args_len, args);

  err_t err = ERROR_NONE;
  jnode_t* args_node = NULL;
  jnode_t* res = NULL;
  FILE* f = NULL;
  FILE* swapf = NULL;
  char* buf = NULL;
  char* filename = NULL;

  if (!args || !result || !result_len) return ERROR_NULLPTR;
  *result = NULL;
  *result_len = 0;

  res = jobject_new();
  if (!res) return ERROR_OUT_OF_MEMORY;
  args_node = jfrom_string(args, (int)args_len);
  TOOL_CHECK(edit, args_node, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args_node, "path");
  jnode_t* old_string_node = jobject_get(args_node, "old_string");
  jnode_t* new_string_node = jobject_get(args_node, "new_string");
  TOOL_CHECK(edit, path_node, "Missing required parameter: path");
  TOOL_CHECK(edit, old_string_node, "Missing required parameter: old_string");
  TOOL_CHECK(edit, new_string_node, "Missing required parameter: new_string");
  TOOL_CHECK(edit, jis_string(path_node), "Parameter 'path' must be a string");
  TOOL_CHECK(edit, jis_string(old_string_node),
             "Parameter 'old_string' must be a string");
  TOOL_CHECK(edit, jis_string(new_string_node),
             "Parameter 'new_string' must be a string");

  jnode_t* replace_all_node = jobject_get(args_node, "replace_all");
  if (replace_all_node) {
    TOOL_CHECK(edit, jis_boolean(replace_all_node),
               "Parameter 'replace_all' must be a boolean");
  }

  const char* path = jstring_content(path_node);
  const char* old_string = jstring_content(old_string_node);
  const char* new_string = jstring_content(new_string_node);
  bool replace_all =
      replace_all_node ? jas_bool(replace_all_node)->value : false;

  size_t path_len = strlen(path);
  filename = malloc(path_len + 6);
  TOOL_CHECK(edit, filename, "Failed to allocate memory for temp filename");
  snprintf(filename, path_len + 6, "%s.swap", path);

  f = fopen(path, "rb");
  TOOL_CHECK(edit, f, "Failed to open file for reading: %s", path);

  swapf = fopen(filename, "wb");
  TOOL_CHECK(edit, swapf, "Failed to open temp file for writing: %s", filename);

  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  buf = malloc(file_size + 1);
  TOOL_CHECK(edit, buf, "Failed to allocate memory for file contents");
  size_t read_size = fread(buf, 1, file_size, f);
  TOOL_CHECK(edit, read_size == file_size, "Failed to read file: %s", path);
  buf[file_size] = '\0';

  // Count occurrences of old_string in buf
  size_t count = 0, old_str_len = strlen(old_string),
         new_str_len = strlen(new_string);
  for (const char* p = buf; (p = strstr(p, old_string)) != NULL;
       p += old_str_len) {
    count++;
  }
  TOOL_CHECK(edit, count > 0, "Old string not found in file: %s", path);
  TOOL_CHECK(edit, replace_all || count == 1,
             "Old string found multiple times in file: %s. Use replace_all to "
             "replace all occurrences or provide more context to nail down the "
             "correct one.",
             path);

  for (const char *it = buf, *next; it < buf + file_size; it = next) {
    next = strstr(it, old_string);
    if (!next) next = buf + file_size;
    size_t copy_len = next - it;
    size_t write_len = fwrite(it, 1, copy_len, swapf);
    TOOL_CHECK(edit, write_len == copy_len, "Failed to write to temp file: %s",
               filename);
    if (next < buf + file_size) {
      write_len = fwrite(new_string, 1, new_str_len, swapf);
      TOOL_CHECK(edit, write_len == new_str_len,
                 "Failed to write to temp file: %s", filename);
      next += old_str_len;
    }
  }

  TOOL_CHECK(edit, rename(filename, path) == 0,
             "Failed to apply changes to original file: %s", path);
  jobject_put(res, "success", jbool_new(1));

edit_end:
  if (filename) free(filename);
  if (buf) free(buf);
  if (swapf) fclose(swapf);
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
