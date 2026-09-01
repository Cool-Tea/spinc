# spinc

A minimal AI agent written in C. `spinc` connects to any OpenAI-compatible Chat Completions API or the Anthropic Messages API, sends your prompt along with a set of tools, and automatically executes tool calls in a loop until the model produces a final answer.

## Design Philosophy

Inspired by [suckless](https://suckless.org/philosophy), the agent is designed to be simple to modify. **If you want new feature, write yourself and enjoy your labor!**

## Features

- **Pure C agent loop** — sends messages to the API, executes any requested tool calls, feeds the results back, and repeats until the model answers.
- **Built-in tools** — `Read`, `Write`, `Edit`, and `Bash` let the model read files, write files, and run shell commands. **In fact, the README is written by this agent!**
- **Multi-protocol** — speaks the OpenAI Chat Completion and Anthropic Message protocols behind a single provider abstraction. Two providers are bundled: `OPENAI_COMPATIBLE` and `ANTHROPIC_COMPATIBLE`.
- **Streaming** — token-by-token streaming via Server-Sent Events for both protocols, so reasoning and answers appear as they are generated instead of waiting for the full response.
- **Session management** — every run gets a unique UUID, and the conversation context is persisted to disk so any session can be resumed later with `-r <uuid>`.
- **Command system** — slash commands in the interactive REPL with readline tab completion.
- **Conversation management** — `/history` lists every conversation turn, and `/rewind <turn_index>` rolls the session back to any earlier turn so you can retry or branch off in a different direction.
- **Bundled JSON library** — ships with [sjson](https://github.com/Cool-Tea/sjson), a compact JSON parser/serializer.
- **Colored logging** — per-file/function/line debug output, written to a per-session log file, configurable at compile time.

## Project layout

```
spinc/
├── .gitignore          # gitignore file
├── LICENSE             # MIT LICENSE
├── Makefile            # Build configuration
└── src/
    ├── main.c          # CLI entry point
    ├── agent.c/h       # Agent loop & tool-call execution
    ├── session.c/h     # Session lifecycle, persistence & resume
    ├── error.c/h       # Error codes & helpers
    ├── http.c/h        # libcurl HTTP POST wrapper
    ├── model.h         # Model configuration struct
    ├── config.def.h    # Config template (copy to config.h)
    ├── config.h        # Your local config (gitignored)
    ├── log.h           # Logging macros & levels
    ├── sjson.c/h       # Bundled JSON library
    ├── command/        # Slash commands
    ├── provider/       # Provider abstraction & protocol implementations
    └── tool/           # Tools framework
```

## Requirements

- A C23 compiler (the code is built with `-std=gnu23`)
- `make`
- [libcurl](https://curl.se/libcurl/)
- [readline](https://tiswww.case.edu/php/chet/readline/rltop.html)
- [libuuid](https://man7.org/linux/man-pages/man3/uuid.3.html)

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

### Runtime directory

The root directory under which all session data lives:

```c
static const char* run_dir = ".spinc";
```

Each session gets its own `<run_dir>/sessions/<uuid>/` directory containing the serialized conversation context (`context.json`) and per-session logs (`log/`).

### Model

```c
static const protyp_t provider_type = OPENAI_COMPATIBLE;
static const model_t model = {
    .name             = "deepseek-v4-flash",
    .base_url         = "https://api.deepseek.com",
    .api_key          = NULL,                    // <- set your key
    .thinking         = "enabled",
    .reasoning_effort = "high",
    .top_p            = 0.1f,
    .max_tokens       = -1,
    .stream           = false,
};
```

- `provider_type` — the provider implementation to use: `OPENAI_COMPATIBLE` or `ANTHROPIC_COMPATIBLE` (see `src/provider/provider.h`).
- `name` — the model identifier sent in the request body.
- `base_url` — the API base URL.
- `api_key` — the secret used for authentication.
- `thinking` — the model's reasoning/thinking mode as a string: `"enabled"`, `"disabled"` or `"adaptive"` (or `NULL` to omit the field).
- `reasoning_effort` — the reasoning effort level.
- `top_p` — nucleus sampling parameter sent with the request.
- `max_tokens` — the maximum number of tokens to generate; `-1` to let the API use its default.
- `stream` — set to `true` to enable streaming mode: the model's reasoning and answer are printed incrementally as they are generated instead of after the full response arrives. Works for both providers (`OPENAI_COMPATIBLE` and `ANTHROPIC_COMPATIBLE`).

### Tools

Tools are declared with the `DECLARE_TOOL` / `DEFINE_TOOL` / `DEFINE_PARAM` macros and implemented in `src/tool/` (one file per tool). The registry in `src/tool/registry.h` registers the tools, and the ones to enable are listed in `src/config.h`. By default the following tools are registered:

| Tool    | Description                                                             | Parameters                                                                                                |
| ------- | ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `Read`  | Read a file and return its contents with line numbers                   | `path` (string, required), `offset` (integer, optional, default `1`), `limit` (integer, optional)         |
| `Write` | Create a new file or overwrite an existing file                         | `path`, `contents` (string, required)                                                                     |
| `Edit`  | Edit an existing file by replacing exact old contents with new contents | `path`, `old_string`, `new_string` (string, required), `replace_all` (boolean, optional, default `false`) |
| `Bash`  | Execute a bash command and return its output                            | `command` (string, required)                                                                              |

#### Adding a tool

Implement a `err_t foo_tool(const char* args, size_t args_len, char** result, size_t* result_len)` function in its own file under `src/tool/`, declare it with `DECLARE_TOOL` in `src/tool/registry.h`, and enable it in `src/config.h`. Tool functions receive a JSON string of the arguments and, on success, return `ERROR_NONE` and produce a JSON result string; system-level failures are reported through the `err_t` return value.

### System prompt

The model's system prompt is defined as `system_prompt` in `src/config.h`.

### Logging

Logging is configured in `src/config.h`:

```c
static enum log_level log_level = DEBUG;
```

- `log_level` — the minimum level to emit. Valid values, in increasing verbosity order, are `ALL`, `DEBUG`, `INFO`, `WARN`, `ERROR`, and `DISABLE` (see `src/log.h`).

Logs are written per-session to `run_dir/sessions/<uuid>/log/<YYYYMMDD>.log` (one file per day).

## Usage

Run `spinc` with the optional flags:

- `-p <prompt>` — pass a single prompt and exit:

```sh
./spinc -p "What files are in this directory?"
```

```sh
./spinc -p "Write a sentence introducing yourself to the file ./doc.txt"
```

- `-r <uuid>` — resume a previously saved session (the UUID is printed at startup and again on exit):

```sh
./spinc -r 1ebb9025-6717-40ea-b4e6-77868fa68012
```

Without `-p`, `spinc` starts an interactive REPL. Type prompts at the `user>` prompt, and use slash commands — `/help` lists them, `/exit` or `/quit` leave the program, `/history` shows the conversation so far, and `/rewind <turn_index>` rolls the conversation back to an earlier turn:

```sh
$ ./spinc # or make run
user> What files are in this directory?
...
user> /help
   exit      Exit the program.
   help      Show this help message.
   history   Show the conversation history.
   quit      Exit the program.
   rewind    Rewind the conversation.
user> /history
--- Conversation History ---
Turn 1: What files are in this directory?
Turn 2: Write a sentence introducing yourself to the file ./doc.txt
user> /rewind 1
user> /exit
```

`/history` prints each conversation turn (numbered from 1, showing the prompt you typed, truncated to 100 characters). `/rewind <turn_index>` discards everything after the given turn — the turn index must be within `[1, <number of turns>]` — so you can retry or continue from an earlier point in the session.

Every run belongs to a session identified by a UUID, printed at startup:

```
Session uuid: 1ebb9025-6717-40ea-b4e6-77868fa68012
```

On exit, `spinc` saves the conversation context and prints the UUID again so you can resume the session later:

```
You can resume this session later with the uuid: 1ebb9025-6717-40ea-b4e6-77868fa68012
```

The agent will:

1. Send the system prompt, your prompt, and the tool definitions to the API.
2. If the model responds with tool calls, execute each tool locally.
3. Append the tool results to the conversation and call the API again.
4. Repeat until the model returns a plain text answer (no tool calls), which is printed to stdout.

With `stream = true` in `src/config.h`, the API is called in streaming mode: reasoning and answer text are printed as they are generated, and tool calls are still executed within the same loop.

## Note

The project is still under development, any bug encountered is possible and any contribution is welcomed!
