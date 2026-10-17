#!/usr/bin/env python3
"""Hopper asset + level builder.

Writes src/apps/modules/hopper/hopper_data.h (RGB565 tiles per theme, sprites,
levels) and, with --preview, a PNG sheet of every tile and sprite so the art can
be checked without a watch. --check runs the reachability checker only.

All art and levels here are original (the game plays like a classic side-scrolling
platformer; no third-party sprites, names or level layouts).

    python3 companions/hopper/build_assets.py            # header + reachability check
    python3 companions/hopper/build_assets.py --preview out.png
"""
import os, sys, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "src", "apps", "modules", "hopper", "hopper_data.h")

# ---------------------------------------------------------------- palette
PAL = {
    'k': (24, 20, 28),    'w': (255, 255, 255), 'W': (200, 200, 212),
    'g': (96, 200, 72),   'G': (36, 120, 52),   'l': (200, 236, 140),
    'r': (226, 52, 44),   'R': (140, 24, 28),   'o': (246, 146, 48),
    'y': (252, 214, 64),  'Y': (196, 140, 24),  'b': (176, 106, 52),
    'B': (104, 58, 28),   'n': (232, 188, 128), 's': (156, 156, 170),
    'S': (84, 84, 100),   'p': (160, 92, 210),  'P': (92, 48, 140),
    'c': (96, 180, 246),  'C': (36, 84, 176),   'm': (250, 150, 190),
    'e': (255, 236, 170), 'd': (60, 36, 20),    'v': (52, 52, 64),
}
KEY = 0xF81F  # transparent


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def grid(rows, w=16, h=None, name="?"):
    """Normalise char art: pad/trim each row to w, check row count."""
    out = []
    for i, r in enumerate(rows):
        if len(r) != w:
            print(f"  art warn: {name} row {i} len {len(r)} != {w}", file=sys.stderr)
            r = (r + '.' * w)[:w]
        out.append(r)
    if h is not None and len(out) != h:
        print(f"  art warn: {name} has {len(out)} rows != {h}", file=sys.stderr)
        out = (['.' * w] * (h - len(out)) + out)[-h:]
    return out


def remap(rows, m):
    return [''.join(m.get(ch, ch) for ch in r) for r in rows]


def scale2(rows):
    out = []
    for r in rows:
        rr = ''.join(ch * 2 for ch in r)
        out += [rr, rr]
    return out


def to565(rows, opaque_bg=None):
    px = []
    for r in rows:
        for ch in r:
            if ch == '.':
                px.append(KEY if opaque_bg is None else opaque_bg)
            else:
                px.append(rgb565(PAL[ch]))
    return px

# ---------------------------------------------------------------- sprites
FROG = grid([
    "................",
    "....kkkk.kkkk...",
    "...kwwwwkwwwwk..",
    "...kwwkkkwwkkk..",
    "..kgkwwkgkwwkgk.",
    ".kgggkkgggkkgggk",
    ".kggggggggggggk.",
    "kggggggggggggggk",
    "kgggggggggkkkkkk",
    "kggggggglllllllk",
    "kgggggglllllllk.",
    ".kGgggglllllllk.",
    ".kGGgggglllllk..",
    "..kGGkkkkkkGGk..",
    ".kGGGk....kGGGk.",
    ".kkkkk....kkkkk.",
], name="frog")
FROG_W1 = FROG[:14] + grid(["..kGGk....kGGGk.", "..kkkk....kkkkkk"], name="w1")
FROG_W2 = FROG[:14] + grid([".kGGGk.....kGGk.", "kkkkk......kkkk."], name="w2")
FROG_JUMP = FROG[:13] + grid(["kGGk.kkkkk.kGGk.", "kGk.........kGk.", "kk...........kk."], name="jump")


def tall(s):
    # Big form: same face, longer body (16x24).
    return s[0:7] + [s[7]] * 5 + [s[8]] + [s[9]] * 2 + [s[10]] * 3 + [s[11]] * 2 + s[12:16]


FIRE = {'g': 'o', 'G': 'R', 'l': 'y'}

LADYBUG = grid([
    "................",
    "................",
    "................",
    "......kkkkkk....",
    ".....krrkrrrrk..",
    "....krrkkkrrrrk.",
    "..kkkrrrkrrkkrrk",
    ".kwkkrrrrrrkkrrk",
    ".kwkkkrrkkrrrrrk",
    "kkkkkrrrkkrrrrrk",
    "kkkkkrrrrrrrkrrk",
    ".kkkkkrrrrrrrrk.",
    "..kkkkkkkkkkkkk.",
    "...k.k...k..k...",
    "..k...k.k....k..",
    "................",
], name="ladybug")
LADYBUG2 = LADYBUG[:13] + grid(["....k.k...k.k...", "...k...k...k...k", "................"], name="lb2")
LADYBUG_FLAT = grid(["................"] * 11 + [
    "....kkkkkkkk....",
    "..kkrrkrrrrkrk..",
    ".kwkkrrrkkrrrrk.",
    ".kkkkkkkkkkkkkk.",
    "................",
], name="lbflat")

SNAIL = grid([
    "................",
    "................",
    ".k..k...........",
    "kwkkwk..kkkkk...",
    ".k..k..kyyyyyk..",
    ".k..k.kyYYYYyyk.",
    ".k..k.kyYkkkYyk.",
    ".kkkk.kyYkyYYyk.",
    "kmmmmkkyYkkYYyk.",
    "kmkmmmkyYYYYyk..",
    "kmmmmmmkkyyyk...",
    "kmmmmmmmkkkkk...",
    ".kmmmmmmmmmmmk..",
    "..kmmmmmmmmmmmk.",
    "...kkkkkkkkkkkk.",
    "................",
], name="snail")
SNAIL2 = SNAIL[:12] + grid([
    ".kmmmmmmmmmmmmk.",
    "..kmmmmmmmmmmmk.",
    "...kkkkkkkkkkk..",
    "................",
], name="snail2")
SHELL = grid(["................"] * 5 + [
    ".....kkkkkk.....",
    "....kyyyyyyk....",
    "...kyYYYYYYyk...",
    "..kyYkkkkkYyyk..",
    "..kyYkyyykYYyk..",
    "..kyYkykkkYYyk..",
    "..kyYkkYYYYyk...",
    "...kyYYYYyyk....",
    "....kkkkkkk.....",
    "...kkkkkkkkk....",
    "................",
], name="shell")
SHELL2 = remap(SHELL, {'Y': 'y', 'y': 'Y'})

