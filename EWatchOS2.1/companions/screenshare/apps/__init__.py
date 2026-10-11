"""Mac-side apps streamed to the watch.

An app runs on the MacBook in its own window (that's where the mouse,
keyboard and controller go) and every frame it draws is sent straight to the
watch: no screen capture, exact pixels, lowest latency.

    python3 screenshare.py --app swarm --landscape
    python3 screenshare.py --app list

To add one, drop a file in this folder with a subclass of apps.App (see
base.py and swarm.py). The file name is the --app name.
"""
import importlib
import inspect
import os
import pkgutil

from .base import App, Input
from .runner import Runner

__all__ = ["App", "Input", "Runner", "load", "available"]


def load(name):
    """The App subclass defined in apps/<name>.py."""
    try:
        mod = importlib.import_module(f"{__name__}.{name}")
    except ModuleNotFoundError as e:
        if e.name == f"{__name__}.{name}":
            names = ", ".join(n for n, _ in available()) or "none"
            raise SystemExit(f"No app '{name}' in apps/ (have: {names})")
        raise
    found = [c for _, c in inspect.getmembers(mod, inspect.isclass)
             if issubclass(c, App) and c is not App and c.__module__ == mod.__name__]
    if len(found) != 1:
        raise SystemExit(f"apps/{name}.py must define exactly one App subclass (found {len(found)})")
    return found[0]


def available():
    """[(name, first line of its docstring)] for every app file."""
    out = []
    here = os.path.dirname(__file__)
    for m in pkgutil.iter_modules([here]):
        if m.name in ("base", "runner") or m.name.startswith("_"):
            continue
        try:
            cls = load(m.name)
            doc = (inspect.getdoc(cls) or "").splitlines()[0:1]
            out.append((m.name, doc[0] if doc else ""))
        except SystemExit as e:
            out.append((m.name, f"(broken: {e})"))
    return out
