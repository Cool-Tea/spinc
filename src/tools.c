#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "sjson.h"

#include "log.h"

#define READ_TOOL_MAX_SIZE (64 * 1024)  // 64KB

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

/**
 * Read a file and return its contents with line numbers.
 * @param path The path to the file to read.
 * @param offset The line number offset to start reading from (optional).
 * @param limit The number of lines to read (optional).
 * @return A JSON string with the file contents and line numbers.
 */
char* read_tool(const char* args_str) {
  log(INFO, "Calling Read tool");
  log(DEBUG, "Read tool args: %s", args_str);

  jnode_t* args = NULL;
  jnode_t* result = NULL;
  char* result_str = NULL;
  FILE* f = NULL;

  args = jfrom_string(args_str);
  result = jobject_new();
  TOOL_CHECK(read, args, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args, "path");
  TOOL_CHECK(read, path_node, "Missing required parameter: path");
  TOOL_CHECK(read, jis_string(path_node), "Parameter 'path' must be a string");

  jnode_t* offset_node = jobject_get(args, "offset");
  if (offset_node) {
    TOOL_CHECK(read, jis_number(offset_node),
               "Parameter 'offset' must be an integer");
  }

  jnode_t* limit_node = jobject_get(args, "limit");
  if (limit_node) {
    TOOL_CHECK(read, jis_number(limit_node),
               "Parameter 'limit' must be an integer");
  }

  const char* path = jstring_content(path_node);
  f = fopen(path, "rb");
  TOOL_CHECK(read, f, "Failed to open file: %s", path);

  jnode_t* contents = jstring_new(0, "");
  TOOL_CHECK(read, contents, "Failed to allocate memory for file contents");
  jobject_put(result, "contents", contents);

  size_t offset = 0;
  if (offset_node) {
    offset = (size_t)jas_number(offset_node)->value;
  }

  size_t end_lineno = (size_t)-1;
  if (limit_node) {
    end_lineno = offset + (size_t)jas_number(limit_node)->value;
  }

  size_t lineno = 1, last_lineno = 0;
  char buf[1024];
  // skip lines until offset
  while (fgets(buf, sizeof(buf), f) != NULL && lineno < offset) {
    if (strchr(buf, '\n')) {
      last_lineno = lineno;
      ++lineno;
    }
  }
  TOOL_CHECK(
      read, !feof(f),
      "Parameter 'offset' exceeds the number of lines (%zu lines) in the file",
      last_lineno);
  while (fgets(buf, sizeof(buf), f) != NULL && lineno < end_lineno &&
         jstring_len(contents) < READ_TOOL_MAX_SIZE) {
    if (lineno > last_lineno) {
      char linebuf[32];
      snprintf(linebuf, sizeof(linebuf), "%8zu: ", lineno);
      jstring_concat(contents, linebuf);
    }
    if (strchr(buf, '\n')) {
      last_lineno = lineno;
      ++lineno;
    }
    jstring_concat(contents, buf);
  }

  if (jstring_len(contents) >= READ_TOOL_MAX_SIZE) {
    jstring_concat(contents, "[Partial View]");
    jnode_t* warning =
        jstring_new(0, "Incomplete Read: Read exceeded maximum size of 64KB. ");
    TOOL_CHECK(read, warning, "Failed to allocate memory for warning message");
    jobject_put(result, "warning", warning);
    snprintf(buf, sizeof(buf), "Read %zu lines. ", last_lineno);
    jstring_concat(warning, buf);
    snprintf(buf, sizeof(buf), "Continue reading from line %zu.", last_lineno);
    jstring_concat(warning, buf);
  }

read_end:
  if (f) fclose(f);
  result_str = jto_string(result);
  jdelete(args);
  jdelete(result);
  return result_str;
}

/**
 * Create a new file or overwrite an existing file.
 * @param path The path to the file to write.
 * @param contents The contents to write to the file.
 * @return A JSON string with the result of the operation.
 */
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

/**
 * Execute a bash command and return its output.
 * @param command The bash command to execute.
 * @return A JSON string with the command output.
 */
char* bash_tool(const char* args_str) {
  log(INFO, "Calling Bash tool");
  log(DEBUG, "Bash tool args: %s", args_str);

  jnode_t* args = NULL;
  jnode_t* result = NULL;
  char* result_str = NULL;
  FILE* pipe = NULL;

  args = jfrom_string(args_str);
  result = jobject_new();
  TOOL_CHECK(bash, args, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* command_node = jobject_get(args, "command");
  TOOL_CHECK(bash, command_node, "Missing required parameter: command");
  TOOL_CHECK(bash, jis_string(command_node),
             "Parameter 'command' must be a string");

  const char* command = jstring_content(command_node);
  pipe = popen(command, "r");
  TOOL_CHECK(bash, pipe, "Failed to execute bash command: %s", command);

  jnode_t* output = jstring_new(0, "");
  TOOL_CHECK(bash, output, "Failed to allocate memory for command output");
  jobject_put(result, "stdout", output);

  char buf[1024];
  while (fgets(buf, sizeof(buf), pipe) != NULL) {
    jstring_concat(output, buf);
  }

bash_end:
  if (pipe) pclose(pipe);
  result_str = jto_string(result);
  jdelete(args);
  jdelete(result);
  return result_str;
}