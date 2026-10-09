#!/usr/bin/env python3
"""Drive hw_reader over serial: crystal check, then for each TX power a fresh
join + speed test. Same script on every board, same spot -> comparable table.

  WIFI_PASS=... ~/.platformio/penv/bin/python tools/wifi_sweep.py /dev/cu.usbmodemXXXX ScrubHub [label]

The password is read from the environment so it never lands in a file or log
(the watch only prints its length). Output: console log + a summary table,
also saved to tools/results/<label>-<time>.txt.
"""
import os, re, sys, time, serial

port, ssid = sys.argv[1], sys.argv[2]
label = sys.argv[3] if len(sys.argv) > 3 else "board"
pw = os.environ["WIFI_PASS"]
LEVELS = [2, 5, 8, 11, 14, 17, 20]
log = []

s = serial.Serial(port, 115200, timeout=0.2)
def out(line):
    line = line.replace(pw, "<pass>")
    log.append(line); print(line, flush=True)
def send(cmd):
    s.write((cmd + "\n").encode()); out(f">>> {cmd.replace(pw, '<pass>')}")
def read_until(pats, timeout):
    """Collect lines until one matches a pattern; returns (index, line, lines)."""
    t0, buf, lines = time.time(), b"", []
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").rstrip()
            if line.startswith("[") and "| WIFI" in line: continue   # live line noise
            out(line); lines.append(line)
            for i, p in enumerate(pats):
                if re.search(p, line): return i, line, lines
    return -1, None, lines

time.sleep(0.5); s.reset_input_buffer()
send("p")                                   # pause live line (toggle)
i, l, _ = read_until([r"-- paused --", r"-- resumed --"], 3)
if i == 1: send("p"); read_until([r"-- paused --"], 3)
send("w"); read_until([r"network\(s\)"], 20)
_, _, scan = read_until([r"nothing saved|no saved|joining"], 3)

# row of the target AP in the scan table for the crystal check
send("w"); _, _, rows = read_until([r"network\(s\)"], 20)
row = next((int(m.group(1)) for r in rows if (m := re.match(r"\s+(\d+)\s+-?\d+.*\s" + re.escape(ssid) + r"\s*(\[saved\])?$", r))), 1)
send(f"x {row}")
_, xres, _ = read_until([r"result:", r"NOT a verdict"], 45)

results = []
for dbm in LEVELS:
    send(f"t {dbm}"); read_until([r"tx power cap now"], 3)
    send(f"j {ssid},{pw}")
    i, line, _ = read_until([r"connected in (\d+) ms", r"join gave up", r"EWatch hardware reader|Guru Meditation|rst:0x"], 25)
    if i == 2:                                   # the join crashed / reset the chip
        results.append({"tx": dbm, "join": "RESET", "ms": "", "rssi": "", "lat": "", "down": "", "up": "", "after": line})
        read_until([r"commands:"], 15); time.sleep(3); send("p"); read_until([r"-- paused --"], 3)
        continue
    row = {"tx": dbm, "join": "FAIL", "ms": "", "rssi": "", "lat": "", "down": "", "up": "", "after": ""}
    if i == 0:
        row["join"], row["ms"] = "ok", re.search(r"in (\d+) ms", line).group(1)
        time.sleep(1)
        send("s 2")
        _, _, sp = read_until([r"after\s+:", r"result : FAIL"], 90)
        for l in sp:
            if m := re.search(r"link\s+: RSSI (-?\d+)", l): row["rssi"] = m.group(1)
            if m := re.search(r"avg (\d+)", l): row["lat"] = m.group(1)
            if m := re.search(r"down\s+:.*-> ([\d.-]+) Mbit/s(.*)", l): row["down"] = m.group(1) + ("*" if "INCOMPLETE" in m.group(2) else "")
            if m := re.search(r"up\s+:.*-> ([\d.-]+) Mbit/s(.*)", l): row["up"] = m.group(1) + ("*" if "INCOMPLETE" in m.group(2) else "")
            if m := re.search(r"after\s+: RSSI (-?\d+) dBm, (.*)", l): row["after"] = m.group(2)
    else:
        m = re.search(r"last reason (\d+ \w+)", line or "")
        row["after"] = m.group(1) if m else "timeout"
    results.append(row)
    send("c"); read_until([r"disconnected"], 5)   # 'c' toggles: drop the link before the next level
    time.sleep(2)

send("p")
summary = [f"\n==== {label}: {ssid}, {time.strftime('%Y-%m-%d %H:%M')} ====",
           f"crystal: {xres.strip() if xres else 'no result'}",
           f"{'tx dBm':>6} {'join':>5} {'ms':>6} {'RSSI':>5} {'lat ms':>7} {'down Mb/s':>10} {'up Mb/s':>8}  after/reason"]
for r in results:
    summary.append(f"{r['tx']:>6} {r['join']:>5} {r['ms']:>6} {r['rssi']:>5} {r['lat']:>7} {r['down']:>10} {r['up']:>8}  {r['after']}")
summary.append("(* = transfer incomplete)")
for l in summary: out(l)
os.makedirs(os.path.join(os.path.dirname(__file__), "results"), exist_ok=True)
fn = os.path.join(os.path.dirname(__file__), "results", f"{label}-{time.strftime('%Y%m%d-%H%M')}.txt")
open(fn, "w").write("\n".join(log) + "\n"); print("saved", fn)
