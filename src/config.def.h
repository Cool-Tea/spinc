/***********************
 * Model configuration *
 ***********************/

static const model_t model = {
    .name = "deepseek-v4-flash",
    .base_url = "https://api.deepseek.com/v1",
    .api_key = NULL,  // Set your API key here
};

/***********************
 * Tools configuration *
 ***********************/

// tool functions (implemented in tools.c)
extern char* read_tool(const char* params);
extern char* write_tool(const char* params);

// tool definitions
static const tool_t tools[] = {
    DEFINE_TOOL(
        "Read", "Read a file and return its contents.", read_tool,
        DEFINE_PARAM(true, "path", "string", "The path to the file to read.")),
    DEFINE_TOOL(
        "Write", "Write contents to a file.", write_tool,
        DEFINE_PARAM(true, "path", "string", "The path to the file to write."),
        DEFINE_PARAM(true, "contents", "string",
                     "The contents to write to the file.")),
};