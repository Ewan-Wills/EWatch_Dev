#!/usr/bin/env python3
"""Emit Claude Face test frames packed with struct.pack straight from
ClaudeFace/protocol/protocol.json (independent of the generated C header and
of the TS encoder), plus decode/verify watch->host lines.

  mkframes.py vectors  <out.txt>     write parse vectors for test_main
  mkframes.py checkout <lines.txt>   verify #CFE / #CFH lines the C++ side encoded

Vector line format (tab separated):
  <EXPECT>\t<TYPE or ->\t<k=v,k=v...>\t<line>
EXPECT in OK | BAD | VERSION | QUERY | NOTCF. Lines are written with a
literal "\\r" / "\\n" escape where the test needs trailing CR/LF.
"""
import base64, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "..", "..", "..", "..", "ClaudeFace", "protocol", "protocol.json")
P = json.load(open(PROTO))
MSG = P["messages"]
FMT = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i"}


def pack(name, values):
    out = b""
    for fname, ftype, *_ in MSG[name]["fields"]:
        v = values.get(fname, 0)
        if ftype in FMT:
            out += struct.pack("<" + FMT[ftype], v)
        elif ftype.startswith("char["):
            n = int(ftype[5:-1])
            b = (v or "").encode("ascii")[: n - 1]
            out += b + b"\0" * (n - len(b))
        else:                                   # SessionRow[4]
            sub, n = ftype[:-1].split("[")
            rows = list(v or [])
            rows += [{}] * (int(n) - len(rows))
            out += b"".join(pack(sub, r) for r in rows)
    return out


def unpack(name, data):
    vals, off = {}, 0
    for fname, ftype, *_ in MSG[name]["fields"]:
        if ftype in FMT:
            sz = struct.calcsize("<" + FMT[ftype])
            vals[fname] = struct.unpack_from("<" + FMT[ftype], data, off)[0]
        elif ftype.startswith("char["):
            sz = int(ftype[5:-1])
            vals[fname] = data[off:off + sz].split(b"\0")[0].decode("ascii")
        else:
            raise ValueError(ftype)
        off += sz
    return vals, off


def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def frame(t, payload, crc=None):
    c = crc16(payload) if crc is None else crc
    return "#CF%s %s" % (t, base64.b64encode(payload + struct.pack("<H", c)).decode())


NOW = 1790000000
STATE = dict(version=P["version"], seq=4242, flags=0x08, global_state=2, five_used=46,
             five_resets_at=NOW + 8040, seven_used=31, seven_resets_at=NOW + 273600,
             five_pace=-9, seven_pace=-21, projected_hit=0, rl_age_s=40, five_elapsed=55,
             seven_elapsed=52, session_count=2, blocked_count=0, subagent_count=3,
             primary_index=0, p_state=2, p_ctx=41, p_model=1, p_effort=3, p_flags=0x06,
             p_turn_elapsed_s=187, p_last_turn_s=95, p_tool_count=14,
             p_current_tool="Bash|npm test", p_project="EWatch_Firmware",
             p_model_name="Opus 4.5", p_cache_expires_at=NOW + 280, p_lines_added=214,
             p_lines_removed=37, p_cost_cents=342, cost_today_cents=1288, p_pr_number=57,
             p_pr_review=2, bridge_now=NOW)
SESS = dict(version=P["version"], seq=77, count=2,
            rows=[dict(state=2, ctx=41, project="EWatch_Firm", turn_elapsed_s=187),
                  dict(state=4, ctx=12, project="bridge", turn_elapsed_s=0)])
TIME = dict(version=P["version"], epoch=NOW, utc_offset_min=-300)


