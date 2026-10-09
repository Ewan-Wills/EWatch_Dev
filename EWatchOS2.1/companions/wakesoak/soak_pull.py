#!/usr/bin/env python3
"""Pull the wake-soak log + wake gesture traces off the watch (USB).

    python3 companions/wakesoak/soak_pull.py            # save + summary
    python3 companions/wakesoak/soak_pull.py --clear    # ...then clear the log on the watch

Saves in companions/wakesoak/soak_logs/:
  wakesoak-STAMP.csv   one row per wake (answer = the label; timeout/no =
                       not intended), trace_id links to:
  wakegest-STAMP.csv   long format, one row per accel sample:
                       trace_id,src,answer,verdict,check_ms,phase,t_ms,ax,ay,az
                       phase pre (FIFO, before the trigger, t<0) | base (the
                       pose the raise check compared against) | chk (raise
                       check, 100 Hz) | post (~1 s after the screen came on)
  wakegest-STAMP.bin   the raw trace file (format: src/apps/wake_gest.h) The USB serial
can drop bytes, so the watch sends every line with an FNV-1a hash
(`soak get`) and lines that arrive damaged are asked for again. Nothing is
saved (exit 1) unless every line checks out.
"""
import csv, io, os, re, struct, sys, time
sys.dont_write_bytecode = True     # importing ewt must not litter the KB
from collections import defaultdict

KB = os.path.expanduser("~/Documents/Claude Knowledge Base/firmware/knowledge")
PENV = os.path.expanduser("~/.platformio/penv/bin/python")
try:
    import serial
except ImportError:                       # pyserial lives in PlatformIO's venv
    os.execv(PENV, [PENV] + sys.argv)
sys.path.insert(0, KB)
os.environ.setdefault("EWT_WHO", "soakpull")
import ewt                                # watch_lock, find_port

HERE = os.path.dirname(os.path.abspath(__file__))   # companions/wakesoak
SK_RE = re.compile(r"SK (\d+) ([0-9a-f]{8}) (.*)$")
END_RE = re.compile(r"SK end (\d+)")
GT_RE = re.compile(r"GT (\d+) ([0-9a-f]{8}) ([0-9a-f]*)$")
GEND_RE = re.compile(r"GT end (\d+) (\d+)")