SPIKY = grid([
    "................",
    "................",
    ".....w...w......",
    "...w.kw.kw.w....",
    "....kWkkWkkW.w..",
    "..wkpppppppkWk..",
    "...kppPpPpppkk..",
    ".wkpppppppppppk.",
    "..kwkpPpPpppPpk.",
    ".kwkkpppppppppk.",
    ".kkkppppPpPppPk.",
    "..kppppppppppk..",
    "...kkkkkkkkkk...",
    "...kk.....kk....",
    "..kkk....kkk....",
    "................",
], name="spiky")
SPIKY2 = SPIKY[:13] + grid(["....kk.....kk...", "....kkk....kkk..", "................"], name="spiky2")

STAG = grid([
    "................",
    ".kk.........kk..",
    "kbk.........kbk.",
    "kbBk.......kBbk.",
    ".kbBk.....kBbk..",
    "..kbBkkkkkBbk...",
    "...kkPPPPPkk....",
    "..kPPwkPwkPPk...",
    "..kPPkkPkkPPk...",
    "..kkPPPPPPPkk...",
    ".kpppkkkkkpppk..",
    "kpppPppppPppppk.",
    "kppPppppppPpppk.",
    "kpppppppppppppk.",
    ".kkkkkkkkkkkkk..",
    "..k..k...k..k...",
], name="stag")
STAG2 = STAG[:15] + grid([".k..k....k..k...."[:16]], name="stag2")

BERRY = grid([
    "................",
    "......kk.kk.....",
    ".....kGGkGGk....",
    "......kGGGk.....",
    "....kkkkkkkkk...",
    "...krmrrmrrmrk..",
    "..krrrrrrrrrrrk.",
    "..krmrrmrrmrrmk.",
    "..krrrrrrrrrrrk.",
    "..kRrmrrmrrmrRk.",
    "...kRrrrrrrrRk..",
    "....kRRmRRRRk...",
    ".....kRRRRRk....",
    "......kkkkk.....",
    "................",
    "................",
], name="berry")
BERRY_1UP = remap(BERRY, {'r': 'c', 'R': 'C', 'm': 'w', 'G': 'G'})

CHILI = grid([
    "................",
    ".........kk.....",
    "........kGk.....",
    ".......kGGGk....",
    "......kkGGGkk...",
    ".....krrkkkrrk..",
    "....krrwrrrrrk..",
    "....krwrrrrrrk..",
    "...krrrrrrrrk...",
    "...krrrrrrrRk...",
    "..krrrrrrrRk....",
    "..krrrrrRRk.....",
    ".krrrrRRkk......",
    ".kRRRRkk........",
    "kRkkkk..........",
    "kk..............",
], name="chili")

GEM = grid([
    "................",
    "....kkkkkkkk....",
    "...kwcwcccccck..",
    "..kwcccwccccCck.",
    ".kccccccccccCCck",
    "kkkkkkkkkkkkkkkk",
    ".kcwccccccccCck.",
    "..kcwcccccccCk..",
    "...kcwccccCCk...",
    "....kccccCCk....",
    ".....kccCCk.....",
    "......kcCk......",
    ".......kk.......",
    "................",
    "................",
    "................",
], name="gem")

COIN1 = grid([
    "................",
    "................",
    ".....kkkkk......",
    "....kyyyyyk.....",
    "...kyywyyyYk....",
    "...kywyyyyYk....",
    "...kywyYYyYk....",
    "...kywyYYyYk....",
    "...kywyYYyYk....",
    "...kywyYYyYk....",
    "...kyyyyyyYk....",
    "...kyyyyyYYk....",
    "....kYYYYYk.....",
    ".....kkkkk......",
    "................",
    "................",
], name="coin1")
COIN2 = grid([
    "................",
    "................",
    "......kkk.......",
    ".....kyyYk......",
    ".....kwyYk......",
    ".....kwyYk......",
    ".....kwyYk......",
    ".....kwyYk......",
    ".....kwyYk......",
    ".....kwyYk......",
    ".....kyyYk......",
    ".....kyyYk......",
    ".....kyYYk......",
    "......kkk.......",
    "................",
    "................",
], name="coin2")

FIREBALL = grid([
    "..kkkk..",
    ".kyyyok.",
    "kywyyook",
    "kyyyoorr"[:8],
    "kyooorrk",
    "koorrrrk",
    ".korrrk.",
    "..kkkk..",
], w=8, name="fireball")
FIREBALL = [r[:8] for r in FIREBALL]
DEBRIS = grid(["kkkkkkk.", "knbbbbk.", "kbbbbBk.", "kbbbBBkk", "kkBBBk..", ".kkkk...", "........", "........"], w=8, name="debris")

FLAG = grid([
    "kkkkkkkkkkkkkk..",
    "kgggggggggggggk.",
    "kgggwwgggwwgggk.",
    "kggwkwgggwkwggk.",
    "kggggggggggggk..",
    "kgglllllllllgk..",
    "kgggggggggggk...",
    "kggggggggggk....",
    "kgggggggggk.....",
    "kggggggggk......",
    "kgggggggk.......",
    "kggggggk........",
    "kgggggk.........",
    "kggggk..........",
    "kgggk...........",
    "kkkk............",
], name="flag")

CLOUD = grid([
    "..........kkkkk.................",
    "........kkwwwwwkk...............",
    ".......kwwwwwwwwwk...kkkk.......",
    "...kkkkwwwwwwwwwwwkkkwwwwk......",
    "..kwwwwwwwwwwwwwwwwwwwwwwwkk....",
    ".kwwwwwwwwwwwwwwwwwwwwwwwwwwk...",
    "kwwwwwwwwwwwwwwwwwwwwwwwwwwwwk..",
    "kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.",
    "kWwwwwwwwwwwwwwwwwwwwwwwwwwwwWk.",
    ".kWWwwwwwwwwwwwwwwwwwwwwwwwWWk..",
    "..kkWWWWWWWWWWWWWWWWWWWWWWWkk...",
    "....kkkkkkkkkkkkkkkkkkkkkkk.....",
], w=32, name="cloud")