def vectors(path):
    st, ss, tm = pack("State", STATE), pack("Sessions", SESS), pack("Time", TIME)
    assert len(st) == 119 and len(ss) == 76 and len(tm) == 7, (len(st), len(ss), len(tm))
    v = []
    chk = "seq=4242,five_resets_at=%d,five_pace=-9,p_turn_elapsed_s=187,p_project=EWatch_Firmware,p_current_tool=Bash|npm test,bridge_now=%d" % (NOW + 8040, NOW)
    v.append(("OK", "S", chk, frame("S", st)))
    v.append(("OK", "S", chk, frame("S", st) + "\\r\\n"))           # CRLF tolerated
    v.append(("OK", "L", "seq=77,count=2,row1_state=4,row0_project=EWatch_Firm", frame("L", ss)))
    v.append(("OK", "T", "epoch=%d,utc_offset_min=-300" % NOW, frame("T", tm)))
    bad_crc = crc16(st) ^ 0x0001
    v.append(("BAD", "-", "", frame("S", st, crc=bad_crc)))            # CRC off by one bit
    tampered = bytearray(st); tampered[5] ^= 0x40                       # payload flipped after CRC
    v.append(("BAD", "-", "", frame("S", bytes(tampered), crc=crc16(st))))
    v.append(("BAD", "-", "", frame("S", st[:-1])))                     # short payload, valid CRC
    v.append(("BAD", "-", "", frame("S", st + b"\0")))                  # long payload, valid CRC
    v.append(("BAD", "-", "", frame("L", st)))                          # STATE body under L
    v.append(("BAD", "-", "", frame("T", tm[:6])))
    v2 = bytearray(st); v2[0] = P["version"] + 1
    v.append(("VERSION", "S", "", frame("S", bytes(v2))))               # future version, bigger
    v.append(("VERSION", "S", "", frame("S", bytes(v2) + b"extra")))
    v.append(("BAD", "-", "", "#CFS AAA="))
    v.append(("BAD", "-", "", "#CFS "))
    v.append(("BAD", "-", "", "#CFS"))
    v.append(("BAD", "-", "", "#CFSx" + frame("S", st)[5:]))            # missing space
    v.append(("BAD", "-", "", frame("S", st)[:-4]))                     # truncated base64
    good = frame("S", st)
    v.append(("BAD", "-", "", good[:20] + "!" + good[21:]))             # invalid b64 char
    v.append(("BAD", "-", "", good[:20] + "=" + good[21:]))             # padding mid-string
    v.append(("BAD", "-", "", "#CFX " + good[5:]))                      # unknown type
    v.append(("BAD", "-", "", frame("E", pack("Event", dict(version=1, code=1)))))  # watch->host type
    v.append(("QUERY", "-", "", "#CFQ"))
    v.append(("QUERY", "-", "", "#CFQ\\r"))
    v.append(("BAD", "-", "", "#CFQQ"))
    v.append(("NOTCF", "-", "", "help"))
    v.append(("NOTCF", "-", "", "log dump"))
    v.append(("NOTCF", "-", "", "#C"))
    v.append(("NOTCF", "-", "", "#cfS abc"))
    with open(path, "w") as f:
        for e in v:
            f.write("\t".join(e) + "\n")
    print("mkframes: wrote %d vectors" % len(v))


def checkout(path):
    ok = bad = 0
    for line in open(path):
        line = line.rstrip("\n")
        if not line:
            continue
        expect, _, fr = line.partition("\t")
        t, b64 = fr[3], fr[5:]
        raw = base64.b64decode(b64, validate=True)
        body, crc = raw[:-2], struct.unpack("<H", raw[-2:])[0]
        name = {"E": "Event", "H": "Hello"}[t]
        vals, size = unpack(name, body)
        got = ",".join("%s=%s" % (k, vals[k]) for k in sorted(vals))
        good = crc == crc16(body) and size == len(body) and fr.startswith("#CF" + t + " ") and got == expect
        if good:
            ok += 1
        else:
            bad += 1
            print("CHECKOUT FAIL: expect %s got %s crc_ok=%s size=%d/%d" % (expect, got, crc == crc16(body), size, len(body)))
    print("checkout: %d ok, %d bad" % (ok, bad))
    return 0 if bad == 0 and ok > 0 else 1


if __name__ == "__main__":
    if sys.argv[1] == "vectors":
        vectors(sys.argv[2])
    else:
        sys.exit(checkout(sys.argv[2]))
