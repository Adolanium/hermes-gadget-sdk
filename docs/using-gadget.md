# Talk, type, and interrupt

Wait for the device to show Ready. On the LCD board, BOOT is TALK and PLUS is CANCEL. On the round touchscreen, hold the screen to talk and swipe down to cancel.

| What you want to do | Control |
|---|---|
| Ask a question | Hold TALK, speak, and release to send |
| Interrupt a spoken reply | Hold TALK to start speaking |
| Read a spoken reply in full before it ends | Tap the screen, or press Up or Down |
| Discard a recording, close a card, or stop a turn | Tap CANCEL |
| Start a fresh conversation | Hold CANCEL for two seconds |
| Open or close the [dashboard](#watch-hermes-sessions-on-the-dashboard) | Tap CANCEL on the idle screen |
| Answer a confirmation question | TALK means yes; CANCEL means no |
| Go back to Ready with the default Hermes profile | Press the TALK key (BOOT on most boards) twice, quickly |
| Type in the simulator | Enter a message in the text box and press Enter |
| Save the simulator's device screen | Press Ctrl+S |

While Hermes speaks a reply, the face talks with the reply's latest two lines under it, and the whole text follows when the speech ends. Screens too small for the face and two lines, such as 240x135 and 128x128, show the text from the start.

Long replies turn their own pages. Simulated boards with scroll buttons also accept Up and Down.

On boards with PSRAM, a recording survives the connection to Hermes dropping. The screen says "Offline, still recording" and you can finish speaking as usual. The gadget sends the whole recording once it is back online and Hermes answers as usual. If the connection doesn't come back within 45 seconds of the end of the recording, the gadget says "Couldn't send the recording". CANCEL discards it meanwhile ([the recording buffer](architecture.md#the-recording-buffer)).

Try asking Hermes to show a reminder on the screen. The agent can use `gadget_display` to display a card. While the screen is dark, the card waits and appears when the screen lights up ([screen sleep](protocol.md#screen-sleep)). Devices with actions can also respond to requests such as "Turn the LED purple."

On a Waveshare P4 7B with an OV5647 module attached, ask Hermes to take a picture. The plugin saves the photo as a timestamped PNG in `~/.hermes/gadgetpix/` on the Hermes computer and tells Hermes its path. Photos stay there until you delete them.

If speech is missing, check [audio setup](connect-hermes.md#4-enable-speech). For connection problems, use [troubleshooting](troubleshooting.md). To change Wi-Fi or install firmware updates, see [Manage an existing gadget](setup-board.md#manage-an-existing-gadget).

## Watch Hermes sessions on the dashboard

The dashboard lists the Hermes agent's sessions, from every platform the gateway serves, and any status tiles you set on the host. When the host offers [several profiles](hermes-integration.md#running-against-a-profile-or-a-multiplexed-gateway), a device that can switch also lists the sessions of each offered profile that has the gadget plugin enabled. Each session shows its state:

| State | Colour | Meaning |
|---|---|---|
| busy | Blue | A turn is running |
| approval | Amber | The turn waits for you to approve a command, on whichever device or chat asked |
| idle | Grey | Nothing is running. An idle session leaves the list 30 minutes after its last turn |

Once the device has a dashboard, the idle screen says **CANCEL shows sessions** (on a touchscreen, **Swipe down for sessions**). Tap CANCEL or swipe down to open it, and again to close it. Talking, a question or a card takes the screen over. A list longer than the screen turns its own pages; Up and Down scroll it on boards with those buttons. The headline counts the sessions waiting for approval, or else the busy ones.

Tiles are lines you set from the Hermes host, for example for homelab services:

```bash
hermes gadget tile set nas up --state ok --label NAS
```

`--state` colours the tile's dot green (`ok`), yellow (`warn`) or red (`bad`). `--ttl 300` removes the tile after five minutes unless it is set again, so a script that stops reporting doesn't leave a stale value. `hermes gadget tile list` shows the tiles, and `hermes gadget tile clear nas` (or `--all`) removes them. The dashboard holds up to eight tiles. Gadgets showing the dashboard pick up a change within a few seconds.

A script or cron job can keep a tile current:

```bash
if ping -c1 -W2 nas.lan >/dev/null; then hermes gadget tile set nas up --state ok --ttl 300; else hermes gadget tile set nas down --state bad --ttl 300; fi
```

The device gets a new dashboard only when something on it changes. While the screen is dark, it gets the latest one when the screen lights up. To turn the dashboard off, set `dashboard: false` under `platforms.gadget.extra` ([configuration](hermes-integration.md#configuration)). Boards without a CANCEL button or a touchscreen don't show it.

## Round screens

On a round display, a thin ring just inside the glass edge shows what the device is doing. It is always on; there is no setting for it. Rectangular screens do not change. The first state that matches wins:

- **Timer alarm, low battery alert, charging and full battery screens:** no ring.
- **Resting clock:** 60 marks around the edge, in the grey of the date: the 12 hour marks are wider and run across the whole band, and the minute marks sit in the band's outer half. An amber dot, about twice as wide as the band, moves to the current second, once a second.
- **Listening:** a green arc grows clockwise from 12 o'clock over a dim green track, showing the part of the 30-second recording limit used. It turns amber in the last 5 seconds. In tap-to-talk mode, a pause usually ends the recording before the arc fills.
- **Firmware update:** an amber arc over a dim amber track shows the part of the image received. It is full while the device restarts.
- **Running timer:** the countdown arc, amber over dim amber, shrinking clockwise from 12 o'clock.
- **Thinking:** a blue comet laps the ring every 2.4 seconds.
- **Speaking:** an amber comet laps the ring every 4 seconds.
- **Starting up and connecting:** an amber comet, one lap every 2.4 seconds.

Anything else shows no ring. A recording, an update or a battery screen takes the ring from a running timer while it lasts, and the timer's arc comes back after it. Screens not in the list, such as settings, cards and the dashboard, still show the timer's arc while a timer runs.

## Timers

Ask Hermes for a timer, for example "set a pasta timer for nine minutes". Hermes starts it on the device with the `timer.start` action. Any gadget with a display or a speaker can run a timer. Hermes can also ask how much time is left, or cancel the timer.

- **Time left:** every screen shows it. On a round screen, an arc runs around the edge and shrinks as the time runs out; a recording, a firmware update or a battery screen takes the ring while it lasts, and the arc comes back after. On a rectangular screen, a line runs along the bottom of the top bar. The Ready screen also shows the time as text, for example "Timer 6:18".
- **Alarm:** at zero, the screen shows TIMER DONE and the timer's name. A board with a speaker beeps twice every 1.5 seconds for one minute, and the screen flashes green with each beep. A board without a speaker shows the alarm without sound. The Knob 1.8, CrowPanel 2.1, CrowPanel 5.0 and T-Display-S3 have no speaker.
- **Stop it:** press a button, tap the screen, or swipe down. The press only stops the alarm; it does not start a recording. Hermes can stop it with `timer.cancel`.
- **Quiet alarm:** after one minute the beeps stop, but TIMER DONE stays on screen until you stop it. If the screen went dark, a press lights it and shows the alarm again.
- **Other screens:** a card or an image from Hermes replaces the alarm. When Hermes starts to speak, the beeps stop and the alarm stays. A question from Hermes goes first: the alarm steps back and returns, ringing, once you answer the question or it expires. A timer that ends during a recording waits until the recording ends.
- **Limits:** one timer runs at a time, and a new timer replaces it. The timer keeps counting when the connection to Hermes drops. It lives in the device's memory, so a restart or power cut forgets it. The end of a timer does not interrupt Hermes: the device sends a `timer.done` event that the plugin only logs.

## Resting clock

Any board with a display can show a large clock when there is nothing else on the screen: an idle Ready screen, online and paired, after `clock` seconds of rest. It is off by default. Turn it on with the **Clock face** settings item (Off, 30 s, 1 min, 2 min, 5 min) or on the console with `set clock 60` (0 turns it off, or 5..3600 seconds).

The face shows the time and the date, with the mascot (or the profile's own face, where Hermes sends one) above them, at 70 percent of the brightness setting, and shifts by a few pixels each minute to spare the panel. On a round screen, the ring shows the 60 marks and a dot for the seconds; each minute the marks and the dot turn with the face's shift, to as much as 2 degrees either side of their place, so the marks do not stay lit on the same pixels. Anything on screen keeps it away: speech or a reply from Hermes, a card or a picture shown until dismissed, a question, a notice, a running timer, the alarm, the low-battery warning, or the settings menu. When the last of them is gone, the rest starts again.

A quick tap brings Ready back and does nothing else. A hold still talks, a swipe down still cancels, and the buttons work as on Ready; the PWR key turns a lit clock's screen off. The screen timeout still applies on top: the clock first dims, then goes dark, and the next input wakes to Ready.

The time comes from the network (`pool.ntp.org`, over SNTP) only while the clock is on; a device that never turns it on sends no time traffic. Boards with a clock chip (the AMOLED-1.75, 2.06 and 2.16; a PCF85063) keep the time in UTC across resets and are set from each network sync. The others wait for network time, and the settings item says so until it arrives.

On ESP32 boards, `set timezone` chooses the clock's timezone as a POSIX TZ string; the default is `UTC0`. Examples: `UTC0`, `EST5EDT,M3.2.0,M11.1.0`, `CST6CDT,M3.2.0,M11.1.0`, `CET-1CEST,M3.5.0,M10.5.0/3`, `IST-5:30`. A string the firmware cannot read means UTC. `set time_format 12h` shows 12-hour time on the face; `set time_format 24h` (the default) shows 24-hour time.

## Device settings and hardware checks

Hold TALK and CANCEL together for one second to open or close device settings. On a touchscreen, hold the title bar for one second. Round displays have no title bar; they show a **SETTINGS** target beneath the status dot instead. Hold it for one second. Touches in the target do not start recording. The target is hidden while a question, update or Wi-Fi setup is on screen, because holding it does nothing then. While settings is open, its label reads **BACK TO HERMES**; hold it again for one second to return. Settings also work while the device is offline. Opening settings stops the current recording or reply.

Settings is one list. Each row shows a small icon, the item's name, and its state at the right: a value, an on/off switch, a percentage, or `>` for an action. The selected row is highlighted and shows its description underneath; volume and brightness also show a slider there.

Tap CANCEL to move to the next item, then tap TALK to change it. Boards with Up and Down buttons, or a knob, can move in either direction. The list scrolls smoothly to keep the selected row in view.

On a touchscreen, drag the list up or down to scroll it, and tap a row to change it. The first tap on an on/off item flips it. The first tap on volume or brightness opens its slider; tap or drag along the bar to set the level, in steps of 5 percent. A drag only scrolls; it changes nothing. While a hardware check runs, a tap stops it and a swipe down moves to the next item, as before.

Select **Back to Hermes** to leave.

| Item | What it does |
|---|---|
| Speaker volume | Change volume in steps of 10 percent, or set it with the slider on a touchscreen |
| Screen brightness | Choose 10, 25, 50, 75, or 100 percent when the display supports it, or set it with the slider on a touchscreen (5 to 100 percent) |
| Talk mode | Choose hold-to-talk or tap-to-talk with silence detection |
| Touch to talk | Touchscreens with a TALK button (BOOT on most): choose whether holding or tapping the screen talks; on by default |
| Hermes profile | Choose the Hermes profile (touchscreens), or switch to the next one; shown only when Hermes offers more than one |
| Profiles per page | Touchscreens: choose how many profiles a page of the profile grid holds (2 x 2, 2 x 3, 3 x 3 or 4 x 4); shown only when Hermes offers more than one |
| Microphone check | Show the live input level; nothing is saved or sent to Hermes |
| Speaker check | Play a short, quiet tone at the current volume |
| Display check | Show red, green, blue, white, and black bands |
| Input check | Show the last button pressed; release CANCEL to leave the check |
| Device information | Show the board, firmware, device ID, and available audio drivers |
| Battery and power | Show available power readings on boards with a power driver; select to choose what the top of the screen shows: icon, percentage, both, or off |
| Low battery alert | Boards with a power driver and a display: choose Off (the default), 10%, 10% and 5%, 15%, 10% and 5%, or 20% and 10% for the screen to light with a low battery reminder |
| Low battery sound | Boards with a speaker: whether the low battery alert beeps; on by default |
| Charging screen | Boards with a power driver and a display: show the battery level when USB is plugged in; on by default |
| Charging sound | Boards with a power driver and a speaker: chirp when USB is plugged in; off by default |
| Full battery screen | Boards with a power driver and a display: light the screen once per charge when the battery is full; on by default |
| Full battery sound | Boards with a power driver and a speaker: chime once per charge when the battery is full; off by default |
| Deep Wi-Fi sleep | ESP32 boards that read their battery: choose whether Wi-Fi sleeps deeper on battery with the screen dark; on by default (see below) |
| Screen timeout | Choose always on, 30, 60, 120, or 300 seconds |
| Touch wakes screen | Touchscreens with a button that can wake the screen: choose whether a touch lights the dark screen; on by default |
| Face-down sleep | Boards with a motion sensor: sleep and stay quiet while the gadget lies on its screen; off by default |
| Clock face | Boards with a display: choose Off (the default), 30 seconds, 1, 2, or 5 minutes of rest before a resting clock shows |
| Power off | Select twice to shut down a board with a power driver |
| Wi-Fi setup | Start [phone-based setup](setup-board.md#set-up-wi-fi-with-your-phone) on ESP32 boards, with a QR code for the temporary network where the screen has room |
| Phone app | On boards with Bluetooth: pair a phone app for two minutes; the screen shows the six-digit code to type on the phone. Shows whether a phone is paired and connected |
| Language | Step through the languages of the device's own text: English, Spanish |
| Double press home | Boards with a TALK key: choose whether two quick presses of it go back to Ready with the default profile; on by default (see below) |
| Talk from dark screen | Boards with a TALK key: choose whether its press on a dark screen lights it and starts recording at once, or only lights it; off by default (see below) |

When Hermes offers several profiles (the host turns this on; see [Hermes integration](hermes-integration.md#running-against-a-profile-or-a-multiplexed-gateway)), the idle screen greets you with the current profile's name. The device shows at most 32 profiles, the default one first. On a touchscreen, swipe from right to left on the idle screen for a grid of profiles, opened at the page with the current one. Each profile shows an icon with its name under it, on up to two lines: the current profile's face; on boards with PSRAM, each other profile's own face or Hermes avatar, sent as a small picture and updated when it changes in Hermes (see [Faces](faces.md#thumbnails-in-the-profile-grid)); else the mascot for the default profile, and for the others the first letter of the name (a capital, for ASCII and Latin-1 letters), in a colour that stays the same for each profile. The current profile has a ring around its icon. After the last profile comes a **Settings** cell with a gear: tapping it closes the grid and opens the settings menu. It takes a cell like a profile, so it counts toward the pages. Swipe left or right on the grid to turn the page (the dots at the bottom show which page this is), tap a profile to switch to it and go back to the idle screen, or swipe down to close the grid without choosing. On boards where PWR turns the screen off (such as the AMOLED-2.06 and 1.75C), a short press of PWR on the grid goes back to the idle screen with the screen still lit; the next press turns it off. The grid follows Screen timeout like the idle screen: it dims at half the timeout, and when the screen goes dark the grid closes, so the next input wakes to the idle screen. In hold-to-talk mode, a tap on the name also opens the grid, and a hold that starts on the name still talks. With **Touch to talk** off, a tap on the name opens the grid in tap-to-talk mode too, and a hold on it does nothing. **Hermes profile** in settings opens the same grid. Boards without a touchscreen show **Change in Settings**: there, **Hermes profile** switches to the next one.

A page holds 9 profiles (3 x 3) by default. **Profiles per page** in settings, or `set profile_grid 4|6|9|16`, chooses 2 x 2, 2 x 3 (3 x 2 on a landscape screen), 3 x 3 or 4 x 4. A screen too small for the chosen grid uses the largest smaller one where each profile stays at least about 7 mm tall and wide and its name keeps the screen's text size: the 2.06" and 1.8" AMOLEDs show all four, the round 1.75" AMOLED, the 1.85C and the 2.16" AMOLED stop at 3 x 3, and the smallest screens at 6 profiles a page. With few profiles a page uses the smallest grid that holds them all, so their icons are larger. The device asks Hermes and changes only when Hermes confirms; if Hermes is busy, try again once the reply is done. The first time you talk to a profile that hasn't approved this gadget yet, the screen shows the command to run on the Hermes host, such as `hermes -p work pairing approve gadget <code>`; run it, then ask again. The device keeps the profile across restarts.

To leave the profile you are on in one move, press the TALK key twice within 0.4 s, start to start. On boards where BOOT talks, such as the AMOLED-2.06 watch, that key is BOOT. The second press throws away what the first one started: the recording is cancelled without its `audio.end`, so Hermes never gets a question from it, though the start of its audio may already have reached Hermes. It stops a reply on its way, closes the profile list, the dashboard or a card, and brings back Ready. Then it asks Hermes for the default profile, as **Hermes profile** would. Already on the default profile, it only brings back Ready. The first press still acts at once, so a single press or a hold talks with no delay. Only the key counts: taps on the touchscreen never make a double press. A press that only wakes a dark screen, or dismisses the alarm, starts no double press, so you can press once to wake and then hold to talk. With `wake_talk` on (below), the press that lights the screen already talks, so a double press on a dark screen lights it and goes home. A first press held long enough to send its recording (0.35 s) starts no double press either, since that question is already on its way to Hermes. In the settings menu, TALK still selects items, while Hermes asks a question it still answers yes, and while the timer alarm shows it still dismisses the alarm. If Hermes is still finishing a reply it may refuse the switch; press twice again once it is done. The **Double press home** settings item or `set double_home off` turns the shortcut off, and a second quick press then talks again; it is on by default.

On a touchscreen with a TALK button (BOOT on most touch boards), the **Touch to talk** settings item or `set touch_talk off` leaves the touchscreen out of talking: in either talk mode, holding or tapping the screen never starts a recording, never sends one and never talks over a reply. Only the button does. The idle screen's hint names the button, such as **hold BOOT to talk**. Everything else the touchscreen does stays: a tap still answers yes to a question (a swipe down answers no), changes a settings item, stops a timer alarm, opens the profile grid or shows a spoken reply's whole text, and a swipe down still discards a recording, closes a list or card, stops a turn or opens the dashboard. Those answer or navigate, and none of them opens the microphone. Boards without a TALK button ignore the setting, since the touchscreen is their only way to talk. It is on by default.

Volume, brightness, talk mode, touch to talk, battery indicator, low battery alert, low battery sound, charging screen, charging sound, full battery screen and sound, screen timeout, face-down sleep, clock face, time format, language, double press home, talk from dark screen and deep Wi-Fi sleep survive restarts. Unavailable drivers show as unavailable. These checks help you test the hardware; a completed tone does not prove that a physical speaker produced sound.

The battery indicator is off by default. When a driver percentage or explicitly enabled voltage estimate is available, each select on **Battery and power** moves to the next choice: icon only, percentage only, both, or off. It sits in the title bar, or centred at the top edge of a round display, just below the ring; without an available percentage it shows nothing. The icon fills green while charging and turns red at 15 percent or less (or at the highest low battery alert level, when that is higher) with an explicit battery-only power reading; with the percentage alone, the number changes color instead. The percentage is the same value as in **Battery and power**. From the USB console, use `set battery_display icon`, `percentage`, `both` or `off`.

On voltage-only boards, first confirm that a compatible **single-cell 3.7 V Li-ion battery is actually fitted**, then enable the rough display estimate from the serial console:

```text
set battery_estimate liion
set battery_display percentage
```

Both settings persist; neither is enabled automatically. The Power page labels this value **voltage estimate**. It follows a typical single-cell Li-ion resting-voltage curve from 3300 mV (0%) to 4200 mV (100%), linear between points (3840 mV is 50%) and rounded down; readings outside 2000–4500 mV, missing readings, or an explicitly absent battery hide it. Charging raises the voltage, so while the board reports charging or external power the top of the screen shows the battery icon without a number, whatever the indicator style, and the Power page marks the value as high on a charger; 100% does not mean charging is complete. It does not detect battery presence, USB power, capacity, or runtime. Native driver percentages always take precedence, including zero. Raw status and gateway telemetry remain unchanged. The estimate adds no alarms of its own; the low battery alert below uses it only on a board that reports battery-only power.

Use `set battery_estimate off` (or `set battery_estimate` to clear it) to stop deriving percentages, and `set battery_display off` to hide any indicator. The percentage remains visible while charging; charging turns percentage-only text green. Red low-battery coloring requires an explicit battery-only power reading, which the LCD-1.54 does not supply.

The low battery alert is off by default. With levels set, the screen lights when the battery percentage reaches one of them, even if it was dark, and fills it with the alert: a large red battery, nearly empty, with a **!** in it, the percentage large in red, and **Low battery** and **Connect USB to charge** below. The icon reads by its shape too, not only by its red. The mascot steps aside while it shows. It goes by itself after 10 seconds, and the screen then times out as usual. Using the gadget while it shows closes it and does what it always does: holding TALK or the screen starts recording, a tap on the name opens the profiles, CANCEL opens the dashboard, PWR darkens the screen. A screen that was dark still takes its first touch or press as only a wake. Choose the levels with the **Low battery alert** settings item, or from the USB console with up to three percentages from 1 to 50:

```text
set battery_alert 10,5
set battery_alert off
```

Each level alerts once per discharge: a percentage that wobbles around a level, or a reading that jumps past it, does not repeat it, and a jump past several levels shows only the lowest. A level counts again only after charging lifts the battery at least 5 points above it, so a minute on USB does not bring the alert back the moment it is unplugged. Plugging in closes an alert on the screen, and no alert shows while the board reports USB power or charging. Like the red indicator, it needs an explicit battery-only reading from the power driver, so the LCD-1.54 never shows it. It waits while you are talking, Hermes is replying or asking a question, the settings are open, a card is up, or the gadget lies face down, and shows at the next reading after that. An open profile list or dashboard does not hold it back: the alert shows over it, and the list is still there when the alert goes. On a board with a speaker it beeps once as it appears, unless Hermes is speaking, the microphone is open or the timer alarm is ringing; turn the beep off with the **Low battery sound** settings item or `set battery_alert_sound off`. On boards whose percentage comes from an uncalibrated gauge, such as the LCD-1.83's AXP2101, it fires at whatever that gauge reports, which can be well away from the real charge left. The AMOLED-1.75C and the AMOLED-2.06 use a discharge curve measured on their own cell instead.

Plugging in USB lights the screen for 5 seconds, as a phone does: a large green battery filled to the current level with a lightning bolt, the percentage large in green, and **Charging** below. The screen then times out as usual. A dark screen notices the cable at once; a lit one within 5 seconds. With the screen timeout off, a screen the power key had darkened goes dark again when the notice ends. It only shows after a battery-only reading, so starting up on USB or a board that cannot tell when USB is connected shows nothing, and unplugging shows nothing either. It does not interrupt a conversation, a reply, a question, the settings or a gadget lying face down: plugged in during one of those, it is skipped, and the battery indicator shows the charge. It shows over an open profile list or dashboard, which is still there after. On a voltage estimate the level reads high on a charger, so the notice shows the bolt and **USB power** without a level. Using the gadget while it shows closes it and acts as usual, as with the low battery alert. Turn it off with the **Charging screen** settings item or `set charging_notice off`. Plugging in is silent by default; the **Charging sound** settings item or `set charging_sound on` adds a short rising chirp, played whether or not the screen notice is on, and never over Hermes, an open microphone or the timer alarm.

When a charge finishes on USB, the screen lights for 5 seconds with a full green battery, **100%** large in green, and **Battery full** and **You can unplug** below. The charge counts as finished when the driver, after reporting charging, reports not charging for three readings in a row (about 10 to 15 seconds) with USB still in, or when the gauge climbs to 100%, whichever comes first. A charger that is still starting up when the cable goes in, or that drops out for a single reading, does not count. It is said once per plug, and only after the battery was seen charging, so plugging in a full battery says nothing. Like the other notices it waits for a quiet screen, goes by itself, and using the gadget meanwhile closes it and acts as usual. A soft rising chime can go with it: it is off by default, because a charge often ends at night, and `set charged_sound on` or the **Full battery sound** settings item turns it on. The screen and the chime are separate settings; `set charged_notice off` or the **Full battery screen** item turns the screen off, for example for a gadget that charges by the bed.

Screen timeout is off by default. When enabled, a brightness-capable display dims halfway through the idle period, then goes dark. The first button press or touch wakes it without recording or answering a prompt. Incoming replies and prompts wake it too. A reply that the screen went dark on before you touched anything comes back when you wake the screen, until you press a button or touch the screen while its text is showing. Waiting for Hermes to answer, recording, playback, Wi-Fi setup, pairing (with Hermes or a phone) and updates keep it awake. The settings menu sleeps like any other screen, and the input that wakes it leaves the menu on the same item, including a running hardware check. The profile list and the dashboard time out too, and close when the screen goes dark, so the input that wakes it shows the home screen. The processor and Wi-Fi remain running; this is screen sleep. AMOLED panels also switch their output off while dark. Use the board's Power off control to shut down the device.

The **Talk from dark screen** settings item or `set wake_talk on` makes the TALK key (BOOT on the AMOLED-2.06) talk from a dark screen in one press: the press lights the screen and starts recording, as it would on a lit screen. In hold-to-talk mode, keep holding and speak; in tap-to-talk mode, the recording runs until you tap again or stop speaking. In hold-to-talk mode, a press too short to be speech is discarded, as always. The press still only wakes the screen when it is from the touchscreen or PWR, when the gadget lies face down, when the alarm or a question is on screen, when the settings menu went dark, and when the gadget can't reach Hermes or isn't paired. It is off by default, so a button pressed in a bag doesn't record. It pairs well with `touch_wake off`, which leaves the key as the way to wake the screen.

On battery with the screen dark and nothing going on, Wi-Fi sleeps deeper: once Hermes has sent nothing but its heartbeat for 10 seconds, the radio checks the router for messages every third beacon, about every 300 ms on most routers, instead of every beacon. A message from Hermes, such as a reminder, can then arrive up to about 0.3 s later. Lighting the screen or talking wakes the radio fully at once, so a reply is not slowed, and any message from Hermes brings back the lighter sleep. Some routers drop or delay a client that sleeps this deeply: if the gadget goes offline while its screen is dark on battery, turn off the **Deep Wi-Fi sleep** settings item, or `set wifi_deep_sleep off`. Wi-Fi then uses only the lighter sleep on battery, as firmware before this setting did. The item shows on ESP32 boards that read their battery. On USB, Wi-Fi never sleeps either way.

On a touchscreen with a button that can wake the screen (TALK, BOOT or PWR), the **Touch wakes screen** settings item or `set touch_wake off` leaves the touchscreen out of waking it: while the screen is dark, touches are ignored, and only a button press lights it. That press only wakes the screen, as before. Once it is lit, the touchscreen works as usual. A gadget in a pocket or bag then stays dark however it is touched. Boards without such a button ignore the setting, since the touchscreen is their only way back. It is on by default.

On the AMOLED-1.75C, a short press of PWR puts the screen to sleep the same way, whether or not Screen timeout is on. The same activities keep it awake: a press during recording, playback, Wi-Fi setup or an update does nothing. In the settings menu, the press is Back instead: it closes the menu, and any hardware check running in it, and leaves the screen lit on the home screen; a second press there turns the screen off. On the profile grid it is Back too: it goes back to the idle screen, still lit. Over the dashboard, or a card or image, the press closes it and turns the screen off, so the screen wakes to the home screen. Press PWR again, touch the screen or press BOOT to wake it. Replies and prompts wake it as well; Wi-Fi or the connection to Hermes dropping and coming back does not.

Boards with a motion sensor and a backlit display (the AMOLED-1.75) can also sleep face down. This is off by default; turn it on with the **Face-down sleep** settings item or `set face_down_sleep on`. After about a second lying face down the screen goes dark, listening is cancelled and Hermes goes quiet: a reply being spoken stops, and speech that arrives while the gadget lies there is dropped. Hermes keeps working, and the reply's text waits; it shows from the top once the gadget is turned over. A ringing timer alarm goes quiet too, and stays quiet when the gadget is turned back over: laying it face down is a way to hush it. A timer that ends face down still rings, with the screen dark. Nothing lights the screen while it lies there, and turning it over lights it within a second; if the sensor stops reporting, the screen lights on its own.

## Language

The device's own text (the Ready screen, hints, status, settings and errors) is English unless you choose another language, or choose **auto** to follow Hermes's. Hermes's replies, cards and questions come in whatever language Hermes writes them, whatever this setting says.

Choose it in one of three ways:

- Select **Language** in device settings until the one you want shows. After the last language comes **Automatic, as Hermes**. Each language is shown in its own name (Español), and the item keeps the English word beside its translation (Idioma / Language), so you can find it again in a language you can't read.
- Ask Hermes, for example "put the screen in Spanish". The firmware offers the agent a `screen.language` action.
- On the USB console, `set language es` or `set language auto`; `set language` with no value goes back to English.

With **auto**, the device takes the language Hermes names when it connects: Hermes's own language (`HERMES_LANGUAGE`, else `display.language`), or the gadget plugin's `language` setting if you set one ([configuration](hermes-integration.md#configuration)). A language the firmware doesn't have, or none, shows as English.

The languages are English (`en`) and Spanish (`es`). The font draws Latin-1 and the Russian alphabet, so translations and Hermes's replies keep their accents, ñ, ¿ and ¡, and Russian replies show in Cyrillic; letters from other alphabets show without their marks, or as `?`. To add a language, see [Translate the device text](development.md#translate-the-device-text).

Incoming confirmation prompts and firmware updates close settings. You cannot open settings during a prompt or update. The USB console also accepts `settings`, `settings close`, and `set brightness 50`.
