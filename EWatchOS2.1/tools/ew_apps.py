# PlatformIO pre-script: compile the optional app modules a build asks for.
#
# Modules live in src/apps/modules/<name>/ and are excluded by default
# (build_src_filter). Pick them per env or per command:
#
#   [env:ewatch_games]                       EW_APPS="doom racer" pio run
#   extends = env:ewatch                     EW_APPS=all ewt flash
#   custom_apps = doom racer pet
#
# For each module this adds -DEW_APP_<NAME>=1 (the OS hooks are guarded by it),
# its folder to the include path, and its sources to the build. Both lists are
# merged; "all" = every module folder. An unknown name fails the build.
Import("env")  # noqa: F821  (PlatformIO SCons)
import os

proj = env.subst("$PROJECT_DIR")
mods = os.path.join(proj, "src", "apps", "modules")
have = sorted(d for d in os.listdir(mods) if os.path.isdir(os.path.join(mods, d))) if os.path.isdir(mods) else []

want = (env.GetProjectOption("custom_apps", "") + " " + os.environ.get("EW_APPS", "")).replace(",", " ").split()
if "all" in want:
    want = have
apps = []
for a in want:
    a = a.lower()
    if a in apps:
        continue
    if a not in have:
        print(f"ew_apps: unknown module '{a}' (have: {' '.join(have)})")
        env.Exit(1)
    apps.append(a)
    env.Append(CPPDEFINES=[(f"EW_APP_{a.upper()}", 1)],
               CPPPATH=[os.path.join(mods, a)],
               SRC_FILTER=[f"+<apps/modules/{a}/>"])

print("ew_apps: modules =", " ".join(apps) if apps else "(none)")
