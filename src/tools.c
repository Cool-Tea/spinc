#include <complex.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
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
  char* line = NULL;

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
    jobject_put(result, "warning", warning);
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
 * Edit an existing file by replacing exact old contents with new contents.
 * @param path The path to the file to edit.
 * @param old_string The string to replace in the file.
 * @param new_string The new string to replace with in the file.
 * @param replace_all Whether to replace all occurrences of the old string
 * (optional).
 * @return A JSON string with the result of the operation.
 */
char* edit_tool(const char* args_str) {
  log(INFO, "Calling Edit tool");
  log(DEBUG, "Edit tool args: %s", args_str);

  jnode_t* args = NULL;
  jnode_t* result = NULL;
  char* result_str = NULL;
  FILE* f = NULL;
  FILE* swapf = NULL;
  char* buf = NULL;
  char* filename = NULL;

  result = jobject_new();
  args = jfrom_string(args_str);
  TOOL_CHECK(edit, args, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* path_node = jobject_get(args, "path");
  jnode_t* old_string_node = jobject_get(args, "old_string");
  jnode_t* new_string_node = jobject_get(args, "new_string");
  TOOL_CHECK(edit, path_node, "Missing required parameter: path");
  TOOL_CHECK(edit, old_string_node, "Missing required parameter: old_string");
  TOOL_CHECK(edit, new_string_node, "Missing required parameter: new_string");
  TOOL_CHECK(edit, jis_string(path_node), "Parameter 'path' must be a string");
  TOOL_CHECK(edit, jis_string(old_string_node),
             "Parameter 'old_string' must be a string");
  TOOL_CHECK(edit, jis_string(new_string_node),
             "Parameter 'new_string' must be a string");

  jnode_t* replace_all_node = jobject_get(args, "replace_all");
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
  jobject_put(result, "success", jbool_new(1));

edit_end:
  if (filename) free(filename);
  if (buf) free(buf);
  if (swapf) fclose(swapf);
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