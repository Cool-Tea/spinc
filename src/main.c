#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "sjson.h"

#include "log.h"
#include "tool.h"
#include "http.h"
#include "model.h"
#include "message.h"

#include "config.h"

int main(int argc, char* argv[]) {
  const char* prompt = NULL;
  if (getopt(argc, argv, "p:") == 'p') prompt = optarg;
  if (!prompt) {
    log(ERROR, "error: -p flag is required");
    return 1;
  }

  const char* base_url = model.base_url;
  const char* api_key = model.api_key;
  if (!base_url || !*base_url || !api_key || !*api_key) {
    log(ERROR, "base_url or api_key is not set in config.h");
    return 1;
  }

  if (!http_init()) {
    log(ERROR, "Failed to initialize HTTP client");
    return 1;
  }

  jnode_t* jmessages = jarray_new();
  jarray_add(jmessages, system_message(strdup("You are a helpful assistant.")));
  jarray_add(jmessages, user_message(strdup(prompt)));

  while (1) {
    char* resp = call_api(&model, OPENAI, jmessages, tools,
                          sizeof(tools) / sizeof(tool_t));
    if (!resp) {
      log(ERROR, "Failed to call API");
      break;
    }
    log(DEBUG, "Response: %s", resp);

    jnode_t* jresp = jfrom_string((char*)resp);
    free(resp);
    if (!jresp) {
      log(ERROR, "Failed to parse response JSON: %s", jerror());
      break;
    }

    jnode_t* choices = jobject_get(jresp, "choices");
    if (!choices || !jis_array(choices) || jarray_size(choices) == 0) {
      log(ERROR, "no choices in response");
      jdelete(jresp);
      break;
    }

    jnode_t* first = jarray_get(choices, 0);
    if (!first || !jis_object(first)) {
      log(ERROR, "first choice is not an object");
      jdelete(jresp);
      break;
    }

    jnode_t* message = jobject_get(first, "message");
    if (!message || !jis_object(message)) {
      log(ERROR, "first choice message is not an object");
      jdelete(jresp);
      break;
    }

    jnode_t* content = jobject_get(message, "content");
    jnode_t* tool_calls = jobject_get(message, "tool_calls");
    size_t tool_call_count = tool_calls ? jarray_size(tool_calls) : 0;

    jarray_add(jmessages, jcopy(message));

    if (!tool_call_count) {
      printf("%s\n", jstring_content(content));
      jdelete(jresp);
      break;
    } else {
      for (size_t i = 0; i < tool_call_count; ++i) {
        jnode_t* call = jarray_get(tool_calls, i);
        jnode_t* id = jobject_get(call, "id");
        jnode_t* func = jobject_get(call, "function");
        jnode_t* name = jobject_get(func, "name");
        jnode_t* args = jobject_get(func, "arguments");
        const char* tool_name = jstring_content(name);
        const tool_t* tool = NULL;
        for (size_t j = 0; j < sizeof(tools) / sizeof(tool_t); ++j) {
          if (strcmp(tools[j].def.name, tool_name) == 0) {
            tool = &tools[j];
            break;
          }
        }
        if (!tool) {
          log(ERROR, "Unknown tool: %s", tool_name);
          continue;
        }
        char* result = tool->func(jstring_content(args));
        log(DEBUG, "Tool '%s' returned: %s", tool->def.name, result);
        char* call_id = strdup(jstring_content(id));
        jarray_add(jmessages, tool_message(call_id, result));
      }
      jdelete(jresp);
    }
  }

  jdelete(jmessages);
  http_quit();
  return 0;
}