HUD_COIN = grid(["..kkkk..", ".kyyyyk.", "kywyyYYk", "kywyyYYk", "kywyyYYk", "kyyyyYYk", ".kYYYYk.", "..kkkk.."], w=8, name="hcoin")
HUD_FROG = grid(["kkk..kkk", "kwkkkkwk", "kggggggk", "kggggggk", "kgkkkkgk", "kgllllgk", ".kGkkGk.", "..k..k.."], w=8, name="hfrog")
HUD_GEM = grid(["..kkkk..", ".kwcccck", "kkkkkkkk", "kcwcccCk", ".kcwcCk.", "..kcCk..", "...kk...", "........"], w=8, name="hgem")
HUD_GEM0 = remap(HUD_GEM, {'w': 'S', 'c': 'v', 'C': 'S'})

# ---------------------------------------------------------------- tiles
GROUND_TOP = grid([
    "kkkkkkkkkkkkkkkk",
    "gggggggggggggggg",
    "glgggggglgggggge",
    "ggggGgggggggGggg",
    "GgGGgGGgGGgGGgGG",
    "BGBBBGBBBBGBBBGB",
    "bbbbbbbnbbbbbbbb",
    "bbBbbbbbbbbBbbbb",
    "bbbbbbbbbbbbbbbb",
    "bnbbbbBbbbbbbbnb",
    "bbbbbbbbbbbbbbbb",
    "bbbbBbbbbbnbbbbb",
    "bbbbbbbbbbbbbbbb",
    "bbnbbbbbbBbbbbBb",
    "bbbbbbbbbbbbbbbb",
    "bbbbbbbnbbbbbbbb",
], name="gtop")
GROUND = grid([
    "bbbbbbbbbbbbbbbb",
    "bbBbbbbbbbnbbbbb",
    "bbbbbbbbbbbbbbbb",
    "bbbbbnbbbbbbbBbb",
    "bbbbbbbbbbbbbbbb",
    "bBbbbbbbBbbbbbbb",
    "bbbbbbbbbbbbbbbb",
    "bbbbbbbbbbbbbnbb",
    "bbbnbbbbbbbbbbbb",
    "bbbbbbbBbbbbbbbb",
    "bbbbbbbbbbbbbbbb",
    "bbbbbbbbbbbBbbbb",
    "bnbbbbbbbbbbbbbb",
    "bbbbbBbbbbbbbbbb",
    "bbbbbbbbbnbbbbbb",
    "bbbbbbbbbbbbbbbb",
], name="ground")
BRICK = grid([
    "nnnnnnnknnnnnnnk",
    "noooooBknoooooBk",
    "noooooBknoooooBk",
    "nBBBBBBknBBBBBBk",
    "kkkkkkkkkkkkkkkk",
    "nnnknnnnnnnknnnn",
    "oooknoooooBknooo",
    "oooknoooooBknooo",
    "BBBknBBBBBBknBBB",
    "kkkkkkkkkkkkkkkk",
    "nnnnnnnknnnnnnnk",
    "noooooBknoooooBk",
    "noooooBknoooooBk",
    "nBBBBBBknBBBBBBk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
], name="brick")
PRIZE1 = grid([
    "kkkkkkkkkkkkkkkk",
    "keyyyyyyyyyyyyYk",
    "keYyyyyyyyyyyYYk",
    "kyyyyyykkyyyyyYk",
    "kyyyyykwwkyyyyYk",
    "kyyyykwwwwkyyyYk",
    "kyyykwwwwwwkyyYk",
    "kyykwwwwwwwwkyYk",
    "kyyykwwwwwwkyyYk",
    "kyyyykwwwwkyyyYk",
    "kyyyyykwwkyyyyYk",
    "kyyyyyykkyyyyyYk",
    "kyyyyyyyyyyyyyYk",
    "keYyyyyyyyyyyYYk",
    "kYYYYYYYYYYYYYYk",
    "kkkkkkkkkkkkkkkk",
], name="prize")
PRIZE2 = remap(PRIZE1, {'w': 'e'})
USED = grid([
    "kkkkkkkkkkkkkkkk",
    "kbbbbbbbbbbbbbBk",
    "kbBbbbbbbbbbbBBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbbbbbbbbbbbbbBk",
    "kbBbbbbbbbbbbBBk",
    "kbbbbbbbbbbbbbBk",
    "kBBBBBBBBBBBBBBk",
    "kkkkkkkkkkkkkkkk",
], name="used")
SOLID = grid([
    "nnnnnnnnnnnnnnnk",
    "neeeeeeeeeeeeebk",
    "nennnnnnnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nennbnnnnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nennnnnnnnbnnbbk",
    "nennnnnnnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nennnnnbnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nennnnnnnnnnnbbk",
    "nebbbbbbbbbbbbbk",
    "nbbbbbbbbbbbbbbk",
    "kkkkkkkkkkkkkkkk",
], name="solid")
LOG_TL = grid([
    "kkkkkkkkkkkkkkkk",
    "kBbbbbbbbbbbbbbb",
    "kbnnnnnnnnnnnnnn",
    "kbnbbbbbbbbbbbbb",
    "kbnbnnnnnnnnnnnn",
    "kbnbnbbbbbbbbbbb",
    "kbnbnbnnnnnnnnnn",
    "kbnbnbnbbbbbbbbb",
    "kbnbnbnbnnnnnnnn",
    "kbnbnbnbnbbbbbbb",
    "kBbbbbbbbbbbbbbb",
    "kkkkkkkkkkkkkkkk",
    ".kbBbbbBbbbbBbbb",
    ".kbBbbbBbbbbBbbb",
    ".kbBbbbBbbbBbbbb",
    ".kbBbbBbbbbBbbbb",
], name="logtl")
LOG_TR = [r[::-1] for r in LOG_TL]
LOG_L = grid([".kbBbbbBbbbbBbbb", ".kbBbbBbbbbbBbbb", ".kbBbbBbbbbBbbbb", ".kbbBbBbbbbBbbbb"] * 4, name="logl")
LOG_R = [r[::-1] for r in LOG_L]
SEMI = grid([
    "kkkkkkkkkkkkkkkk",
    "nnnnnnnnnnnnnnnn",
    "nbnnnnbnnnnnbnnn",
    "bbbbbbbbbbbbbbbb",
    "BBBBBBBBBBBBBBBB",
    "kkkkkkkkkkkkkkkk",
    "..kBk......kBk..",
    "..kBk......kBk..",
    "..kBk......kBk..",
    "...k........k...",
] + ["................"] * 6, name="semi")
LAVA1 = grid([
    "................",
    "....yy......yy..",
    "..yyooyy..yyooyy",
    "yyoooooyyyoooooo",
    "oooorroooooorroo",
    "oorrrrrooorrrrro",
    "rrrrrrrrrrrrrrrr",
    "rrrRrrrrrrrRrrrr",
    "rrrrrrrRrrrrrrrr",
    "rRrrrrrrrrrrrRrr",
    "rrrrrrrrrrRrrrrr",
    "rrrrRrrrrrrrrrrr",
    "rrrrrrrrrRrrrrrr",
    "rrRrrrrrrrrrrRrr",
    "rrrrrrRrrrrrrrrr",
    "rrrrrrrrrrrrrrrr",
], name="lava1")
LAVA2 = [r[8:] + r[:8] for r in LAVA1]
POLE = grid(["......kGGk......"] * 16, name="pole")
POLE_TOP = grid([
    "................",
    "................",
    ".....kkkkkk.....",
    "....kgwgggGk....",
    "...kgwggggGGk...",
    "...kggggggGGk...",
    "...kgggggGGGk...",
    "...kggggGGGGk...",
    "....kgGGGGGk....",
    ".....kkkkkk.....",
    "......kGGk......",
    "......kGGk......",
    "......kGGk......",
    "......kGGk......",
    "......kGGk......",
    "......kGGk......",
], name="poletop")
FORT = grid([
    "SSSSSSSkSSSSSSSk",
    "sssssssksssssssk",
    "sWsssssksWsssssk",
    "kkkkkkkkkkkkkkkk",
    "SSSkSSSSSSSkSSSS",
    "sssksssssssksSSs",
    "sWskWssssssksSSs",
    "kkkkkkkkkkkkkkkk",
    "SSSSSSSkSSSSSSSk",
    "sssssssksssssssk",
    "sWsssssksWsssssk",
    "kkkkkkkkkkkkkkkk",
    "SSSkSSSSSSSkSSSS",
    "sssksssssssksssS",
    "sWskWssssssksssS",
    "kkkkkkkkkkkkkkkk",
], name="fort")
FORT_TOP = grid([
    "kkkkk......kkkkk",
    "kSSSk......kSSSk",
    "kSWsk......kSWsk",
    "kSssk......kSssk",
    "kSssk......kSssk",
    "kSsskkkkkkkkSssk",
] + FORT[:10], name="forttop")
DOOR = grid(["kkkkkkkkkkkkkkkk"] + ["kdddddddddddddddk"[:16]] * 15, name="door")
DOOR_TOP = grid([
    "SSSSSSSkSSSSSSSk",
    "sssssssksssssssk",
    "sWsssskkkksssssk",
    "kkkkkkddddkkkkkk",
    "SSSkkddddddkSSSS",
    "sssddddddddddSSs",
    "sWkddddddddddkSs",
    "kkdddddddddddddk",
    "SkdddddddddddddS",
    "skdddddddddddddk",
    "kddddddddddddddk",
    "kddddddddddddddk",
    "kddddddddddddddk",
    "kddddddddddddddk",
    "kddddddddddddddk",
    "kddddddddddddddk",
], name="doortop")
BUSH = grid([
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "......kkkk......",
    "....kkggggkk....",
    "...kgglgggggk...",
    "..kgglggggggGk..",
    ".kgglgggggggGGk.",
    ".kgggggggggGGGk.",
    "kggggggggggGGGGk",
    "kgggggggggGGGGGk",
    "kgGgggGggGGGGGGk",
    "kkkkkkkkkkkkkkkk",
], name="bush")
SWITCH = grid([
    "................",
    "..........kkk...",
    ".........kyyyk..",
    "........kyywyk..",
    ".......kYyyyk...",
    "......kYYkkk....",
    ".....kYk........",
    "....kYk.........",
    "...kYk..........",
    "..kYk...........",
    ".kkkkkkkkkkkkk..",
    ".kssssssssssSk..",
    ".kswssssssssSk..",
    ".ksssssssssSSk..",
    ".kSSSSSSSSSSSk..",
    ".kkkkkkkkkkkkk..",
], name="switch")
CHECK = grid([
    "................",
    "....kkkkkkkk....",
    "....kSkyyyyyk...",
    "....kSkywyyyyk..",
    "....kSkyyyyyyyk.",
    "....kSkyyyyyyk..",
    "....kSkyyyyyk...",
    "....kSkkkkkk....",
    "....kSk.........",
    "....kSk.........",
    "....kSk.........",
    "....kSk.........",
    "....kSk.........",
    "...kkkkk........",
    "..kSSSSSk.......",
    "..kkkkkkk.......",
], name="check")
CHECK_ON = remap(CHECK, {'y': 'g', 'w': 'l'})

