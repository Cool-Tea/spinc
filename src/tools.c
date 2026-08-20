#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "sjson.h"

#include "log.h"

// clang-format off

#define TOOL_CHECK(name, condition, msg, ...)                \
  if (!(condition)) {                                        \
    log(ERROR, "[%s] tool error" msg, #name, ##__VA_ARGS__); \
    tool_error(result, msg, ##__VA_ARGS__);                  \
    goto name##_end;                                         \
  }

static
__attribute__((format(printf, 2, 3)))
void tool_error(jnode_t* result, const char* msg, ...) {
  va_list args1;
  va_start(args1, msg);
  va_list args2;
  va_copy(args2, args1);
  int len = vsnprintf(NULL, 0, msg, args1);
  va_end(args1);
  char* buf = malloc(len + 1);
  vsnprintf(buf, len + 1, msg, args2);
  va_end(args2);
  log(ERROR, "Error: %s", buf);
  jobject_put(result, "error", jstring_own(buf));
}

// clang-format on

char* read_tool(const char* args_str) {
  log(INFO, "Calling Read tool");
  log(DEBUG, "Read tool args: %s", args_str);

  jnode_t* args = NULL;
  jnode_t* result = NULL;
  char* result_str = NULL;
  FILE* f = NULL;
  char* buf = NULL;

  args = jfrom_string(args_str);
  result = jobject_new();
  TOOL_CHECK(read, args, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args, "path");
  TOOL_CHECK(read, path_node, "Missing required parameter: path");
  TOOL_CHECK(read, jis_string(path_node), "Parameter 'path' must be a string");

  const char* path = jstring_content(path_node);
  f = fopen(path, "rb");
  TOOL_CHECK(read, f, "Failed to open file: %s", path);

  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(size + 1);
  TOOL_CHECK(read, buf, "Failed to allocate memory for file contents");

  TOOL_CHECK(read, fread(buf, 1, size, f) == size,
             "Failed to read file contents: %s", path);
  buf[size] = '\0';
  jobject_put(result, "contents", jstring_own(buf));
  // Ownership transferred to result
  buf = NULL;

read_end:
  if (f) fclose(f);
  if (buf) free(buf);
  result_str = jto_string(result);
  jdelete(args);
  jdelete(result);
  return result_str;
}

char* write_tool(const char* args_str) {
  log(INFO, "Calling Write tool");
  log(DEBUG, "Write tool args: %s", args_str);

  jnode_t* args = NULL;
  jnode_t* result = NULL;
  char* result_str = NULL;
  FILE* f = NULL;

  result = jobject_new();
  args = jfrom_string(args_str);
  TOOL_CHECK(write, args, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args, "path");
  jnode_t* contents_node = jobject_get(args, "contents");
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
  jobject_put(result, "success", jbool_new(1));

write_end:
  if (f) fclose(f);
  result_str = jto_string(result);
  jdelete(args);
  jdelete(result);
  return result_str;
}