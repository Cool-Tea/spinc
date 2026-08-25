# spinc

A minimal AI agent written in C. `spinc` connects to any OpenAI-compatible Chat Completions API or the Anthropic Messages API, sends your prompt along with a set of tools, and automatically executes tool calls in a loop until the model produces a final answer.

## Design Philosophy

Inspired by [suckless](https://suckless.org), the agent is designed to be simple to modify. **If you want new feature, write yourself and enjoy your labor!**

## Features

- **Pure C agent loop** — sends messages to the API, executes any requested tool calls, feeds the results back, and repeats until the model answers.
- **Built-in tools** — `Read`, `Write`, `Edit`, and `Bash` let the model read files, write files, and run shell commands. **In fact, the README is written by this agent!**
- **Multi-protocol** — speaks both the OpenAI (`/chat/completions`) and Anthropic (`/v1/messages`) protocols behind a single abstraction, so it works with any OpenAI-compatible provider (e.g. DeepSeek) as well as Anthropic.
- **Bundled JSON library** — ships with [sjson](https://github.com/Cool-Tea/sjson), a compact JSON parser/serializer.
- **Colored logging** — per-file/function/line debug output, configurable at compile time.

## Project layout

```
spinc/
├── .gitignore          # gitignore file
├── LICENSE             # MIT LICENSE
├── Makefile            # Build configuration
└── src/
    ├── main.c          # CLI entry point
    ├── agent.c/h       # Agent loop & tool-call execution
    ├── model.c/h       # Protocol abstraction & API request building (OpenAI/Anthropic)
    ├── http.c/h        # libcurl HTTP POST wrapper
    ├── tools.c         # Tool implementations (Read/Write/Edit/Bash)
    ├── tool.h          # Tool/parameter definition macros & structs
    ├── config.def.h    # Config template (copy to config.h)
    ├── config.h        # Your local config (gitignored)
    ├── log.c/h         # Logging (macros, init, log levels)
    └── sjson.c/h       # Bundled JSON library
```

## Requirements

- A C23 compiler (the code is built with `-std=gnu23`)
- [libcurl](https://curl.se/libcurl/)
- [readline](https://tiswww.case.edu/php/chet/readline/rltop.html)
- `make`

## Build

```sh
make          # release build and debug build
make all      # release build and debug build
make release  # release build
make debug    # debug build
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
    .thinking         = "enabled",
    .reasoning_effort = "high",
    .top_p            = 0.1f,
    .max_tokens       = -1,
};
```

- `protocol` — the API protocol used (`OPENAI` or `ANTHROPIC`).
- `name` — the model identifier sent in the request body.
- `base_url` — the API base URL.
- `api_key` — the secret used for authentication.
- `thinking` — the model's reasoning/thinking mode as a string: `"enabled"`, `"disabled"` or `"adaptive"` (or `NULL` to omit the field).
- `reasoning_effort` — the reasoning effort level.
- `top_p` — nucleus sampling parameter sent with the request.
- `max_tokens` — the maximum number of tokens to generate; `-1` to let the API use its default.

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

Run `spinc` with the optional `-p` flag to pass a single prompt and exit:

```sh
./spinc -p "What files are in this directory?"
```

```sh
./spinc -p "Write a sentence introducing yourself to the file ./doc.txt"
```

Without `-p`, `spinc` starts an interactive REPL. Type prompts at the `user>` prompt, and quit with `/exit` or `/quit`:

```sh
$ ./spinc # or make run
user> What files are in this directory?
...
user> /exit
```

The agent will:

1. Send the system prompt, your prompt, and the tool definitions to the API.
2. If the model responds with tool calls, execute each tool locally.
3. Append the tool results to the conversation and call the API again.
4. Repeat until the model returns a plain text answer (no tool calls), which is printed to stdout.

## Note

The project is still under development, any bug encountered is possible and any contribution is welcomed!