# Tile ids (must match hopper.cpp enum Tile). Order = TILE_ART order.
TILE_NAMES = ["EMPTY", "GROUND", "BRICK", "QCOIN", "QPOW", "Q1UP", "USED", "SOLID",
              "LOG_TL", "LOG_TR", "LOG_L", "LOG_R", "COIN", "SEMI", "LAVA", "POLE",
              "POLE_TOP", "FORT", "FORT_TOP", "DOOR", "DOOR_TOP", "BUSH", "SWITCH",
              "BRICKC", "GEM", "CHECK", "HIDDEN"]
# Art slots (extra animation frames appended after the tile ids).
ART = {
    "GROUND": GROUND, "GROUND_TOP": GROUND_TOP, "BRICK": BRICK, "PRIZE1": PRIZE1,
    "PRIZE2": PRIZE2, "USED": USED, "SOLID": SOLID, "LOG_TL": LOG_TL, "LOG_TR": LOG_TR,
    "LOG_L": LOG_L, "LOG_R": LOG_R, "COIN1": COIN1, "COIN2": COIN2, "SEMI": SEMI,
    "LAVA1": LAVA1, "LAVA2": LAVA2, "POLE": POLE, "POLE_TOP": POLE_TOP, "FORT": FORT,
    "FORT_TOP": FORT_TOP, "DOOR": DOOR, "DOOR_TOP": DOOR_TOP, "BUSH": BUSH,
    "SWITCH": SWITCH, "GEM": GEM, "CHECK": CHECK, "CHECK_ON": CHECK_ON,
}
ART_ORDER = list(ART.keys())

