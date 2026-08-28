# SLWF-12 — the manual

The SLWF-12 is a small box that sits in the room with your air conditioner and
works its remote control for you.

It does not need to be wired into the unit, it does not need the manufacturer's
app, and it does not need an account with anybody. You plug it in, join it to
your Wi-Fi, show it your remote once, and after that the air conditioner can be
worked from a phone, from a computer, from Home Assistant, from a message on
Telegram, from a spare button on your TV remote, or by rules the box runs on its
own.

![The control page](images/control-light.png)

## What is in this manual

| | |
|---|---|
| **[1. First run](01-first-run.md)** | Out of the box, onto your Wi-Fi, into the page |
| **[2. Teaching it your air conditioner](02-teaching.md)** | The one thing to do before anything else works |
| **[3. Everyday use](03-everyday-use.md)** | The control page, scenes, what everything means |
| **[4. Using your TV remote](04-other-remotes.md)** | A spare button on a handset you already own |
| **[5. Timers, schedules and automations](05-automations.md)** | Rules that run without anybody watching |
| **[6. Other devices](06-other-devices.md)** | Lights, other bridges, a computer to wake, a sensor to read |
| **[7. Connecting it to things](07-connecting.md)** | Home Assistant, Telegram, MQTT and the rest |
| **[8. Settings](08-settings.md)** | Language, units, clock, security, updates, backup |
| **[9. When something is wrong](09-troubleshooting.md)** | The short list of things that go wrong, and what to do |

Every screenshot here is a photograph of the real interface, not a drawing. If
one disagrees with what you see, the interface has moved on and the picture is
the stale one.

## What it can do, in one page

**Work the air conditioner.** Power, mode, temperature, fan speed, swing —
everything the handset does, from a page that looks like a thermostat.

**Follow the handset.** The box listens as well as talks. Use the original
remote and the page updates to match, so what you see is what the unit was last
told.

**Remember what you like.** Scenes are one-tap presets. "Night" might mean cool,
26°, low fan, quiet.

**Run on its own.** Daily schedules, countdown timers, and IF/THEN rules that
keep working when the internet is down, the phone is asleep and nobody is home.
It works out sunrise and sunset for itself, so "half an hour before sunset"
means the right time in December as well as June.

**Protect the compressor.** An air conditioner switched off and straight back on
can damage itself. The box holds the restart for as long as you tell it to, and
says so rather than silently ignoring you.

**Talk to other things.** Home Assistant (it appears by itself, no
configuration), Telegram, MQTT, Modbus, plain web requests — all switched off
until you ask for them.

**Control more than the one unit.** Other SLWF devices, WLED light controllers,
ESPHome devices, a computer to wake with a magic packet, a temperature sensor to
read. They appear as tabs on the control page and can be used in rules.

**Stay yours.** The box does not contact any server unless you press a button
that says it will, and it does not need the internet to work at all.

## How this differs from the SLWF-01 Pro

Both are SMLIGHT bridges for air conditioners, and which one is right depends
almost entirely on your unit.

The **SLWF-01 Pro** plugs into a socket **inside** the air conditioner — the
service connector some manufacturers fit for exactly this purpose. Because it is
wired in, it talks to the unit *both ways*: it can ask what the machine is
actually doing, read the unit's own temperature sensors, and know when something
changes without having to overhear it.

The **SLWF-12** works by **infrared**, like the handset does. It sits in the
room, sees the air conditioner, and sends the same codes the remote sends.

|  | SLWF-01 Pro | SLWF-12 |
|---|---|---|
| How it connects | A cable inside the unit | Infrared, across the room |
| Fitting it | Open the case, find the connector | Plug it in, point it at the unit |
| Which units it works with | Only those with a compatible internal port | Almost any unit with a remote |
| Does it know the real state | Yes — it asks the machine | It knows what it last sent, and what it overheard |
| Readings from the unit's sensors | Yes | No — infrared does not carry them back |
| If somebody uses the handset | Sees it | Sees it, because it is listening |
| If the unit is behind a door | Fine | It needs line of sight |

**Choose the SLWF-01 Pro** if your air conditioner has the port for it. Being
wired in is better in every way that matters: nothing to aim, nothing to block,
and the unit tells it the truth rather than being told.

**Choose the SLWF-12** if it does not — which covers most air conditioners,
including nearly every one already installed. It asks nothing of the machine
except that it came with a remote.

They are not rivals. Plenty of homes have one of each in different rooms, and
this box can control the other one over the network from the same page — see
[Other devices](06-other-devices.md).

## Getting the software

Firmware, release notes and the source are at
[github.com/smlight-tech/ir-remote-control](https://github.com/smlight-tech/ir-remote-control).

Start with **[First run](01-first-run.md)**.

---

## Licence

This is free software under the **GNU General Public License v3.0**. You may
use it, study it, change it and pass it on; if you pass on a device running it,
or a modified version, the people you give it to are entitled to the source of
what you gave them, under the same terms.

It comes with **no warranty** of any kind.

The full terms are in
[LICENSE](https://github.com/smlight-tech/ir-remote-control/blob/main/LICENSE),
and the interface links to them from its footer.
