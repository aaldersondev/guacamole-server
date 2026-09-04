# guacbench

An offline benchmark for `libguac`'s `guac_display` rendering pipeline.

`guacbench` drives `guac_display` exactly as a protocol client (VNC, RDP, ...)
does — painting into a layer, declaring what changed, ending the frame — but
with synthetic frames and with the resulting Guacamole instruction stream
written to `/dev/null`. What it measures is therefore everything `guacd` does
per frame and nothing else: dirty-region detection, the search for image data
reusable from the previous frame, operation combining, and image encoding. No
network, no remote desktop server, no browser.

## Building

The benchmark reaches into `libguac`'s private headers, so it is deliberately
kept out of the autotools build and is built by hand against a tree that has
already been configured and built:

```sh
./configure
make
make -C bench
```

## Running

```sh
./bench/guacbench --image desktop.png --frames 200
```

```
scenario       frames   wall(ms)    cpu(ms)   ms/frame  cpu/frame
typing            200       58.4       59.0      0.292      0.295
scroll            200     4176.0     6976.0     20.880     34.880
window            200     1042.8     2064.2      5.214     10.321
video             200      835.2     1672.0      4.176      8.360
fullscreen        200     4392.2     7937.4     21.961     39.687
```

`wall` is elapsed time; `cpu` is total processor time across all threads, and
is the more meaningful of the two, since `guac_display` encodes on a pool of
worker threads and a busy `guacd` is limited by processor time rather than by
any one thread.

| Option | Meaning |
|---|---|
| `--width`, `--height` | Layer dimensions. Default 1600x900. |
| `--frames N` | Frames measured per scenario. Default 300. |
| `--image FILE.png` | Use a real screenshot as source imagery. Without it, a desktop-like image is synthesized. |
| `--scenario NAME` | Run only one scenario. |
| `--csv` | Emit CSV instead of a table. |
| `--out PREFIX` | Write each scenario's instruction stream to `PREFIX.SCENARIO`. |
| `--pace MS` | Hold each frame to a fixed duration. Makes timings meaningless; see below. |

## Scenarios

| Scenario | What changes per frame | What it stresses |
|---|---|---|
| `typing` | a 24x40 region at a fixed spot | the fixed per-frame cost, which dominates real interactive use |
| `scroll` | the full screen, shifted 16 px up | the search for reusable image data |
| `window` | a 600x400 region moving diagonally | the ordinary case of one active window |
| `video` | a 640x360 region of photographic content | the lossy encoders |
| `fullscreen` | the entire screen | everything, at maximum volume |

Real screenshots matter here. Synthetic imagery is either too compressible or
too noisy, and both extremes distort the choice between PNG, JPEG and WebP.
Capture one from the machine you care about:

```sh
import -window root desktop.png     # ImageMagick, under X11
```

Check what you captured. A lock screen or a bare wallpaper is a smooth gradient
with almost no text, and benchmarking against one measures something quite
different from a desktop: it inflates the apparent gain of anything that
touches the fixed per-frame cost and understates everything else.

## Comparing two builds

The interesting question about an optimization is usually not only whether it
is faster but whether it changed anything it should not have. `--out` writes
the instruction stream that would have gone to the browser, so two builds can
be compared on their actual output:

```sh
taskset -c 0 ./bench/guacbench --image desktop.png --frames 40 --pace 60 --out a
```

Both flags are needed:

- `taskset -c 0` leaves `guac_display` with a single worker thread, so the
  ordering of instructions is deterministic. With several workers the frame
  content is the same but the interleaving is not.
- `--pace` holds the frame rate steady. The choice between PNG, JPEG and WebP
  depends on how frequently a region is being updated, so a build that is
  merely faster will legitimately encode differently. Fixing the frame duration
  removes that variable.

The `sync` instruction carries a wall-clock timestamp, which must be normalized
away before comparing:

```sh
sed -E 's/4\.sync,[0-9]+\.[0-9]+,/4.sync,TS,/g' a.fullscreen > a.norm
```

Note that upstream 1.6.0 is not itself deterministic for every scenario. With a
real desktop screenshot as source imagery, `scroll`, `window` and `fullscreen`
repeat exactly and are worth comparing byte for byte; `typing` and `video` do
not, because the encoder choice depends on timing that `--pace` only partly
constrains. Which scenarios fall on which side depends on the source image, so
check that a scenario repeats for the *unmodified* build before drawing any
conclusion from a difference.
