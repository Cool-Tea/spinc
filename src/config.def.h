/***********************
 * Model configuration *
 ***********************/

static const model_t model = {
    .protocol = OPENAI,
    .name = "deepseek-v4-flash",
    .base_url = "https://api.deepseek.com/v1",
    .api_key = NULL,  // Set your API key here
    .thinking = true,
    .reasoning_effort = "high",
    .top_p = 0.1f,
};

/***********************
 * Tools configuration *
 ***********************/

// tool functions (implemented in tools.c)
extern char* read_tool(const char* params);
extern char* write_tool(const char* params);
extern char* bash_tool(const char* params);

// tool definitions
static const tool_t tools[] = {
    DEFINE_TOOL(
        "Read", "Read a file and return its contents with line numbers.",
        read_tool,
        DEFINE_PARAM(true, "path", "string", "The path to the file to read."),
        DEFINE_PARAM(
            false, "offset", "integer",
            "The line number offset to start reading from. Defaults to 1."),
        DEFINE_PARAM(false, "limit", "integer",
                     "The number of lines to read. Defaults to reading the "
                     "entire file.")),

    DEFINE_TOOL(
        "Write", "Write contents to a file.", write_tool,
        DEFINE_PARAM(true, "path", "string", "The path to the file to write."),
        DEFINE_PARAM(true, "contents", "string",
                     "The contents to write to the file.")),

    DEFINE_TOOL(
        "Edit",
        "Edit an existing file by replacing exact old contents with new "
        "contents.",
        edit_tool,
        DEFINE_PARAM(true, "path", "string", "The path to the file to edit."),
        DEFINE_PARAM(true, "old_string", "string",
                     "The string to replace for in the file."),
        DEFINE_PARAM(true, "new_string", "string",
                     "The new string to replace with in the file."),
        DEFINE_PARAM(false, "replace_all", "boolean",
                     "Whether to replace all occurrences of the old string. "
                     "Defaults to false.")),

    DEFINE_TOOL("Bash", "Execute a bash command and return its output.",
                bash_tool,
                DEFINE_PARAM(true, "command", "string",
                             "The bash command to execute.")),
};
static const size_t n_tool = sizeof(tools) / sizeof(tool_t);

/*****************
 * System Prompt *
 *****************/

static const char* system_prompt =
    "You are a helpful assistant. You can call tools to read and write files, "
    "and execute bash commands. You must always use the tools when you need "
    "to read or write files, or execute bash commands. You must not make any "
    "assumptions about the file system or the environment. You must not "
    "execute any bash commands that are not explicitly requested by the user. "
    "You must not execute any bash commands that are not safe. You must not "
    "execute any bash commands that are not necessary to fulfill the user's "
    "request.";