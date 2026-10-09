# flypet — Mac-side fly brain

`flypet_sim.py` is a small behaviour state machine that drives the watch's fly
pet over USB serial. It is **behaviour-inspired** (walk / groom / rest / feed +
hunger / energy / mood drives) — a nod to the Drosophila connectome work, not
the real connectome. No datasets; it is fully self-contained.

The watch runs fine on its own (autonomous wander). Running this script just
hands the "brain" to the Mac so behaviour can be richer and scripted.

## Setup

```sh
cd pc
python3 -m venv .venv && source .venv/bin/activate   # optional
pip install -r requirements.txt
```

## Run

```sh
python3 flypet_sim.py                       # auto-detect the watch port
python3 flypet_sim.py --port /dev/cu.usbmodem1101
python3 flypet_sim.py --rate 15             # 15 commands/sec (default 10)
python3 flypet_sim.py --seed 42             # repeatable behaviour
python3 flypet_sim.py --quiet               # don't echo watch telemetry
```

The watch's USB serial is also the flash/monitor channel, so **close any
`pio device monitor` first** — only one program can own the port.

When you tap the fly on the watch it drops a food crumb and sends `>>FEED x y`;
the script picks that up and walks the fly over to eat it.

## Protocol

PC → watch, one line per update:

```
FLY action=<walk|groom|rest|feed|wander> x=<0..100> y=<0..100> \
    hunger=<0..100> energy=<0..100> mood=<0..100> state=<label>
```

- `x`/`y` are the target position in arena percent (0..100).
- All keys are optional; the watch keeps its last value for any key it doesn't
  see. `FLY` must be the first token.

Watch → PC, lines prefixed `>>` (everything else is boot/log noise, ignore it):

```
>>HELLO flypet v1                                  on screen open
>>STAT hunger=.. energy=.. mood=.. action=.. x=.. y=..   ~every 2 s
>>FEED x=.. y=..                                   user tapped to feed
>>BYE flypet                                       on screen exit
```

If no `FLY` line arrives for 4 s the watch falls back to autonomous mode (the
status line shows `[solo]`; it shows `[PC]` while this script is connected).