THEMES = [
    # name, sky top, sky bottom, tile remap
    ("meadow", (104, 176, 248), (196, 232, 255), {}),
    ("cave", (14, 12, 30), (30, 24, 52),
     {'g': 's', 'G': 'S', 'l': 'W', 'e': 'W', 'b': 'v', 'B': 'k', 'n': 'S',
      'o': 'c', 'O': 'C'}),
    ("treetops", (150, 206, 255), (236, 246, 255), {}),
    ("fort", (24, 14, 22), (58, 26, 32),
     {'g': 's', 'G': 'S', 'l': 'W', 'e': 'W', 'b': 'S', 'B': 'v', 'n': 's', 'o': 's'}),
]
# Remap only applies to these (prize blocks, coins, lava, gems, flags keep colours).
THEMED = {"GROUND", "GROUND_TOP", "BRICK", "USED", "SOLID", "SEMI", "BUSH"}

SPRITES = [
    ("FROG", FROG), ("FROG_W1", FROG_W1), ("FROG_W2", FROG_W2), ("FROG_JUMP", FROG_JUMP),
    ("BIG", tall(FROG)), ("BIG_W1", tall(FROG_W1)), ("BIG_W2", tall(FROG_W2)), ("BIG_JUMP", tall(FROG_JUMP)),
    ("FFROG", remap(FROG, FIRE)), ("FFROG_W1", remap(FROG_W1, FIRE)), ("FFROG_W2", remap(FROG_W2, FIRE)), ("FFROG_JUMP", remap(FROG_JUMP, FIRE)),
    ("FBIG", remap(tall(FROG), FIRE)), ("FBIG_W1", remap(tall(FROG_W1), FIRE)), ("FBIG_W2", remap(tall(FROG_W2), FIRE)), ("FBIG_JUMP", remap(tall(FROG_JUMP), FIRE)),
    ("LADYBUG", LADYBUG), ("LADYBUG2", LADYBUG2), ("LADYBUG_FLAT", LADYBUG_FLAT),
    ("SNAIL", SNAIL), ("SNAIL2", SNAIL2), ("SHELL", SHELL), ("SHELL2", SHELL2),
    ("SPIKY", SPIKY), ("SPIKY2", SPIKY2),
    ("BOSS", scale2(STAG)), ("BOSS2", scale2(STAG2)),
    ("BERRY", BERRY), ("BERRY_1UP", BERRY_1UP), ("CHILI", CHILI), ("GEM", GEM),
    ("COIN1", COIN1), ("COIN2", COIN2), ("FIREBALL", FIREBALL), ("DEBRIS", DEBRIS),
    ("FLAG", FLAG), ("CLOUD", CLOUD), ("SEMI", SEMI),
    ("HUD_COIN", HUD_COIN), ("HUD_FROG", HUD_FROG), ("HUD_GEM", HUD_GEM), ("HUD_GEM0", HUD_GEM0),
]

# ---------------------------------------------------------------- levels
ROWS = 16
GT = 14   # ground top row


class L:
    def __init__(self, name, title, theme, width, time):
        self.name, self.title, self.theme, self.w, self.time = name, title, theme, width, time
        self.g = [[' '] * width for _ in range(ROWS)]

    def put(self, x, y, ch):
        if 0 <= x < self.w and 0 <= y < ROWS:
            self.g[y][x] = ch

    def ground(self, x0, x1, top=GT):
        for x in range(x0, x1):
            for y in range(top, ROWS):
                self.put(x, y, '#')

    def gap(self, x0, x1):
        for x in range(x0, x1):
            for y in range(ROWS):
                if self.g[y][x] == '#':
                    self.put(x, y, ' ')

    def lava(self, x0, x1):
        self.gap(x0, x1)
        for x in range(x0, x1):
            self.put(x, 15, '~')

    def row(self, x, y, s):
        for i, ch in enumerate(s):
            if ch != ' ':
                self.put(x + i, y, ch)

    def col(self, x, y, s):
        for i, ch in enumerate(s):
            if ch != ' ':
                self.put(x, y + i, ch)

    def log(self, x, h, base=GT):
        top = base - h
        self.put(x, top, '['); self.put(x + 1, top, ']')
        for y in range(top + 1, base):
            self.put(x, y, '{'); self.put(x + 1, y, '}')

    def stairs(self, x, n, up=True, base=GT, fill=True):
        for i in range(n):
            hgt = i + 1 if up else n - i
            for k in range(hgt):
                self.put(x + i, base - 1 - k, 'X')

    def finish(self, x):
        """Goal reed at x, fort at x+5. Needs ground under it."""
        self.put(x, GT - 1, 'X')
        self.put(x, GT - 11, '^')
        for y in range(GT - 10, GT - 1):
            self.put(x, y, '|')
        fx = x + 5
        for dx in range(5):
            for y in range(GT - 4, GT):
                self.put(fx + dx, y, 'F')
            self.put(fx + dx, GT - 5, 'T')
        self.put(fx + 2, GT - 1, 'D'); self.put(fx + 2, GT - 2, 'd')

    def text(self):
        return [''.join(r) for r in self.g]


