#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjson.h"

#include "log.h"
#include "tool/tool.h"
#include "tool/utils.h"

/**
 * Execute a bash command and return its output.
 * @param command The bash command to execute.
 * @return A JSON string with the command output.
 */
err_t bash_tool(const char* args, size_t args_len, char** result,
                size_t* result_len) {
  log(INFO, "Calling Bash tool");
  log(DEBUG, "Bash tool args: %.*s", (int)args_len, args);

  err_t err = ERROR_NONE;
  jnode_t* args_node = NULL;
  jnode_t* res = NULL;
  FILE* pipe = NULL;

  if (!args || !result || !result_len) return ERROR_NULLPTR;
  *result = NULL;
  *result_len = 0;

  res = jobject_new();
  if (!res) return ERROR_OUT_OF_MEMORY;
  args_node = jfrom_string(args, (int)args_len);
  TOOL_CHECK(bash, args_node, "Failed to parse arguments JSON: %s", jerror());

  jnode_t* command_node = jobject_get(args_node, "command");
  TOOL_CHECK(bash, command_node, "Missing required parameter: command");
  TOOL_CHECK(bash, jis_string(command_node),
             "Parameter 'command' must be a string");

  const char* command = jstring_content(command_node);
  pipe = popen(command, "r");
  TOOL_CHECK(bash, pipe, "Failed to execute bash command: %s", command);

  jnode_t* output = jstring_new(0, "");
  TOOL_CHECK(bash, output, "Failed to allocate memory for command output");
  jobject_put(res, "stdout", output);

  char buf[1024];
  while (fgets(buf, sizeof(buf), pipe) != NULL) {
    jstring_concat(output, buf);
  }

bash_end:
  if (pipe) pclose(pipe);
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
