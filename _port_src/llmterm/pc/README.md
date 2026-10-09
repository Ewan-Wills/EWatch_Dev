# llmterm — Mac companion bridge

Relays prompts from the EWatch to a local LLM on the Mac and streams the answer
back over the USB-serial link. The watch never runs a model and never touches
the network — all inference happens here.

## 1. Install a model runner

### Option A — Ollama (preferred)

```sh
# Install (Homebrew), or grab the app from https://ollama.com/download
brew install ollama

# Start the server (leave it running in its own terminal; the app also
# auto-starts a background server on macOS)
ollama serve

# Pull a small, fast model (first run downloads ~1 GB)
ollama pull llama3.2:1b
#   even smaller / faster alternatives:
#   ollama pull qwen2.5:0.5b
#   ollama pull tinyllama
```

Ollama listens on `http://localhost:11434`. Sanity check:
```sh
curl http://localhost:11434/api/tags
```

### Option B — llama.cpp server (fallback)

```sh
brew install llama.cpp
# serve any GGUF with the built-in HTTP server on :8080
llama-server -m /path/to/model.gguf --port 8080
```
The bridge auto-falls back to this if Ollama isn't reachable, or force it with
`--backend llamacpp`.

## 2. Install Python deps

```sh
cd EWatch_Firmware/ShortsApps/llmterm/pc
python3 -m venv .venv && source .venv/bin/activate      # optional but tidy
pip install -r requirements.txt
```

## 3. Run the bridge

Flash the watch firmware first (see ../NOTES.md), then plug it in and:

```sh
python3 llmterm_bridge.py                       # auto port + Ollama + llama3.2:1b
python3 llmterm_bridge.py --model qwen2.5:0.5b  # a different Ollama model
python3 llmterm_bridge.py --port /dev/cu.usbmodem1101
python3 llmterm_bridge.py --backend llamacpp --llamacpp-url http://localhost:8080
```

The port auto-detects the first `/dev/cu.usbmodem*` (macOS). If several devices
are present, pass `--port` explicitly — list them with:
```sh
ls /dev/cu.usbmodem*
```

Leave the bridge running. On the watch: swipe up → **LLM Terminal** → tap a
prompt. Tokens stream onto the screen.

> **Serial is a shared channel.** The same USB port is used for flashing and for
> `pio ... -t monitor`. Close any serial monitor before starting the bridge —
> only one process can own the port at a time.

## Flags

| Flag | Default | Notes |
|---|---|---|
| `--port` | auto | serial device; `/dev/cu.usbmodem*` is auto-detected |
| `--model` | `llama3.2:1b` | Ollama model tag |
| `--backend` | `auto` | `auto` \| `ollama` \| `llamacpp` |
| `--ollama-url` | `http://localhost:11434/api/generate` | |
| `--llamacpp-url` | `http://localhost:8080` | |
| `--timeout` | `120` | HTTP timeout in seconds |
| `--baud` | `115200` | ignored by native USB CDC, kept for completeness |

## Protocol (what crosses the wire)

```
watch -> PC :  PROMPT <text>\n
PC -> watch :  >>TOK <chunk>\n     one streamed token/chunk (zero or more)
               >>END\n             stream finished
               >>ERR <message>\n   backend unreachable / failed
```
Chunks are escaped so one token == one line: `\` → `\\`, newline → `\n`
(two chars). The watch un-escapes, so a `\n` inside a token becomes a hard line
break in the wrapped output. Any other `>>`-prefixed line is ignored by the
watch, so boot-log noise is harmless.
