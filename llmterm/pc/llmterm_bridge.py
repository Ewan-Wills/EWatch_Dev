#!/usr/bin/env python3
"""llmterm bridge — relay prompts from the EWatch to a local LLM and stream the
answer back over USB serial.

Watch -> PC :  PROMPT <text>\n
PC -> Watch :  >>TOK <chunk>\n   (zero or more; one streamed token each)
               >>END\n          (stream finished)
               >>ERR <msg>\n     (something went wrong)

Backends, in order of preference:
  * Ollama           http://localhost:11434/api/generate  (streaming NDJSON)
  * llama.cpp server http://localhost:8080/completion       (streaming SSE)

Token chunks are escaped so exactly one token maps to one serial line:
  '\\' -> '\\\\'  and  newline -> '\\n'.  The watch un-escapes on receipt.

Usage:
  python3 llmterm_bridge.py                      # auto-detect port, Ollama, llama3.2:1b
  python3 llmterm_bridge.py --port /dev/cu.usbmodem1101 --model qwen2.5:0.5b
  python3 llmterm_bridge.py --backend llamacpp --llamacpp-url http://localhost:8080
"""
import argparse
import glob
import json
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed — run:  pip install -r requirements.txt")

try:
    import requests
except ImportError:
    sys.exit("requests not installed — run:  pip install -r requirements.txt")


def autodetect_port():
    # macOS native-USB CDC shows up as /dev/cu.usbmodem*; Linux as /dev/ttyACM*.
    cands = sorted(glob.glob("/dev/cu.usbmodem*") +
                   glob.glob("/dev/ttyACM*") +
                   glob.glob("/dev/ttyUSB*"))
    return cands[0] if cands else None


def esc(chunk: str) -> str:
    """Escape a token so it survives as a single newline-terminated line."""
    return chunk.replace("\\", "\\\\").replace("\r", "").replace("\n", "\\n")


# ---------------------------------------------------------------------------
# Backends — each yields text chunks as they arrive.
# ---------------------------------------------------------------------------
def stream_ollama(url, model, prompt, timeout):
    r = requests.post(url, json={"model": model, "prompt": prompt, "stream": True},
                      stream=True, timeout=timeout)
    r.raise_for_status()
    for raw in r.iter_lines(decode_unicode=True):
        if not raw:
            continue
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if obj.get("response"):
            yield obj["response"]
        if obj.get("done"):
            break


def stream_llamacpp(base_url, prompt, timeout):
    url = base_url.rstrip("/") + "/completion"
    r = requests.post(url, json={"prompt": prompt, "stream": True, "n_predict": 256},
                      stream=True, timeout=timeout)
    r.raise_for_status()
    for raw in r.iter_lines(decode_unicode=True):
        if not raw:
            continue
        # llama.cpp server streams Server-Sent Events: "data: {json}".
        if raw.startswith("data:"):
            raw = raw[len("data:"):].strip()
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if obj.get("content"):
            yield obj["content"]
        if obj.get("stop"):
            break


def pick_backend(args):
    """Return (name, generator_factory) honouring --backend / availability."""
    ollama_gen = (
        "ollama",
        lambda p: stream_ollama(args.ollama_url, args.model, p, args.timeout),
    )
    llamacpp_gen = (
        "llamacpp",
        lambda p: stream_llamacpp(args.llamacpp_url, p, args.timeout),
    )

    def ollama_up():
        try:
            requests.get(args.ollama_url.rsplit("/api/", 1)[0] + "/api/tags",
                         timeout=2).raise_for_status()
            return True
        except Exception:
            return False

    def llamacpp_up():
        try:
            requests.get(args.llamacpp_url.rstrip("/") + "/health",
                         timeout=2)
            return True
        except Exception:
            return False

    if args.backend == "ollama":
        return ollama_gen
    if args.backend == "llamacpp":
        return llamacpp_gen
    # auto
    if ollama_up():
        return ollama_gen
    if llamacpp_up():
        return llamacpp_gen
    # Default to Ollama and let the per-request error surface on the watch.
    return ollama_gen


def main():
    ap = argparse.ArgumentParser(description="EWatch llmterm <-> local LLM bridge")
    ap.add_argument("--port", help="serial port (auto-detects /dev/cu.usbmodem* if omitted)")
    ap.add_argument("--baud", type=int, default=115200, help="ignored by native USB CDC")
    ap.add_argument("--model", default="llama3.2:1b", help="Ollama model tag")
    ap.add_argument("--backend", choices=["auto", "ollama", "llamacpp"], default="auto")
    ap.add_argument("--ollama-url", default="http://localhost:11434/api/generate")
    ap.add_argument("--llamacpp-url", default="http://localhost:8080")
    ap.add_argument("--timeout", type=float, default=120.0, help="HTTP timeout (s)")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if not port:
        sys.exit("No serial port found — plug the watch in or pass --port.")

    name, factory = pick_backend(args)
    print(f"[llmterm] backend={name} model={args.model} port={port}", flush=True)

    ser = serial.Serial(port, args.baud, timeout=0.1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    try:
        ser.write(b">>READY\n")
    except Exception:
        pass

    buf = bytearray()
    print("[llmterm] waiting for PROMPT lines from the watch…  (Ctrl-C to quit)",
          flush=True)
    while True:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
        while b"\n" in buf:
            raw, _, rest = buf.partition(b"\n")
            buf = bytearray(rest)
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("PROMPT "):
                continue  # ignore watch boot noise / anything else
            prompt = line[len("PROMPT "):].strip()
            print(f"[llmterm] PROMPT: {prompt!r}", flush=True)
            n = 0
            try:
                for piece in factory(prompt):
                    # One model chunk == one >>TOK line; esc() folds any embedded
                    # newline into the literal two-char sequence \n for the watch.
                    ser.write((">>TOK " + esc(piece) + "\n").encode("utf-8"))
                    n += 1
                ser.write(b">>END\n")
                print(f"[llmterm] streamed {n} chunk(s); END", flush=True)
            except requests.exceptions.ConnectionError:
                msg = f"cannot reach {name} backend"
                ser.write((">>ERR " + esc(msg) + "\n").encode("utf-8"))
                print(f"[llmterm] ERR: {msg}", flush=True)
            except Exception as e:  # keep the bridge alive for the next prompt
                msg = f"{type(e).__name__}: {e}"[:160]
                ser.write((">>ERR " + esc(msg) + "\n").encode("utf-8"))
                print(f"[llmterm] ERR: {msg}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[llmterm] bye")
