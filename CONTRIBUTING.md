# Contributing

## The most valuable contribution: a profile

If you got your air conditioner working, share it. Web interface →
**Teach → Share your unit → Download profile**, then open a pull request or an
issue with the file attached. Details in [codes/README.md](codes/README.md).
CI validates every profile, so a malformed file fails fast rather than breaking
somebody's import six months later.

## Interface work needs no hardware

```bash
python tools/mock_server.py      # http://localhost:8080
```

A fake device that answers every endpoint, advances the learning wizard for
real, and nudges the state from a simulated remote so the live-update path gets
exercised. Stdlib only. `--unconfigured` shows the first-run flow;
`--latency 150` shows what the interface feels like over a weak signal.

### The manual's images

Every image under `docs/images/` is a photograph of the real interface, not a
mock-up:

```bash
python tools/screenshots.py     # the still images
python tools/demo_gif.py        # the animations (needs: pip install pillow)
```

Both drive the mock into each state the manual describes — mid-wizard, holding
a start for compressor protection, a scene applied — and capture the pages with
headless Chrome. Run them after changing anything visual, so the manual cannot
quietly drift out of step with the product.

The animations are sequences of settled pages rather than video captures. That
is deliberate for documentation: a reader gets time to read each step, and no
frame catches a CSS transition half-finished. GIF is used because it is the
only moving format GitHub renders inline in Markdown.

When you add a page to the manual, add it to the contents table in
`docs/README.md` — that table is the only index.

## Translations

Copy `web/lang/en.json` to `web/lang/<code>.json` and translate the values.
Keys are never translated. The device lists whatever files are present, so a
new language needs no code change at all — add the file, run
`pio run -t uploadfs`, and it appears in the picker.

Strings starting with `learn.` are the learning wizard's prompts, which the
firmware emits as keys precisely so that every client can render them in the
user's language. Please keep them conversational; they are read by somebody
standing in front of an air conditioner holding a remote.

## Code

- Arduino framework, C++17, built with PlatformIO. `pio run` must be clean.
- Keep the layering: client adapters call `bus::CommandBus`, never each other
  and never the IR service directly.
- Watch the heap. New long-lived allocations need justifying; the budget is
  already tight (see the memory notes in the README). Prefer fixed buffers and
  stack objects to `String` churn on hot paths.
- Match the surrounding comment density. Comments here explain *why* a
  constraint exists — they are mostly about the ESP8266's limits, and that
  context is not recoverable from the code.
- Log messages are read by users in the web interface, not only by developers.
  Write them as sentences.

## Adding a client adapter

1. Write a service under `src/net/` or `src/io/` with `begin()`, `loop()` and
   `reconfigure()`.
2. Parse whatever your transport speaks into an `ac::Delta`
   (`ac::deltaFromJson` does the heavy lifting for anything JSON-shaped).
3. Call `bus::commands.apply(delta, src::Source::YourSource)` and report the
   `Outcome` back to your user — including the failure cases, which carry
   messages worth showing.
4. Add your source to `src::Source` and to `src/core/Source.cpp`'s name table
   so it can be switched off from the Clients page.
5. Subscribe in `wireSubscribers()` in `src/main.cpp` if you need to react to
   changes made elsewhere.

## Reporting a bug

Include the output of `GET /api/status` — it carries the firmware version, heap
figures, IR counters and the detected protocol, which answers most of the first
round of questions. For IR problems, the **Teach → What the bridge is hearing**
panel is the useful part.
