#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#ifdef TYPING
#include "tool/tool.h"
#endif

DECLARE_TOOL(
    Read, "Read a file and return its contents with line numbers", read_tool,
    DEFINE_PARAM(true, "path", "string", "The path to the file to read"),
    DEFINE_PARAM(false, "offset", "integer",
                 "The line number to start reading from (1-based index)"),
    DEFINE_PARAM(false, "limit", "integer",
                 "The maximum number of lines to read"));

DECLARE_TOOL(Write, "Create a new file or overwrite an existing file",
             write_tool,
             DEFINE_PARAM(true, "path", "string",
                          "The path to the file to write"),
             DEFINE_PARAM(true, "contents", "string",
                          "The contents to write to the file"));

DECLARE_TOOL(
    Edit,
    "Edit an existing file by replacing exact old contents with new "
    "contents",
    edit_tool,
    DEFINE_PARAM(true, "path", "string", "The path to the file to edit"),
    DEFINE_PARAM(true, "old_string", "string",
                 "The string to replace in the file"),
    DEFINE_PARAM(true, "new_string", "string",
                 "The new string to replace with in the file"),
    DEFINE_PARAM(false, "replace_all", "boolean",
                 "Whether to replace all occurrences of the old string"));

DECLARE_TOOL(Bash, "Execute a bash command and return its output", bash_tool,
             DEFINE_PARAM(true, "command", "string",
                          "The bash command to execute"));

static tool_t tools[] = {
    DEFINE_TOOL(Read, read_tool),
    DEFINE_TOOL(Write, write_tool),
    DEFINE_TOOL(Edit, edit_tool),
    DEFINE_TOOL(Bash, bash_tool),
};

static const toolset_t registry = {
    .n_tool = sizeof(tools) / sizeof(const tool_t),
    .cap = sizeof(tools) / sizeof(const tool_t),
    .tools = tools,
};

#endif  // TOOL_REGISTRY_H