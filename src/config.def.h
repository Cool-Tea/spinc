#ifdef TYPING
#include "log.h"
#include "provider/provider.h"
#endif

/***********************
 * Model configuration *
 ***********************/

static const protyp_t provider_type = DEEPSEEK;
static const model_t model = {
    .name = "deepseek-v4-flash",
    .base_url = "https://api.deepseek.com",
    .api_key = NULL,  // Set your API key here
    .thinking = "enabled",
    .reasoning_effort = "high",
    .top_p = 0.1f,
    .max_tokens = -1,
    .stream = false,
};

/***********************
 * Tools configuration *
 ***********************/

// Tool names to enable. Tools are implemented under src/tool/ and registered
// in src/tool/registry.h.
static const char* tools[] = {
    "Read",
    "Write",
    "Edit",
    "Bash",
};
static const size_t n_tool = sizeof(tools) / sizeof(const char*);

/*****************
 * System Prompt *
 *****************/

static const char* system_prompt =
    "You are a helpful assistant. You can call tools to read, write and edit "
    "files, and execute bash commands. You must always use the tools when you "
    "need to read or write files, or execute bash commands. You must not make "
    "any assumptions about the file system or the environment. You must not "
    "execute any bash commands that are not explicitly requested by the user. "
    "You must not execute any bash commands that are not safe. You must not "
    "execute any bash commands that are not necessary to fulfill the user's "
    "request.";

/***********
 * Logging *
 ***********/

static enum log_level log_level = DEBUG;
static const char* log_dir = ".spinc/logs";