def fnv(s):
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def ask(ser, cmd, secs=8.0, line_re=SK_RE, end_re=END_RE):
    """Send a console line, return (good {idx: text}, end match groups or None)."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    good, total, buf = {}, None, b""
    t = time.time() + secs
    while time.time() < t:
        buf += ser.read(4096)
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").rstrip("\r")
            m = line_re.search(line)
            if m and fnv(m.group(3)) == int(m.group(2), 16):
                good[int(m.group(1))] = m.group(3)
            e = end_re.search(line)
            if e:
                total = e.groups()
        if total is not None:
            break
    return good, total


def pull_lines(ser, cmd, line_re, end_re, what):
    """Whole-file pass, then re-ask damaged lines one by one."""
    lines, end = {}, None
    for _ in range(4):
        good, end = ask(ser, f"{cmd} 0 1000000", 60, line_re, end_re)
        lines.update(good)
        if end is not None:
            break
    if end is None:
        sys.exit(f"no reply to `{cmd}` (old firmware?)")
    total = int(end[0])
    for _ in range(8):
        missing = [i for i in range(total) if i not in lines]
        if not missing:
            break
        for i in missing:
            good, _ = ask(ser, f"{cmd} {i} 1", 3, line_re, end_re)
            lines.update(good)
    missing = [i for i in range(total) if i not in lines]
    if missing:
        sys.exit(f"{what}: {len(missing)} of {total} lines never arrived intact")
    return [lines[i] for i in range(total)], end


def pull():
    ewt.watch_lock("soak pull", 30)
    ser = serial.Serial(ewt.find_port(10), 115200, timeout=0.1)
    time.sleep(0.3)
    rows, _ = pull_lines(ser, "soak get", SK_RE, END_RE, "soak log")
    bad = [r for r in rows if r.count(",") not in (14, 15)]
    if bad:
        sys.exit(f"log has {len(bad)} malformed rows on the watch, e.g. {bad[0]!r}")
    hexl, end = pull_lines(ser, "soak gget", GT_RE, GEND_RE, "gesture traces")
    blob = bytes.fromhex("".join(hexl))
    if len(blob) != int(end[1]):
        sys.exit(f"gesture traces: got {len(blob)} bytes, watch has {end[1]}")
    ser.close()
    return rows, blob


SRC = {1: "motion", 2: "touch", 3: "button"}
WHY = {0: "ok", 1: "not_facing", 2: "not_steady", 3: "no_turn", 4: "no_imu", 255: "n/a"}
ANS = {0: "yes", 1: "no", 2: "timeout", 3: "rejected", 4: "auto_yes", 5: "superseded", 255: "none"}


def parse_traces(blob):
    """-> list of dicts (header + samples [(phase, t_ms, ax, ay, az)])."""
    out, off = [], 0
    while off + 28 <= len(blob):
        h = blob[off:off + 28]
        if h[:2] != b"WG" or h[2] != 1:
            sys.exit(f"trace file corrupt at byte {off}")
        tid, trig = struct.unpack_from("<II", h, 4)
        why, ok, chk_ms = h[12], h[13], struct.unpack_from("<H", h, 14)[0]
        base = struct.unpack_from("<hhh", h, 16)
        ans, pre_n, chk_n, post_n = h[22], h[24], struct.unpack_from("<H", h, 25)[0], h[27]
        off += 28
        s = []
        for i in range(pre_n):
            s.append(("pre", -(pre_n - i) * 20) + struct.unpack_from("<hhh", blob, off)); off += 6
        if any(base):
            s.append(("base", 0) + tuple(base))
        for i in range(chk_n):
            s.append(("chk", i * 10) + struct.unpack_from("<hhh", blob, off)); off += 6
        for i in range(post_n):
            t, x, y, z = struct.unpack_from("<Hhhh", blob, off); off += 8
            s.append(("post", t, x, y, z))
        out.append(dict(id=tid, src=SRC.get(h[3], h[3]), trig_ms=trig, verdict=WHY.get(why, why),
                        check_ok=ok, check_ms=chk_ms, answer=ANS.get(ans, ans), samples=s))
    if off != len(blob):
        sys.exit(f"trace file: {len(blob) - off} trailing bytes")
    return out


def summarise(rows):
    recs = list(csv.DictReader(io.StringIO("\n".join(rows))))
    by = defaultdict(list)
    for r in recs:
        by[r["src"]].append(r)
    print(f"{len(recs)} wakes logged")
    print(f"{'source':22} {'n':>4} {'yes':>4} {'no':>4} {'t/o':>4} {'rej':>4}  held_ms(median)")
    for src, rs in sorted(by.items()):
        cnt = defaultdict(int)
        for r in rs:
            cnt[r["answer"]] += 1
        held = sorted(int(r["held_ms"]) for r in rs if int(r["held_ms"]) > 0)
        med = held[len(held) // 2] if held else "-"
        print(f"{src:22} {len(rs):4} {cnt['yes'] + cnt['auto_yes']:4} {cnt['no']:4} "
              f"{cnt['timeout']:4} {cnt['rejected']:4}  {med}")
    print("t/o = no answer within 5 s, counted as NOT intentional")
    lab = [r for r in recs if r["src"] == "sleep_motion" and r["answer"] in ("yes", "no", "timeout")]
    if lab:   # label mode: how the raise check's verdict lines up with the answers
        WHYS = {"0": "ok", "1": "not_facing", "2": "not_steady", "3": "no_turn", "4": "no_imu"}
        print("motion: check verdict vs your answer (yes / not intended)")
        for v in sorted({r["gesture"] for r in lab}):
            g = [r for r in lab if r["gesture"] == v]
            y = sum(r["answer"] == "yes" for r in g)
            print(f"  {WHYS.get(v, v):11} {y:4} / {len(g) - y:4}")


def main():
    rows, blob = pull()
    if len(rows) <= 1:
        print("soak log is empty")
        return
    os.makedirs(os.path.join(HERE, "soak_logs"), exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(HERE, "soak_logs", f"wakesoak-{stamp}.csv")
    with open(path, "w") as f:
        f.write("\n".join(rows) + "\n")
    print(f"saved {path} ({len(rows) - 1} rows, every line hash-checked)")
    summarise(rows)
    traces = parse_traces(blob)
    if traces:
        with open(os.path.join(HERE, "soak_logs", f"wakegest-{stamp}.bin"), "wb") as f:
            f.write(blob)
        gpath = os.path.join(HERE, "soak_logs", f"wakegest-{stamp}.csv")
        with open(gpath, "w") as f:
            f.write("trace_id,src,answer,verdict,check_ms,phase,t_ms,ax,ay,az\n")
            for tr in traces:
                for ph, t, x, y, z in tr["samples"]:
                    f.write(f"{tr['id']},{tr['src']},{tr['answer']},{tr['verdict']},"
                            f"{tr['check_ms']},{ph},{t},{x},{y},{z}\n")
        n = sum(len(t["samples"]) for t in traces)
        print(f"saved {gpath} ({len(traces)} traces, {n} samples, {len(blob)} bytes)")
        ids = {r.split(",")[15] for r in rows[1:] if r.count(",") == 15}
        orphan = [t["id"] for t in traces if str(t["id"]) not in ids]
        if orphan:
            print(f"  note: {len(orphan)} traces have no soak row (e.g. id {orphan[0]})")
    if "--clear" in sys.argv:
        ser = serial.Serial(ewt.find_port(10), 115200, timeout=0.1)
        ser.write(b"soak clear\n"); time.sleep(0.5); ser.close()
        print("cleared the log and traces on the watch")


if __name__ == "__main__":
    main()