def level1():
    v = L("1-1", "LILY MEADOW", 0, 212, 300)
    v.ground(0, 212)
    v.put(3, GT - 1, '@')
    v.put(8, GT - 1, 'b'); v.put(9, GT - 1, 'b')
    v.put(12, 10, '?')
    v.row(16, 10, "B?BMB")
    v.put(18, 6, '?')
    v.put(22, GT - 1, 'e')
    v.log(26, 2)
    v.put(31, GT - 1, 'e')
    v.log(34, 3)
    v.put(38, GT - 1, 'e'); v.put(40, GT - 1, 'e')
    v.log(43, 4)
    v.row(46, 9, "ooo")
    v.put(44, 4, '*')                 # gem 1: above the tall log
    v.log(51, 4)
    v.gap(56, 59)
    v.row(62, 10, "BMB")
    v.row(65, 6, "BBBBBBBB")
    v.row(66, 5, "oooooo")
    v.row(68, 10, "B")
    v.put(70, GT - 1, 'e'); v.put(72, GT - 1, 'e')
    v.row(74, 10, "BB?B")
    v.put(78, GT - 1, 's')
    v.row(82, 10, "?"); v.row(85, 10, "?"); v.row(88, 10, "?"); v.put(85, 6, 'M')
    v.put(84, GT - 1, 'e'); v.put(86, GT - 1, 'e')
    v.put(93, GT - 1, 'b')
    v.put(96, GT - 1, 'k')            # checkpoint
    v.row(100, 10, "B"); v.row(101, 6, "BBB"); v.put(102, 3, '*')   # gem 2: on top of the high bricks
    v.row(104, 10, "BBBBm")
    v.put(108, GT - 1, 'e'); v.put(110, GT - 1, 'e')
    v.stairs(114, 4, up=True)
    v.gap(118, 120)
    v.stairs(120, 4, up=False)
    v.put(126, GT - 1, 's'); v.put(130, GT - 1, 'e')
    v.log(134, 2)
    v.row(138, 9, "-----")
    v.row(139, 8, "ooo")
    v.gap(140, 143)
    v.log(146, 3)
    v.put(150, GT - 1, 'e'); v.put(152, GT - 1, 'e'); v.put(154, GT - 1, 's')
    v.row(156, 10, "BBB"); v.put(157, 11, ' ')
    v.row(160, 10, "?L?")
    v.put(164, GT - 1, 'b'); v.put(165, GT - 1, 'b')
    v.gap(168, 171)
    v.put(169, 8, '*')                # gem 3: over the last pit, needs a running jump
    v.stairs(175, 8, up=True)
    v.col(183, GT - 8, "XXXXXXXX")
    v.finish(190)
    return v


def level2():
    v = L("1-2", "ROOT CAVERN", 1, 200, 300)
    v.ground(0, 200)
    for x in range(0, 200):
        v.put(x, 0, 'B'); v.put(x, 1, 'B')
    v.put(3, GT - 1, '@')
    v.row(10, 10, "?M?")
    v.put(14, GT - 1, 'e'); v.put(16, GT - 1, 'e')
    v.row(20, 9, "BBBBBBBB")
    v.row(20, 8, "oooooooo")
    v.row(20, 5, "BBBBBBBB")
    v.put(30, GT - 1, 's')
    v.log(34, 3)
    v.gap(37, 40)
    v.log(40, 4)
    v.put(45, GT - 1, 'p')
    v.row(48, 10, "BBBB?BBBB")
    v.put(52, 6, 'L')
    v.put(56, GT - 1, 'e'); v.put(58, GT - 1, 'e'); v.put(60, GT - 1, 'e')
    for x in range(64, 80):
        v.put(x, 2, 'B'); v.put(x, 3, 'B'); v.put(x, 4, 'B')
    v.put(70, 5, '*')                 # gem 1: tucked under the low ceiling
    v.row(66, 10, "BBBBBBBBBB")
    v.put(68, 12, 'p'); v.put(74, GT - 1, 's')
    v.gap(82, 85)
    v.ground(85, 92, top=12)
    v.put(88, 11, 'e')
    v.gap(92, 95)
    v.put(97, GT - 1, 'k')
    v.row(100, 10, "BmB")
    v.stairs(106, 3)
    v.stairs(109, 3, up=False)
    v.put(114, GT - 1, 'p'); v.put(117, GT - 1, 'e')
    v.log(120, 2); v.log(126, 3); v.log(132, 4)
    v.put(123, GT - 1, 'e'); v.put(129, GT - 1, 's')
    v.put(133, 6, '*')                # gem 2: above the tallest root
    v.row(138, 10, "?M?")
    v.gap(144, 148)
    v.row(144, 10, "----")
    v.put(146, 6, '*')                # gem 3: above the plank over the pit
    v.put(152, GT - 1, 'p'); v.put(155, GT - 1, 'e'); v.put(157, GT - 1, 'e')
    v.stairs(162, 6)
    v.col(168, GT - 6, "XXXXXX")
    for x in range(170, 200):
        v.put(x, 0, ' '); v.put(x, 1, ' ')
    v.finish(178)
    return v


