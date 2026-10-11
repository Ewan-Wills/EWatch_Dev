"""MCP server (stdio, JSON-RPC 2.0) for the bridge, so an LLM agent can drive
the watch: choose what it shows, launch apps, read input, take snapshots,
inject test input. A thin layer over the HTTP API; the bridge must be running.

Register with Claude Code:
    claude mcp add ewatch -- /path/to/companions/screenshare/bin/ewbridge mcp
"""
import base64
import json
import sys

from .cli import ApiError, api

PROTO = "2024-11-05"

W = {"watch": {"type": "string", "description": "watch id (see ewatch_status); default: the first connected"}}

TOOLS = [
    {"name": "ewatch_status",
     "description": "Connected watches (what each shows, orientation, fps, latency, latest input) and apps.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "ewatch_list_windows",
     "description": "On-screen Mac windows that can be shown on the watch (id, owner, title, size).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "ewatch_show",
     "description": "Choose what the watch shows. kind: app (name), window (id or name), monitor "
                    "(virtual second display; view fit|follow, zoom), screen (display), url (an MJPEG stream or "
                    "JPEG/PNG URL), test, none. "
                    "Showing an app that isn't running launches it.",
     "inputSchema": {"type": "object", "required": ["kind"], "properties": dict(W, **{
         "kind": {"type": "string", "enum": ["app", "window", "monitor", "screen", "url", "test", "none"]},
         "url": {"type": "string", "description": "for kind url"},
         "name": {"type": "string", "description": "app name, or window owner/title substring"},
         "id": {"type": "integer", "description": "window id from ewatch_list_windows"},
         "view": {"type": "string", "enum": ["fit", "follow"]},
         "zoom": {"type": "number"},
         "display": {"type": "integer"}})}},
    {"name": "ewatch_orientation",
     "description": "Set the watch picture orientation.",
     "inputSchema": {"type": "object", "required": ["orientation"], "properties": dict(W, **{
         "orientation": {"type": "string", "enum": ["portrait", "landscape-left", "landscape-right"]}})}},
    {"name": "ewatch_apps",
     "description": "Apps that can be launched (apps/*.py) and the ones connected.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "ewatch_stop_app",
     "description": "Stop an app the bridge launched.",
     "inputSchema": {"type": "object", "required": ["name"], "properties": {"name": {"type": "string"}}}},
    {"name": "ewatch_snapshot",
     "description": "Read back what the watch is showing (its own copy of the panel pixels) as an image.",
     "inputSchema": {"type": "object", "properties": W}},
    {"name": "ewatch_input",
     "description": "Latest watch input (touch position, button, IMU in g) plus input events since `since` "
                    "(touch down/move/up, button down/up/short, gestures).",
     "inputSchema": {"type": "object", "properties": dict(W, since={"type": "integer"})}},
    {"name": "ewatch_haptic",
     "description": "Buzz the watch.",
     "inputSchema": {"type": "object", "properties": dict(W, ms={"type": "integer"},
                                                         strength={"type": "integer"})}},
    {"name": "ewatch_sim",
     "description": "Inject input on the watch as if a person did it (tests): 'tap X Y', "
                    "'drag X1 Y1 X2 Y2 MS', 'swipe l|r|u|d', 'btn short', 'btn long', "
                    "'tilt GX GY GZ', 'tilt off'. Coordinates are panel pixels (240x280, portrait).",
     "inputSchema": {"type": "object", "required": ["cmd"], "properties": dict(W, cmd={"type": "string"})}},
    {"name": "ewatch_console",
     "description": "Run a watch console command and return its output: 'log dump' (persistent log incl. "
                    "boot/reset reasons and panics), 'pwr stats', 'help'. Only while Screenshare is not open.",
     "inputSchema": {"type": "object", "required": ["line"], "properties": dict(W, line={"type": "string"},
                                                                         seconds={"type": "number"})}},
    {"name": "ewatch_release",
     "description": "Let go of the watch's USB port for N seconds (so `ewt flash` can use it).",
     "inputSchema": {"type": "object", "properties": dict(W, seconds={"type": "number"})}},
]


def call(name, a):
    wq = f"?watch={a['watch']}" if a.get("watch") else ""
    if name == "ewatch_status":
        return api("GET", "/api/status")
    if name == "ewatch_list_windows":
        return api("GET", "/api/windows")
    if name == "ewatch_apps":
        return api("GET", "/api/apps")
    if name == "ewatch_show":
        spec = {k: v for k, v in a.items() if v is not None}
        if spec["kind"] == "app":
            st = api("GET", "/api/status")
            if spec.get("name") not in [c["name"] for c in st["apps"]["connected"]]:
                import time
                api("POST", "/api/apps/launch", {"name": spec["name"], "watch": a.get("watch")})
                for _ in range(80):
                    if spec["name"] in [c["name"] for c in api("GET", "/api/status")["apps"]["connected"]]:
                        break
                    time.sleep(0.1)
        return api("POST", "/api/source", spec)
    if name == "ewatch_orientation":
        return api("POST", "/api/orientation", a)
    if name == "ewatch_stop_app":
        return api("POST", "/api/apps/stop", a)
    if name == "ewatch_snapshot":
        png = api("GET", "/api/snapshot.png" + wq, raw=True)
        return {"_image": base64.b64encode(png).decode()}
    if name == "ewatch_input":
        st = api("GET", "/api/input" + wq)
        ev = api("GET", f"/api/events?since={int(a.get('since', 0))}"
                 + (f"&watch={a['watch']}" if a.get("watch") else ""))
        return {"state": st, "events": ev[-100:]}
    if name == "ewatch_haptic":
        return api("POST", "/api/haptic", a)
    if name == "ewatch_sim":
        return api("POST", "/api/sim", a)
    if name == "ewatch_console":
        return api("POST", "/api/console", a, timeout=float(a.get("seconds", 2)) + 10)
    if name == "ewatch_release":
        return api("POST", "/api/release", a)
    raise ValueError(f"unknown tool {name}")


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid, method = msg.get("id"), msg.get("method")
        if mid is None:                      # notification (initialized, cancelled, ...)
            continue
        try:
            if method == "initialize":
                res = {"protocolVersion": msg.get("params", {}).get("protocolVersion", PROTO),
                       "capabilities": {"tools": {}},
                       "serverInfo": {"name": "ewatch-bridge", "version": "1.0"}}
            elif method == "tools/list":
                res = {"tools": TOOLS}
            elif method == "tools/call":
                p = msg.get("params", {})
                try:
                    out = call(p.get("name"), p.get("arguments") or {})
                    if isinstance(out, dict) and "_image" in out:
                        content = [{"type": "image", "data": out["_image"], "mimeType": "image/png"}]
                    else:
                        content = [{"type": "text", "text": json.dumps(out, indent=1)}]
                    res = {"content": content}
                except (ApiError, ValueError, KeyError) as e:
                    res = {"content": [{"type": "text", "text": f"error: {e}"}], "isError": True}
            elif method == "ping":
                res = {}
            else:
                raise LookupError(method)
            out = {"jsonrpc": "2.0", "id": mid, "result": res}
        except LookupError as e:
            out = {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"no method {e}"}}
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()
