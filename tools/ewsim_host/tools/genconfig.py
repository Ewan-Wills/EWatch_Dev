#!/usr/bin/env python3
"""Generate build/<proj>/config.mk + screens_gen.cpp for one EWatchOS2.1-family project.

Reads the project's platformio.ini ([env:<env>] build_flags -D/-I and
build_src_filter), lists every src/**/*.c[pp], swaps the radio modules for the
host stubs in host/stubs/, symlinks the project's own PlatformIO libraries
(.pio/libdeps/<env>) into the build dir (Make can't cope with the spaces in
"GFX Library for Arduino"), and preprocesses model.h with the same defines to
get the Screen enum exactly as the firmware compiles it.
"""
import configparser, glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_ROOT = os.path.dirname(os.path.dirname(HERE))   # repo root (ewsim_host lives in tools/)

# Firmware sources replaced by host stubs (radio / BLE stacks the host can't run).
STUBBED = ["core/wifi_svc.cpp", "core/cloud_net.cpp", "apps/spotify/spotify.cpp", "apps/modules/spotify/spotify.cpp", "apps/bluetooth.cpp",
           "apps/xbox_pad.cpp", "apps/btmouse.cpp", "core/ble_stack.cpp"]
RADIO_RE = re.compile(r'#include\s*<((?:WiFi|WiFiClient|WiFiClientSecure|HTTPClient|WebServer|AsyncTCP|ESPAsyncWebServer|BLE\w*|NimBLE\w*|esp_bt\w*|esp_gap\w*|esp_gatt\w*)\.h)>')
LIBS = {  # build-dir name -> (libdeps folder, source globs relative to it, include dir)
    "gfx": ("GFX Library for Arduino",
            ["src/Arduino_GFX.cpp", "src/Arduino_G.cpp", "src/Arduino_DataBus.cpp", "src/Arduino_TFT.cpp",
             "src/canvas/Arduino_Canvas.cpp", "src/canvas/Arduino_Canvas_Indexed.cpp",
             "src/canvas/Arduino_Canvas_3bit.cpp", "src/canvas/Arduino_Canvas_Mono.cpp",
             "src/display/Arduino_ST7789.cpp"], "src"),
    "cst816s": ("CST816S", ["CST816S.cpp"], "."),
    "qrcode": ("QRCode", ["src/qrcode.c"], "src"),
    "jpegdec": ("JPEGDEC", ["src/JPEGDEC.cpp"], "src"),
    "arduinojson": ("ArduinoJson", [], "src"),
}


def resolve_proj(p):
    for cand in (p, os.path.join(FW_ROOT, p)):
        if os.path.isfile(os.path.join(cand, "platformio.ini")):
            return os.path.abspath(cand)
    sys.exit(f"ewsim: no platformio.ini for project {p!r} (looked in {FW_ROOT})")