def level3():
    v = L("1-3", "CANOPY HOP", 2, 214, 300)
    v.ground(0, 16)
    v.put(3, GT - 1, '@')
    v.put(7, GT - 1, 'b')
    v.row(18, 11, "-----")
    v.row(19, 9, "ooo")
    v.ground(26, 32, top=12)
    v.put(29, 11, 'e')
    v.row(34, 10, "----")
    v.row(40, 8, "----")
    v.put(42, 4, '?'); v.put(43, 4, 'M')
    v.row(46, 11, "------")
    v.put(49, 10, 's')
    v.put(55, 10, 'h')                # moving platform (horizontal)
    v.row(54, 7, "ooooo")
    v.ground(64, 72, top=12)
    v.put(68, 11, 'e'); v.put(70, 11, 'e')
    v.put(75, 9, 'v')                 # moving platform (vertical)
    v.put(76, 3, '*')                 # gem 1: top of the lift's travel
    v.ground(80, 90, top=11)
    v.put(84, 10, 'k')
    v.row(92, 10, "---")
    v.row(97, 8, "---")
    v.row(102, 10, "---")
    v.put(98, 7, 'e')
    v.put(103, 4, '*')                # gem 2: high over the zig-zag
    v.ground(107, 116, top=12)
    v.row(109, 8, "B?B")
    v.put(113, 11, 's')
    v.put(120, 11, 'h')
    v.put(128, 9, 'h')
    v.row(126, 6, "oooo")
    v.ground(136, 142, top=11)
    v.put(140, 10, 'p')
    v.row(145, 9, "--"); v.row(149, 7, "--"); v.row(153, 9, "--")
    v.put(150, 3, '*')                # gem 3: above the stepping planks
    v.ground(157, 166, top=12)
    v.put(162, 11, 'e')
    v.put(169, 10, 'v')
    v.ground(174, 214)
    v.stairs(178, 5)
    v.col(183, GT - 5, "XXXXX")
    v.finish(192)
    return v


def level4():
    v = L("1-4", "THORN FORT", 3, 192, 300)
    v.ground(0, 192)
    for x in range(192):
        v.put(x, 0, 'X'); v.put(x, 1, 'X')
    v.put(3, GT - 1, '@')
    v.lava(12, 15)
    v.ground(15, 19, top=11)
    v.put(17, 10, 'f')                # firebar on a block
    v.lava(19, 22)
    v.row(26, 10, "?M?")
    v.put(30, GT - 1, 'p')
    for x in range(34, 52):
        for y in range(2, 8):
            v.put(x, y, 'X')
    v.put(42, 8, ' ')
    v.row(40, 10, "X")
    v.put(40, 10, 'f')
    v.put(46, GT - 1, 'e'); v.put(48, GT - 1, 'p')
    v.put(43, 9, '*')                 # gem 1: in the low corridor (dodge the bar)
    v.lava(54, 58)
    v.row(54, 10, "----")
    v.ground(58, 70, top=12)
    v.put(64, 9, 'f')
    v.lava(70, 74)
    v.put(72, 9, 'h')
    v.ground(76, 90)
    v.put(80, GT - 1, 'k')
    v.row(84, 10, "BmB")
    v.put(88, GT - 1, 's')
    v.lava(90, 95)
    v.row(91, 9, "---")
    v.put(92, 5, '*')                 # gem 2: high over the lava
    v.ground(95, 110, top=12)
    v.put(100, 8, 'f'); v.put(106, 11, 'p')
    v.lava(110, 114)
    v.ground(114, 120, top=10)
    v.put(117, 9, 'f')
    v.lava(120, 125)
    v.put(122, 9, 'h')
    v.ground(125, 140)
    v.row(128, 10, "?L?")
    v.put(134, 5, '*')                # gem 3: above the last rest stop
    v.put(132, GT - 1, 'p'); v.put(136, GT - 1, 'p')
    # boss arena
    v.ground(140, 192)
    for x in range(140, 192):
        v.put(x, 2, ' '); v.put(x, 0, 'X'); v.put(x, 1, 'X')
    v.put(160, GT - 2, 'K')
    v.put(182, GT - 1, 'A')
    v.ground(184, 192, top=GT)
    return v


LEVELS = [level1(), level2(), level3(), level4()]

# ---------------------------------------------------------------- reachability
# Mirrors the C++ physics in hopper.cpp (px, s). Keep in sync.
P = dict(GRAV_HOLD=1000.0, GRAV=2200.0, JUMP_V=370.0, JUMP_RUN=0.20, MAX_FALL=420.0,
         MAXV=150.0, W=12, H=14)
SOLIDCH = set("#B?MLmXH[]{}Af")  # tiles you can stand on (A switch is solid-ish)
SEMICH = set("-")


def solid_at(lv, tx, ty, txt):
    if tx < 0 or tx >= lv.w:
        return True
    if ty < 0:
        return False
    if ty >= ROWS:
        return False
    return txt[ty][tx] in SOLIDCH


