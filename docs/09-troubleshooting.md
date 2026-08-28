[← 8. Settings](08-settings.md) · **9. When something is wrong**

# When something is wrong

Start at the top. The first three account for nearly everything.

---

## The air conditioner ignores it

**Is it pointing at the unit?** Infrared is light. It does not go through walls,
around corners, or out of a drawer. Move the box so it can see the air
conditioner and try again.

**Is the right protocol set?** Settings → Air conditioner. If the box has not
been taught, nothing else will work — see [Teaching](02-teaching.md).

**Try Send again.** Bottom left of the control page. If that works, the unit
missed a command rather than failing to understand it, and the answer is
usually a better position — or **Repeats: 1** on the Air conditioner page, which
sends everything twice.

**Does the original remote still work?** If not, the problem is the air
conditioner, not the box.

---

## It waits before switching on

That is compressor protection, and it is doing its job. Your request is being
held, not lost, and the page counts down. See
[Everyday use](03-everyday-use.md#when-it-makes-you-wait).

---

## The page shows the wrong settings

The box knows what it last sent and what it last overheard. If somebody used the
handset while the box was unplugged or out of range, it will be confidently
wrong.

Press any button on the original remote in front of the box, or set something
from the page. Either brings the two back into agreement.

---

## I cannot open the page

**Try the address rather than the name.** `http://slwf12-a1b2c3.local` relies on
a discovery protocol that some networks and some Android versions do not
support. Your router's device list will show the plain address, like
`192.168.1.50`.

**Is it on the network at all?** If it is not in the router's list, it did not
join. Hold the button for ten seconds and set it up again.

**Are you on the same network?** A guest network is usually walled off from the
main one on purpose.

**Did you set a static address that does not fit your network?** Hold the button
for ten seconds.

---

## It keeps dropping off the Wi-Fi

**Signal.** Settings → Network shows the strength. Below about −75 dBm is
unreliable. Move it, or move the router, or add a repeater.

**2.4 GHz only.** The box cannot see 5 GHz networks. If your router recently
split its bands into two names, the box lost the one it knew.

**Channel width.** Some routers set 40 MHz on 2.4 GHz, which small devices
handle badly. 20 MHz is more reliable.

---

## Home Assistant does not show it

**Is MQTT switched on**, with the broker address filled in? The status line under
those fields says whether it is actually connected.

**Is MQTT ticked in the client list** at the top of the Integrations page? Both
have to be on.

**Does the broker need a username?** If it does and the box has not got one, it
will keep retrying quietly.

**Is Announce to Home Assistant ticked?** Without it the box publishes state but
never introduces itself.

---

## The TV remote button does nothing

Point that remote **at the box**, not at the television. See
[Using your TV remote](04-other-remotes.md#when-a-button-seems-not-to-work).

---

## A schedule did not fire

**Does the box know the time?** Settings → Clock. If it says the clock is not
set, open the page from a browser and it will be set within a second.

**Are schedules switched on** in the client list on the Integrations page?

**Is the rule itself enabled**, and is today one of its days?

**Is the timezone right?** A rule set for 07:00 fires at 07:00 by the box's
reckoning, which is only your 07:00 if the timezone is correct.

---

## An automation fires over and over — or stopped firing

If two rules trigger each other, the box notices and puts them to sleep for five
minutes rather than hammering the air conditioner. The log says so. Look for two
rules whose actions are each other's conditions.

A rule fires when its conditions *become* true, not continuously while they stay
true. If a rule seems to have fired only once, that is why — it will fire again
next time the conditions go from false to true.

---

## Everything is unresponsive

Settings → Backup and reset → **Restart**. Or unplug it for five seconds.

If it will not come back, hold the button for ten seconds to factory reset and
set it up again. You did download a backup, didn't you?

---

## The button on the box

| Hold for | What happens |
|---|---|
| about 5 seconds | Restart |
| about 10 seconds | Factory reset — everything erased, back to the setup network |

---

## Asking for help

Open an issue at
[github.com/smlight-tech/ir-remote-control/issues](https://github.com/smlight-tech/ir-remote-control/issues)
with:

- what the air conditioner is — brand and model
- what you did, what you expected, and what happened
- the firmware version from the bottom of the page
- the last few lines from **Settings → Log**

The log is the useful part. It usually says exactly what went wrong.
