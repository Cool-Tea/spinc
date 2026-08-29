#ifndef TOOL_H
#define TOOL_H

#include <stddef.h>
#include <stdbool.h>

#include "error.h"

#define DEFINE_PARAM(required, name, type, description) \
  (paramdef_t) { required, name, type, description }

#define DECLARE_TOOL(name, description, func, ...)             \
  static const char name##_name[] = #name;                     \
  static const char name##_description[] = description;        \
  static paramdef_t name##_params[] = {__VA_ARGS__};           \
  err_t func(const char* args, size_t args_len, char** result, \
             size_t* result_len)

#define DEFINE_TOOL(name, func)                                  \
  (tool_t) {                                                     \
    {name##_name, name##_description,                            \
     sizeof(name##_params) / sizeof(paramdef_t), name##_params}, \
        func                                                     \
  }

typedef struct paramdef {
  bool required;
  const char* name;
  const char* type;
  const char* description;
} paramdef_t;

typedef struct tooldef {
  const char* name;
  const char* description;
  size_t n_param;
  const paramdef_t* params;
} tooldef_t;

typedef struct tool {
  tooldef_t def;
  err_t (*func)(const char* args, size_t args_len, char** result,
                size_t* result_len);
} tool_t;

typedef struct toolset {
  size_t n_tool;
  size_t cap;
  tool_t* tools;
} toolset_t;

err_t toolset_new(toolset_t** toolset);
void toolset_delete(toolset_t* toolset);
err_t toolset_copy(const toolset_t* src, toolset_t** dst);
err_t toolset_add(toolset_t* toolset, const tool_t* tool);
void toolset_clear(toolset_t* toolset);
err_t toolset_find(const toolset_t* toolset, const char* name, size_t name_len,
                   const tool_t** tool);
const toolset_t* global_toolset();

#endif  // TOOL_H