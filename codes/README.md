# Shared air-conditioner profile database

A profile is everything the bridge needs to talk to one air-conditioner model.
Importing one means a new owner of the same unit does not have to teach
anything.

## Two kinds of profile

**Protocol profiles** (preferred) name an IRremoteESP8266 protocol. The bridge
then *builds* every command itself, so it can reach states the contributor
never demonstrated — every temperature, every fan speed, every combination.
These files are tiny and carry no raw codes.

**Raw profiles** carry recorded timings for units whose protocol the library
cannot encode. They only cover the states that were actually recorded, so they
are larger and less capable. Prefer a protocol profile whenever the identify
step succeeds.

## Layout

```
codes/
  index.json            catalogue the web interface reads
  profiles/<id>.json    one file per model
```

`index.json` is fetched by the browser, not by the device — an ESP8266 has
neither the heap for a large JSON document nor the spare flash for a TLS stack
to fetch it with. Keep it small; per-model detail belongs in the profile file.

## Profile format

```jsonc
{
  "schema": 1,
  "brand": "Daikin",
  "model": "FTXS35K",
  "protocol": "DAIKIN216",     // IRremoteESP8266 name, or "" for raw-only
  "protocolModel": -1,          // protocol variant, -1 when not applicable
  "useLearnedCodes": false,
  "minTemp": 18, "maxTemp": 32, "tempStep": 1,
  "carrierKhz": 38,
  "codes": [                    // empty for protocol profiles
    { "key": "off",          "khz": 38, "timings": [3400, 1750, 440, ...] },
    { "key": "cool_24_auto", "khz": 38, "timings": [3400, 1750, 440, ...] }
  ]
}
```

Code keys are `off`, `<mode>_<temperature>_<fan>`, or `btn_<name>` for
free-standing buttons. Modes are `auto`, `cool`, `heat`, `dry`, `fan_only`;
fan speeds are `auto`, `min`, `low`, `medium`, `medium_high`, `high`, `max`.

## Contributing

1. Get your unit working with the bridge.
2. **Teach → Share your unit → Download profile.** The browser assembles the
   file by reading the codes off the device one at a time.
3. Open a pull request adding `profiles/<brand>-<model>.json` and an entry in
   `index.json`, or open an issue with the file attached and a maintainer will
   do it.

Please fill in `brand` and `model` accurately — they are the only thing the
next person has to search on. Set `verified: true` in the index entry only
after somebody other than the contributor has confirmed the profile works.
