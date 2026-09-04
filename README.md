# guacamole-server, with a faster display pipeline

This is a fork of [apache/guacamole-server](https://github.com/apache/guacamole-server)
1.6.0 that reduces the CPU cost of `guacd`'s frame encoding pipeline.

Nothing about the protocol, the configuration, or the client changes. The same
`guacd` binary serves the same connections and emits the same Guacamole
instruction stream — it just spends considerably less CPU doing it. On a busy
`guacd` that is felt directly as a smoother session, because the constraint on
a remote desktop over Guacamole is very often the encoder, not the network.

Upstream's own documentation is unchanged and still lives in [`README`](README).

## Why

On a 1600x900 VNC session over a 100 Mb/s link, measuring a worst case of
continuously scrolling full-screen text showed only **10.4 Mb/s** of traffic
while `guacd` sat at **110 % of one core**. Compressing harder would have been
the wrong move: the bottleneck was never the link.

Profiling `guacd` under that load showed where the time actually went, and two
things stood out. Roughly **a quarter** of it was spent in the search for image
data that could be reused from the previous frame, and a good part of the
remainder was a full-layer `memcpy` performed on every single frame — 5.8 MB
moved to commit a frame, whether the user had redrawn the whole desktop or
typed one character.

## What changed

Seven changes, all in `libguac`'s `guac_display` pipeline. None of them is
meant to alter what `guacd` sends, and that is checked rather than assumed;
see [Correctness](#correctness) below.

**Commit only the pixels that changed.** `guac_display_frame_complete()` copied
the entire layer from the pending frame to the last frame on every frame, even
though the pixels that can possibly differ are exactly those in the layer's
dirty rect — a rect that `guac_display_plan_create()` has already refined down
to what actually changed. It now copies that rect. This is what makes typing
nearly free.

**Hash a cell directly instead of sliding a window over it.** Indexing a dirty
64x64 cell ran the generic sliding-window hash machinery over that cell: a
64 KB scratch array zeroed, 4096 indirect callback invocations, and two hash
updates per pixel, all to produce a single hash value. It is now computed
directly. On a full-screen change that removed roughly 24 MB of pointless
`memset` per frame.

**Devirtualize the search loop.** The search for reusable data ran one indirect
call per pixel of the changed region — over a million per frame at 1600x900.
The two callers of the generic iterator now have nothing in common, so the
search has its own loop with the match check inlined, and the per-row test for
whether the sliding window is complete is hoisted out of the inner loop.

**Answer index misses from a bitmap.** The search probes a 64 K-entry, 1 MB
index once per pixel, and virtually every probe misses: only a few hundred
buckets are ever occupied. Each of those misses was a cache miss. An 8 KB
occupancy bitmap, small enough to stay in cache, now answers them. As a side
effect the index no longer needs clearing — only the bitmap does, which also
removes a 1 MB `memset` per frame.

**Compare pixel runs with `memcmp`.** Detecting what changed within a cell
walked two buffers one 32-bit pixel at a time, and always scanned to the end of
the run once a difference was found. The overwhelmingly common answer is
"nothing changed at all", which the C library's vectorized `memcmp` gives in a
fraction of the time; the search for the last difference now runs backwards and
stops early.

**Estimate compressibility once per update.** Choosing between WebP, JPEG and
PNG relies on an estimate of how well a region would compress losslessly, and
that estimate reads every pixel of the region. Both the WebP and the JPEG check
computed it separately, so a region that ended up as PNG paid for it twice. It
is now computed at most once, and only when a lossy format is actually in the
running.

**Hash four rows at a time.** Each value fed to the hash depends on the one
before it, so hashing one row at a time leaves the processor stalled on that
dependency. Four independent row hashes are now computed at once and folded
together in row order, which leaves the resulting value unchanged.

## Results

Measured with [`bench/guacbench`](bench/) on an Intel i5-7500T (4 cores), a
1600x900 layer, and a screenshot of a real desktop as source imagery — two
overlapping terminals full of dense monospace text over an editor window, which
is what a remote session actually carries. Each figure is the best of six runs
of 200 frames.

| Scenario | What it simulates | Wall/frame | | CPU/frame | |
|---|---|---:|---:|---:|---:|
| | | 1.6.0 | fork | 1.6.0 | fork |
| `typing` | a caret-sized region changing | 1.114 ms | **0.292 ms** | 1.113 ms | **0.295 ms** |
| `window` | a 600x400 window dragged | 7.407 ms | **5.214 ms** | 12.530 ms | **10.321 ms** |
| `video` | a 640x360 video region | 6.426 ms | **4.176 ms** | 10.651 ms | **8.360 ms** |
| `scroll` | full-screen text scrolling | 27.496 ms | **20.880 ms** | 41.598 ms | **34.880 ms** |
| `fullscreen` | the whole desktop redrawn | 28.948 ms | **21.961 ms** | 46.751 ms | **39.687 ms** |

That is **3.8x** on light interactive use, around **1.5x** on ordinary window
activity, and **1.3x** when the entire screen is churning. The gain is largest
exactly where a remote session spends most of its time — small, frequent
updates — because that is where the fixed per-frame cost dominated.

The source imagery matters, and not by a little. Run against a plain gradient
wallpaper instead of a real desktop, the same code reports 5.1x on `typing` and
1.4x on `fullscreen`: smooth images compress differently, are encoded in
different formats, and offer the copy search far more to find. Benchmark
against something that looks like what you actually send.

## Correctness

These changes are optimizations, not behaviour changes: the same input frames
must produce the same Guacamole instruction stream. `bench/guacbench` can dump
that stream, which makes the claim testable rather than merely asserted:

```sh
taskset -c 0 ./bench/guacbench --image desktop.png --frames 40 --pace 60 --out stream
```

`taskset -c 0` forces a single worker thread so that instruction ordering is
deterministic, and `--pace` holds the frame rate steady so that both builds
observe the same update frequency — the choice between PNG, JPEG and WebP
depends on how often a region changes, so a build that is simply faster would
otherwise legitimately encode differently.

Comparing the two builds this way over three runs each (after normalizing the
wall-clock timestamp carried by `sync`):

- `scroll`, `window` and `fullscreen` are fully deterministic in both builds,
  and their output is **byte-for-byte identical**. Between them they exercise
  the copy search, the cell hashing, the pixel comparison and the format choice
  across the entire screen on every frame.
- `typing` and `video` are not deterministic in *upstream* either: repeated runs
  of unmodified 1.6.0 produce different streams, because the encoder choice
  depends on wall-clock timing that `--pace` only partly constrains. For
  `typing`, every output this fork produced is one unmodified 1.6.0 also
  produced; for `video`, neither build repeats itself.

`make check` passes: 86 tests, 0 failures.

## Building and benchmarking

Build exactly as upstream. To also build the benchmark, from a tree that has
already been configured and built:

```sh
make -C bench
./bench/guacbench --image /path/to/screenshot.png --frames 200
```

The benchmark drives `guac_display` the way a protocol client does, with the
resulting instruction stream discarded, so it measures the encoding pipeline
alone — no network, no remote desktop server. Passing `--image` makes it use a
real screenshot as source imagery; without it, it synthesizes a desktop-like
image. See [`bench/README.md`](bench/README.md) for the details.

A `guacd` container image can be built as usual:

```sh
docker build -t guacd:1.6.0-fast .
```

Note that this fork pins the versions of FreeRDP and libwebsockets that the
image build fetches. Upstream's Dockerfile selects the newest tag matching a
pattern, and as of today both patterns resolve to versions that no longer build
against the Alpine 3.18 base: libwebsockets now requires GnuTLS, and the newest
FreeRDP 2.x tag is a development snapshot that `configure` refuses.

## Licensing and attribution

Apache License 2.0, unchanged from upstream. This fork modifies the following
files relative to Apache Guacamole 1.6.0, and adds `bench/` and this README:

```
src/libguac/display-flush.c
src/libguac/display-plan.c
src/libguac/display-plan.h
src/libguac/display-plan-search.c
src/libguac/display-worker.c
```

Apache Guacamole is a trademark of the Apache Software Foundation. This fork is
not affiliated with or endorsed by the ASF.