def simulate(lv, txt, x, y, vx, hold, plats):
    """Simulate one jump/drop from feet (x left px, y bottom px). Returns landing
    (x, y) or None, plus the set of tiles swept."""
    dt = 1 / 120
    vy = -(P['JUMP_V'] + P['JUMP_RUN'] * abs(vx)) if hold is not None else 0.0
    w, h = P['W'], P['H']
    top = y - h
    swept = set()
    t = 0.0
    while t < 3.0:
        t += dt
        g = P['GRAV_HOLD'] if (hold and vy < 0 and t < hold) else P['GRAV']
        vy = min(vy + g * dt, P['MAX_FALL'])
        # x
        nx = x + vx * dt
        bad = False
        for ty in range(int(top // 16), int((top + h - 1) // 16) + 1):
            for tx in (int(nx // 16), int((nx + w - 1) // 16)):
                if solid_at(lv, tx, ty, txt):
                    bad = True
        if not bad:
            x = nx
        # y
        ny = top + vy * dt
        hit = False
        for tx in range(int(x // 16), int((x + w - 1) // 16) + 1):
            if vy < 0:
                ty = int(ny // 16)
                if solid_at(lv, tx, ty, txt):
                    hit = True; ny = (ty + 1) * 16
            else:
                ty = int((ny + h) // 16)
                feet_before = top + h
                if solid_at(lv, tx, ty, txt) or ((txt[ty][tx] in SEMICH or (tx, ty) in plats) if 0 <= ty < ROWS and 0 <= tx < lv.w else False) and feet_before <= ty * 16 + 0.01:
                    if vy >= 0:
                        return (x, ty * 16), swept
        if hit:
            vy = 0
        top = ny
        if top > ROWS * 16:
            return None, swept
        for ty in range(int(top // 16), int((top + h - 1) // 16) + 1):
            for tx in range(int(x // 16), int((x + w - 1) // 16) + 1):
                swept.add((tx, ty))
    return None, swept


def check_level(lv):
    txt = lv.text()
    plats = set()
    # moving platforms: their sweep counts as semi-solid tiles (approximation).
    for y in range(ROWS):
        for x in range(lv.w):
            c = txt[y][x]
            if c == 'h':
                for dx in range(-3, 6):
                    plats.add((x + dx, y))
            if c == 'v':
                for dy in range(-3, 4):
                    for dx in range(3):
                        plats.add((x + dx, y + dy))
    start = goal = None
    gems = []
    for y in range(ROWS):
        for x in range(lv.w):
            if txt[y][x] == '@':
                start = (x, y + 1)
            if txt[y][x] in '|A':
                goal = x if goal is None else min(goal, x)
            if txt[y][x] == '*':
                gems.append((x, y))
    seen = set()
    stack = [(start[0] * 16 + 2, start[1] * 16)]
    reach_tiles = set()
    max_x = 0
    while stack:
        x, y = stack.pop()
        key = (int(x // 4), int(y))
        if key in seen:
            continue
        seen.add(key)
        max_x = max(max_x, x)
        for vx in (-150, -100, -50, 0, 50, 100, 150):
            for hold in (None, 0.0, 0.15, 0.6):
                land, swept = simulate(lv, txt, x, y, vx, hold, plats)
                reach_tiles |= swept
                if land:
                    stack.append(land)
    ok = goal is not None and max_x + 12 >= goal * 16
    got = [gm for gm in gems if gm in reach_tiles]
    return ok, max_x / 16, goal, len(got), len(gems)


# ---------------------------------------------------------------- emit
LEVEL_CHARS = set(" #BLMm?X[]{}o*-~|^FTDdbkKA@esphvfK")


def emit():
    lines = ["// GENERATED by companions/hopper/build_assets.py - do not edit by hand.",
             "// Original art + levels for Hopper (EWatchOS module).", "#pragma once",
             "#include <stdint.h>", "", f"#define HOP_KEY 0x{KEY:04X}", ""]
    lines.append(f"#define HOP_ART_COUNT {len(ART_ORDER)}")
    for i, n in enumerate(ART_ORDER):
        lines.append(f"#define HOP_ART_{n} {i}")
    lines.append(f"#define HOP_THEMES {len(THEMES)}")
    lines.append("static const uint16_t HOP_TILEART[HOP_THEMES][HOP_ART_COUNT][256] = {")
    for (tn, top, bot, rm) in THEMES:
        lines.append(f"  {{ // {tn}")
        for n in ART_ORDER:
            rows = remap(ART[n], rm) if n in THEMED else ART[n]
            px = to565(rows)
            lines.append("    {" + ",".join(f"0x{p:04X}" for p in px) + "},")
        lines.append("  },")
    lines.append("};")
    lines.append("struct HopTheme { uint16_t skyTop, skyBot; };")
    lines.append("static const HopTheme HOP_THEME[HOP_THEMES] = {")
    for (tn, top, bot, rm) in THEMES:
        lines.append(f"  {{0x{rgb565(top):04X}, 0x{rgb565(bot):04X}}},  // {tn}")
    lines.append("};")
    lines.append("")
    lines.append("struct HopSprite { uint8_t w, h; const uint16_t *px; };")
    for n, rows in SPRITES:
        w = len(rows[0])
        px = to565(rows)
        lines.append(f"static const uint16_t HOP_PX_{n}[{len(px)}] = {{" + ",".join(f"0x{p:04X}" for p in px) + "};")
    lines.append("enum HopSpr : uint8_t {")
    for n, _ in SPRITES:
        lines.append(f"  SPR_{n},")
    lines.append("  SPR_COUNT };")
    lines.append("static const HopSprite HOP_SPR[SPR_COUNT] = {")
    for n, rows in SPRITES:
        lines.append(f"  {{{len(rows[0])}, {len(rows)}, HOP_PX_{n}}},")
    lines.append("};")
    lines.append("")
    lines.append("struct HopLevel { const char *id; const char *title; uint8_t theme; uint16_t width, time; const char *rows[16]; };")
    lines.append(f"#define HOP_LEVELS {len(LEVELS)}")
    lines.append("static const HopLevel HOP_LEVEL[HOP_LEVELS] = {")
    for lv in LEVELS:
        txt = lv.text()
        for r in txt:
            bad = set(r) - LEVEL_CHARS
            assert not bad, f"{lv.name}: bad chars {bad}"
        lines.append(f"  {{\"{lv.name}\", \"{lv.title}\", {lv.theme}, {lv.w}, {lv.time}, {{")
        for r in txt:
            lines.append("    \"" + r + "\",")
        lines.append("  }},")
    lines.append("};")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {os.path.relpath(OUT)} ({os.path.getsize(OUT)//1024} KB source)")


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def preview(path, S=4):
    items = [(n, ART[n]) for n in ART_ORDER] + [(n, r) for n, r in SPRITES]
    cols = 10
    cell = 34
    rows_n = (len(items) + cols - 1) // cols
    W, H = cols * cell * S, rows_n * cell * S
    img = bytearray([90, 140, 200] * W * H)
    for i, (n, rows) in enumerate(items):
        ox, oy = (i % cols) * cell * S, (i // cols) * cell * S
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                if ch == '.':
                    continue
                c = PAL[ch]
                for yy in range(S):
                    for xx in range(S):
                        p = ((oy + y * S + yy) * W + ox + x * S + xx) * 3
                        if p + 2 < len(img):
                            img[p:p + 3] = bytes(c)
    write_png(path, W, H, img)
    print("preview", path)


if __name__ == "__main__":
    if "--preview" in sys.argv:
        preview(sys.argv[sys.argv.index("--preview") + 1])
    if "--no-emit" not in sys.argv:
        emit()
    allok = True
    for lv in LEVELS:
        ok, mx, goal, g, gn = check_level(lv)
        print(f"  {lv.name} {lv.title:12s} w={lv.w} goal_col={goal} reach_col={mx:.0f} "
              f"{'OK' if ok else 'UNREACHABLE'} gems {g}/{gn}")
        allok &= ok and g == gn
    sys.exit(0 if allok else 1)
