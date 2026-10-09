"""Motion-language vocabulary. MUST match src/apps/motion/motion_tok.h exactly.

Token layout (108 tokens):
  0          BOS
  1          SEP   (end of observation: the next token is the label)
  2..7       STILL_<orient>                    orient = axis*2 + (negative)
  8..79      MOVE_<dir>_<mag>_<orient>         8 + (dir*2 + mag)*6 + orient
  80..85     JOLT_<axis>_<level>               80 + axis*2 + level
  86..92     touch: TAP DTAP LONG SW_U SW_D SW_L SW_R
  93..107    labels (LABELS below)
"""

AXES = "XYZ"
ORIENT_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]

BOS, SEP = 0, 1
STILL0 = 2
MOVE0 = 8
JOLT0 = 80
TOUCH0 = 86
TOUCH_NAMES = ["TAP", "DTAP", "LONG", "SW_U", "SW_D", "SW_L", "SW_R"]
TAP, DTAP, LONG, SW_U, SW_D, SW_L, SW_R = range(TOUCH0, TOUCH0 + 7)
LABEL0 = 93

# Gesture classes. Order is the on-device label id (0..14).
LABELS = [
    "none",
    "flick_out", "flick_in",
    "twist_l", "twist_r",
    "shake", "double_shake",
    "raise", "lower",
    "knock", "circle",
    "swipe_uu", "swipe_lrl", "swipe_dd", "tap_tap_r",
]
IMU_LABELS = LABELS[:11]
TOUCH_LABELS = ["none"] + LABELS[11:]
VOCAB = LABEL0 + len(LABELS)   # 108


def label_tok(name):
    return LABEL0 + LABELS.index(name)


def tok_name(t):
    if t == BOS:
        return "BOS"
    if t == SEP:
        return "SEP"
    if STILL0 <= t < MOVE0:
        return "S" + ORIENT_NAMES[t - STILL0]
    if MOVE0 <= t < JOLT0:
        k = t - MOVE0
        o = k % 6
        dm = k // 6
        d, mag = dm // 2, dm % 2
        return "M%s%s@%s" % (ORIENT_NAMES[d], "!" if mag else "", ORIENT_NAMES[o])
    if JOLT0 <= t < TOUCH0:
        k = t - JOLT0
        return "J%s%s" % (AXES[k // 2], "!" if k % 2 else "")
    if TOUCH0 <= t < LABEL0:
        return TOUCH_NAMES[t - TOUCH0]
    if LABEL0 <= t < VOCAB:
        return "<" + LABELS[t - LABEL0] + ">"
    return "?%d" % t
