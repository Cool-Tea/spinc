#ifndef TOOL_H
#define TOOL_H

#include <stddef.h>
#include <stdbool.h>

#define TOOL_MAX_PARAMS 8

#define DEFINE_PARAM(required, name, type, description) \
  (paramdef_t) { required, name, type, description }

#define DEFINE_TOOL(name, description, function, ...)             \
  (tool_t) {                                                      \
    name, description,                                            \
        sizeof((paramdef_t[]){__VA_ARGS__}) / sizeof(paramdef_t), \
        {__VA_ARGS__}, function                                   \
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
  const paramdef_t params[TOOL_MAX_PARAMS];
} tooldef_t;

typedef struct tool {
  tooldef_t def;
  char* (*func)(const char* args);
} tool_t;

#endif  // TOOL_H