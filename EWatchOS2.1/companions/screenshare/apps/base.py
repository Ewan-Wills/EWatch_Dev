"""What an app implements. Keep games to this and they run with any input
device and any orientation the streamer picks."""
from dataclasses import dataclass, field


@dataclass
class Input:
    """Input for one frame, in app pixels (0..w, 0..h)."""
    mouse: tuple = None          # (x, y) while the pointer is over the window, else None
    held: set = field(default_factory=set)       # mouse buttons down: 1 left, 2 middle, 3 right
    clicked: set = field(default_factory=set)    # buttons pressed this frame
    wheel: int = 0               # scroll steps this frame (+ up)
    keys: set = field(default_factory=set)       # keys down, pygame names: "a", "space", "left"
    pressed: set = field(default_factory=set)    # keys pressed this frame
    axes: dict = field(default_factory=dict)     # controller sticks/triggers, -1..1: {"lx": .., "ly": ..}
    pad: set = field(default_factory=set)        # controller buttons down: "a", "b", "start", ...
    # From the watch (through ewbridge; empty when run straight from screenshare.py):
    touch: tuple = None          # (x, y) while a finger is on the watch, else None
    tapped: bool = False         # finger went down this frame
    tilt: tuple = None           # (ax, ay, az) in g, watch axes; flat face-up ~ (0, 0, 1)
    wbutton: bool = False        # side button held
    wpressed: bool = False       # side button short press this frame
    gestures: list = field(default_factory=list)  # gesture names this frame: "swipe_left", ...


class App:
    """Base class: subclass it in apps/<name>.py.

    The runner calls update() then draw() once per frame at `fps`. Draw into
    self.surface (a pygame Surface, w x h) or return an h x w x 3 uint8 numpy
    array from draw(). w x h is the watch picture: 280x240 landscape or
    240x280 portrait (the streamer handles rotation).
    """
    title = "EWatch app"
    fps = 60
    touch_as_mouse = True        # watch touches also move/click inp.mouse (button 1)

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.surface = None          # set by the runner before the first draw()

    def haptic(self, ms=40, strength=200):
        """Buzz the watch (no-op when not connected through ewbridge)."""
        fn = getattr(self, "_haptic", None)
        if fn:
            fn(ms, strength)

    def update(self, dt, inp):
        """Advance by dt seconds. inp: Input."""

    def draw(self):
        """Draw this frame (into self.surface, or return an array)."""
