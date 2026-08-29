#include <stdlib.h>
#include <string.h>

#include "tool/tool.h"

#define TOOLSET_INITIAL_CAPACITY 4
#define TOOLSET_GROWTH_FACTOR 2

static err_t toolset_reserve(toolset_t* toolset, size_t n_tool) {
  if (!toolset) return ERROR_NULLPTR;
  if (n_tool <= toolset->cap) return ERROR_NONE;
  size_t new_cap = toolset->cap;
  while (new_cap < n_tool) {
    new_cap *= TOOLSET_GROWTH_FACTOR;
  }
  tool_t* new_tools = realloc(toolset->tools, new_cap * sizeof(tool_t));
  if (!new_tools) return ERROR_OUT_OF_MEMORY;
  toolset->tools = new_tools;
  toolset->cap = new_cap;
  return ERROR_NONE;
}

err_t toolset_new(toolset_t** toolset) {
  if (!toolset) return ERROR_NULLPTR;
  *toolset = malloc(sizeof(toolset_t));
  if (!*toolset) return ERROR_OUT_OF_MEMORY;
  toolset_t* ts = *toolset;
  ts->n_tool = 0;
  ts->cap = TOOLSET_INITIAL_CAPACITY;
  ts->tools = malloc(TOOLSET_INITIAL_CAPACITY * sizeof(tool_t));
  if (!ts->tools) {
    free(ts);
    *toolset = NULL;
    return ERROR_OUT_OF_MEMORY;
  }
  return ERROR_NONE;
}

void toolset_delete(toolset_t* toolset) {
  if (!toolset) return;
  toolset_clear(toolset);
  free(toolset->tools);
  free(toolset);
}

err_t toolset_copy(const toolset_t* src, toolset_t** dst) {
  if (!src || !dst) return ERROR_NULLPTR;
  *dst = NULL;
  err_t err = toolset_new(dst);
  if (err != ERROR_NONE) return err;
  toolset_t* ts = *dst;
  err = toolset_reserve(ts, src->n_tool);
  if (err != ERROR_NONE) {
    toolset_delete(ts);
    *dst = NULL;
    return err;
  }
  memcpy(ts->tools, src->tools, src->n_tool * sizeof(tool_t));
  ts->n_tool = src->n_tool;
  return ERROR_NONE;
}

err_t toolset_add(toolset_t* toolset, const tool_t* tool) {
  if (!toolset || !tool) return ERROR_NULLPTR;
  err_t err = toolset_reserve(toolset, toolset->n_tool + 1);
  if (err != ERROR_NONE) return err;
  toolset->tools[toolset->n_tool] = *tool;
  ++toolset->n_tool;
  return ERROR_NONE;
}

void toolset_clear(toolset_t* toolset) {
  if (!toolset) return;
  toolset->n_tool = 0;
}

err_t toolset_find(const toolset_t* toolset, const char* name, size_t name_len,
                   const tool_t** tool) {
  if (!toolset || !name || !tool) return ERROR_NULLPTR;
  for (size_t i = 0; i < toolset->n_tool; ++i) {
    if (memcmp(toolset->tools[i].def.name, name, name_len) == 0) {
      *tool = &toolset->tools[i];
      return ERROR_NONE;
    }
  }
  return ERROR_NOT_FOUND;
}

#include "tool/registry.h"

const toolset_t* global_toolset() { return &registry; }