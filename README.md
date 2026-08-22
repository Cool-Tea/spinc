# spinc

A minimal AI agent written in C. `spinc` connects to any OpenAI-compatible Chat Completions API, sends your prompt along with a set of tools, and automatically executes tool calls in a loop until the model produces a final answer.

## Design Philosophy

Inspired by [suckless](https://suckless.org), the agent is designed to be simple to modify. **If you want new feature, write yourself and enjoy your labor!**

## Features

- **Pure C agent loop** — sends messages to the API, executes any requested tool calls, feeds the results back, and repeats until the model answers.
- **Built-in tools** — `Read`, `Write`, `Edit`, and `Bash` let the model read files, write files, and run shell commands. **In fact, the README is written by this agent!**
- **OpenAI-compatible** — works with any provider exposing the `/chat/completions` endpoint (e.g. DeepSeek). A protocol abstraction (`OPENAI`/`ANTHROPIC`) exists for future providers.
- **Bundled JSON library** — ships with [sjson](https://github.com/Cool-Tea/sjson), a compact JSON parser/serializer.
- **Colored logging** — per-file/function/line debug output, configurable at compile time.

## Project layout

```
spinc/
├── Makefile            # Build configuration
├── src/
│   ├── main.c          # Agent loop & CLI entry point
│   ├── model.c/h       # API request building (OpenAI protocol)
│   ├── http.c/h        # libcurl HTTP POST wrapper
│   ├── tools.c         # Tool implementations (Read/Write/Edit/Bash)
│   ├── tool.h          # Tool/parameter definition macros & structs
│   ├── message.c/h     # Chat message (system/user/assistant/tool) builders
│   ├── config.def.h    # Config template (copy to config.h)
│   ├── config.h        # Your local config (gitignored)
│   ├── log.c/h         # Logging (macros, init, log levels)
│   ├── sjson.c/h       # Bundled JSON library
│   └── dynarr.h        # Generic dynamic array macros
└── spinc               # Built binary (output of `make`)
```

## Requirements

- A C23 compiler (the code is built with `-std=gnu23`)
- [libcurl](https://curl.se/libcurl/) development headers
- `make`

## Build

```sh
make
```

The default build enables AddressSanitizer for safer debugging:

```makefile
CFLAGS  := -Wall -Wextra -std=gnu23 -O2 -g -fsanitize=address
LDFLAGS := -lcurl -fsanitize=address
```

To build a release binary, change the flags in the `Makefile`:

```makefile
CFLAGS  := -Wall -Wextra -std=gnu23 -O2 -DLOG_LEVEL=INFO
LDFLAGS := -lcurl
```

Clean up build artifacts with `make clean`.

## Configuration

The project is configured via `src/config.h`, which is generated from the
template `src/config.def.h` and is intentionally gitignored:

```sh
cp src/config.def.h src/config.h
```

Then edit `src/config.h`:

### Model

```c
static const model_t model = {
    .protocol         = OPENAI,
    .name             = "deepseek-v4-flash",
    .base_url         = "https://api.deepseek.com",
    .api_key          = "your-api-key-here",   // <- set your key
    .thinking         = true,
    .reasoning_effort = "high",
    .top_p            = 0.1f,
};
```

- `protocol` — the API protocol used (`OPENAI` or `ANTHROPIC`).
- `name` — the model identifier sent in the request body.
- `base_url` — any OpenAI-compatible API base URL.
- `api_key` — the bearer token used for `Authorization: Bearer <key>`.
- `thinking` — enable/disable the model's reasoning/thinking mode.
- `reasoning_effort` — the reasoning effort level (e.g. `"low"`, `"medium"`, `"high"`).
- `top_p` — nucleus sampling parameter sent with the request.

### Tools

Tools are declared with the `DEFINE_TOOL` / `DEFINE_PARAM` macros and implemented in `src/tools.c`. By default the following tools are registered:

| Tool    | Description                                                             | Parameters                                                                                                |
| ------- | ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `Read`  | Read a file and return its contents with line numbers                   | `path` (string, required), `offset` (integer, optional, default `1`), `limit` (integer, optional)         |
| `Write` | Create a new file or overwrite an existing file                         | `path`, `contents` (string, required)                                                                     |
| `Edit`  | Edit an existing file by replacing exact old contents with new contents | `path`, `old_string`, `new_string` (string, required), `replace_all` (boolean, optional, default `false`) |
| `Bash`  | Execute a bash command and return its output                            | `command` (string, required)                                                                              |

#### Adding a tool

Implement a `char* foo_tool(const char* params)` function in `src/tools.c`, declare it in `config.h` (e.g. `extern char* foo_tool(const char* params);`), and register it in the `tools[]` array with `DEFINE_TOOL`. Tool functions receive a JSON string of the arguments and return a JSON string of the result.

### System prompt

The model's system prompt is defined as `system_prompt` in `src/config.h`.

### Logging

Logging is configured at the bottom of `src/config.h`:

```c
static enum log_level log_level = DEBUG;
static const char* log_dir = ".spinc/logs";
```

- `log_level` — the minimum level to emit. Valid values, in increasing verbosity order, are `ALL`, `DEBUG`, `INFO`, `WARN`, `ERROR`, and `DISABLE` (see `src/log.h`).
- `log_dir` — the directory where log files are written.

## Usage

`spinc` takes a single required flag, `-p`, with your prompt:

```sh
./spinc -p "What files are in this directory?"
```

```sh
./spinc -p "Write a sentence introducing yourself to the file ./doc.txt"
```

The agent will:

1. Send the system prompt, your prompt, and the tool definitions to the API.
2. If the model responds with tool calls, execute each tool locally.
3. Append the tool results to the conversation and call the API again.
4. Repeat until the model returns a plain text answer (no tool calls), which is printed to stdout.

## Note

The project is still under development, any bug encountered is possible and any contribution is welcomed!