def ini_env(proj, env):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(os.path.join(proj, "platformio.ini"))
    sec = f"env:{env}"
    if not cp.has_section(sec):
        sys.exit(f"ewsim: [{sec}] not in {proj}/platformio.ini")

    def get(section, key, depth=0):
        if not cp.has_option(section, key):
            ext = cp.get(section, "extends", fallback=None)
            return get(ext.strip() if ext.startswith("env:") else "env:" + ext.strip(), key, depth + 1) if ext and depth < 5 else ""
        v = cp.get(section, key)
        return re.sub(r"\$\{(env:[^.}]+)\.([^}]+)\}", lambda m: get(m.group(1), m.group(2), depth + 1), v)

    flags = []
    for line in get(sec, "build_flags").splitlines():
        line = line.split(";", 1)[0].strip()
        if line:
            flags += line.split()
    unflags = get(sec, "build_unflags").split()
    flags = [f for f in flags if f not in unflags]
    # later -DX=... wins
    defs, incs = {}, []
    for f in flags:
        if f.startswith("-D"):
            k, _, v = f[2:].partition("=")
            defs[k] = v
        elif f.startswith("-I"):
            incs.append(os.path.normpath(os.path.join(proj, f[2:])))
    excl = re.findall(r"-<([^>]+)>", get(sec, "build_src_filter"))
    # optional app modules (EWatchOS tools/ew_apps.py): custom_apps + EW_APPS
    mods = os.path.join(proj, "src", "apps", "modules")
    have = sorted(os.listdir(mods)) if os.path.isdir(mods) else []
    apps = (get(sec, "custom_apps") + " " + os.environ.get("EW_APPS", "")).replace(",", " ").split()
    apps = have if "all" in apps else apps
    for a in dict.fromkeys(x.lower() for x in apps):
        if a not in have:
            sys.exit(f"ewsim: unknown module {a!r} (have: {' '.join(have)})")
        defs[f"EW_APP_{a.upper()}"] = "1"
        incs.append(os.path.join(mods, a))
        excl = [e for e in excl if e.rstrip("/") != "apps/modules"] + \
               [f"apps/modules/{m}" for m in have if m not in apps]
    mods = sorted(dict.fromkeys(x.lower() for x in apps))
    return defs, incs, excl, mods


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: genconfig.py PROJ [ENV]")
    proj = resolve_proj(sys.argv[1])
    env = sys.argv[2] if len(sys.argv) > 2 else "ewatch"
    name = os.path.basename(proj)
    defs, incs, excl, mods = ini_env(proj, env)
    # one build dir per module set: the OS hooks are compiled with -DEW_APP_*,
    # so objects built without a module must never be reused with it
    if mods:
        name += "+" + "+".join(mods)
    B = os.path.join(HERE, "build", name)
    os.makedirs(os.path.join(B, "obj"), exist_ok=True)
    os.makedirs(os.path.join(B, "libs"), exist_ok=True)
    src = os.path.join(proj, "src")
    if not os.path.isfile(os.path.join(src, "core", "ewsim.cpp")):
        sys.exit(f"ewsim: {name} has no src/core/ewsim.cpp (not an EWatchOS2.1-family app with ewsim). "
                 f"Add it with ewsim_install.py from the Claude Knowledge Base, or record on the watch.")

    # libraries: the project's own resolved PlatformIO deps, else EWatchOS2.1's
    libdirs = {}
    for key, (folder, srcs, inc) in LIBS.items():
        cands = [os.path.join(proj, ".pio", "libdeps", env, folder)] + \
                sorted(glob.glob(os.path.join(proj, ".pio", "libdeps", "*", folder))) + \
                [os.path.join(FW_ROOT, "EWatchOS2.1", ".pio", "libdeps", "ewatch", folder)]
        found = next((c for c in cands if os.path.isdir(c)), None)
        if not found:
            sys.exit(f"ewsim: library {folder!r} not found for {name}: run one `pio run` (or `ewt build`) "
                     f"for the project first so PlatformIO downloads its libraries")
        link = os.path.join(B, "libs", key)
        if os.path.islink(link) and os.readlink(link) != found:
            os.unlink(link)
        if not os.path.islink(link):
            os.symlink(found, link)
        libdirs[key] = link

    # firmware sources
    srcs = []
    for path in sorted(glob.glob(os.path.join(src, "**", "*.c*"), recursive=True)):
        if not path.endswith((".cpp", ".c")):
            continue
        rel = os.path.relpath(path, src)
        if any(rel.startswith(e.rstrip("/") + "/") or rel == e for e in excl):
            continue
        if rel in STUBBED:
            stub = os.path.join(HERE, "host", "stubs", os.path.basename(rel))
            if not os.path.isfile(stub):
                sys.exit(f"ewsim: no host stub for {rel}")
            srcs.append(stub)
            continue
        if " " in path:
            sys.exit(f"ewsim: space in source path not supported: {path}")
        radio = RADIO_RE.search(open(path, errors="replace").read())
        if radio:
            sys.exit(f"ewsim: {name}/src/{rel} uses {radio.group(1)} directly: the WiFi/BLE radio is not "
                     f"simulated, so this app can't run on the host. Record it on the watch (ewt record), "
                     f"or add a host stub for {rel} under ewsim_host/host/stubs/.")
        srcs.append(path)
    host = sorted(glob.glob(os.path.join(HERE, "host", "src", "*.cpp")))
    vendor = sorted(glob.glob(os.path.join(HERE, "vendor", "arduino-core", "*.c*")))
    libsrcs = [os.path.join(libdirs[k], s) for k, (_, ss, _) in LIBS.items() for s in ss]

    include = [os.path.join(HERE, "host", "include"), os.path.join(HERE, "vendor", "arduino-core"),
               os.path.join(HERE, "host", "stubs")] + incs + [src, os.path.join(proj, "include")] + \
              [os.path.normpath(os.path.join(libdirs[k], inc)) if inc != "." else libdirs[k]
               for k, (_, _, inc) in LIBS.items()]
    for i in include:
        if " " in i:
            sys.exit(f"ewsim: space in include path not supported: {i}")
    dflags = ["-DEWSIM_HOST=1", "-DARDUINO=10819"] + \
             [f"-D{k}" + (f"={v}" if v else "") for k, v in defs.items()]
    cpp = " ".join(dflags + ["-I" + i for i in include])

    # Screen enum, preprocessed with the firmware's defines (honours #if gates exactly)
    model = os.path.join(src, "core", "model.h")
    pre = subprocess.run(["xcrun", "clang++", "-std=gnu++17", "-E", "-P", "-w", "-x", "c++"] + cpp.split() + [model],
                         capture_output=True, text=True)
    if pre.returncode != 0:
        sys.exit("ewsim: preprocessing model.h failed:\n" + pre.stderr[-2000:])
    m = re.search(r"enum class Screen[^{]*\{(.*?)\};", pre.stdout, re.S)
    if not m:
        sys.exit("ewsim: no `enum class Screen` in model.h")
    names = [t.strip().split("=")[0].strip() for t in m.group(1).split(",") if t.strip()]
    gen = os.path.join(B, "screens_gen.cpp")
    body = ("// generated by tools/genconfig.py from " + model + "\n"
            "extern const char *const kEwsimScreens[] = {" + ", ".join(f'"{n}"' for n in names) + "};\n"
            f"extern const int kEwsimScreenCount = {len(names)};\n"
            f'extern const char *const kEwsimProject = "{name}";\n')
    if not os.path.exists(gen) or open(gen).read() != body:
        open(gen, "w").write(body)

    lines = [f"# generated by tools/genconfig.py for {proj} [env:{env}]",
             f"PROJ_DIR := {proj}", f"CPPFLAGS := {cpp}", "OBJS :="]
    for s in srcs + host + vendor + libsrcs + [gen]:
        if s.startswith(src):
            tag = "fw_" + os.path.relpath(s, src)
        elif s.startswith(HERE):
            tag = os.path.relpath(s, HERE)
        else:
            tag = s
        tag = re.sub(r"[^A-Za-z0-9_.]", "_", tag.replace(os.sep, "__"))
        obj = os.path.join(B, "obj", tag + ".o")
        comp = "$(CC) $(CFLAGS)" if s.endswith(".c") else "$(CXX) $(CXXFLAGS)"
        lines += [f"OBJS += {obj}", f"{obj}: {s}", f"\t@echo '  CC {os.path.relpath(s, FW_ROOT) if s.startswith(FW_ROOT) else s}'",
                  f"\t@{comp} $(CPPFLAGS) -MMD -MP -c $< -o $@"]
    cfg = os.path.join(B, "config.mk")
    text = "\n".join(lines) + "\n"
    if not os.path.exists(cfg) or open(cfg).read() != text:
        open(cfg, "w").write(text)
    print(B)


if __name__ == "__main__":
    main()
