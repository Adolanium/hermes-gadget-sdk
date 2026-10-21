# Talk, type, and interrupt

Wait for the device to show Ready. On the LCD board, BOOT is TALK and PLUS is CANCEL. On the round touchscreen, hold the screen to talk and swipe down to cancel.

| What you want to do | Control |
|---|---|
| Ask a question | Hold TALK, speak, and release to send |
| Interrupt a spoken reply | Hold TALK to start speaking |
| Discard a recording, close a card, or stop a turn | Tap CANCEL |
| Start a fresh conversation | Hold CANCEL for two seconds |
| Open or close the [dashboard](#watch-hermes-sessions-on-the-dashboard) | Tap CANCEL on the idle screen |
| Answer a confirmation question | TALK means yes; CANCEL means no |
| Type in the simulator | Enter a message in the text box and press Enter |
| Save the simulator's device screen | Press Ctrl+S |

Long replies turn their own pages. Simulated boards with scroll buttons also accept Up and Down.

Try asking Hermes to show a reminder on the screen. The agent can use `gadget_display` to display a card. While the screen is dark, the card waits and appears when the screen lights up ([screen sleep](protocol.md#screen-sleep)). Devices with actions can also respond to requests such as "Turn the LED purple."

If speech is missing, check [audio setup](connect-hermes.md#4-enable-speech). For connection problems, use [troubleshooting](troubleshooting.md). To change Wi-Fi or install firmware updates, see [Manage an existing gadget](setup-board.md#manage-an-existing-gadget).

## Watch Hermes sessions on the dashboard

The dashboard lists the Hermes agent's sessions, from every platform the gateway serves, and any status tiles you set on the host. It includes sessions of other profiles only where the gadget plugin is enabled in that profile ([multiple profiles](hermes-integration.md#running-against-a-profile-or-a-multiplexed-gateway)). Each session shows its state:

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

## Device settings and hardware checks

Hold TALK and CANCEL together for one second to open or close device settings. On a touchscreen, hold the title bar for one second. Round displays have no title bar; they show a **SETTINGS** target beneath the status dot instead. Hold it for one second. Touches in the target do not start recording. The target is hidden while a question, update or Wi-Fi setup is on screen, because holding it does nothing then. While settings is open, its label reads **BACK TO HERMES**; hold it again for one second to return. Settings also work while the device is offline. Opening settings stops the current recording or reply.

Tap CANCEL to move to the next item, then tap TALK to change it. On a touchscreen, swipe down for the next item and tap to select. Boards with Up and Down buttons can move in either direction. Select **Back to Hermes** to leave.

| Item | What it does |
|---|---|
| Speaker volume | Change volume in steps of 10 percent |
| Screen brightness | Choose 10, 25, 50, 75, or 100 percent when the display supports it |
| Talk mode | Choose hold-to-talk or tap-to-talk with silence detection |
| Hermes profile | Choose the Hermes profile (touchscreens), or switch to the next one; shown only when Hermes offers more than one |
| Microphone check | Show the live input level; nothing is saved or sent to Hermes |
| Speaker check | Play a short, quiet tone at the current volume |
| Display check | Show red, green, blue, white, and black bands |
| Input check | Show the last button pressed; release CANCEL to leave the check |
| Device information | Show the board, firmware, device ID, and available audio drivers |
| Battery and power | Show available power readings on boards with a power driver; select to choose what the top of the screen shows: icon, percentage, both, or off |
| Screen timeout | Choose always on, 30, 60, 120, or 300 seconds |
| Power off | Select twice to shut down a board with a power driver |
| Wi-Fi setup | Start [phone-based setup](setup-board.md#set-up-wi-fi-with-your-phone) on ESP32 boards, with a QR code for the temporary network where the screen has room |
| Phone app | On boards with Bluetooth: pair a phone app for two minutes; the screen shows the six-digit code to type on the phone. Shows whether a phone is paired and connected |
| Language | Step through the languages of the device's own text: English, Spanish |

When Hermes offers several profiles (the host turns this on; see [Hermes integration](hermes-integration.md#running-against-a-profile-or-a-multiplexed-gateway)), the idle screen greets you with the current profile's name. The device shows at most 8 profiles, the default one first. On a touchscreen in hold-to-talk mode, tap the name for the list of profiles, then tap one; swipe down to close the list without choosing. A hold that starts on the name still talks. **Hermes profile** in settings opens the same list. Boards without a touchscreen, and tap-to-talk mode, show **Change in Settings**: there, **Hermes profile** switches to the next one. The device asks Hermes and changes only when Hermes confirms; if Hermes is busy, try again once the reply is done. The device keeps the profile across restarts.

Volume, brightness, talk mode, battery indicator, screen timeout, and language survive restarts. Unavailable drivers show as unavailable. These checks help you test the hardware; a completed tone does not prove that a physical speaker produced sound.

The battery indicator is off by default. When a driver percentage or explicitly enabled voltage estimate is available, each select on **Battery and power** moves to the next choice: icon only, percentage only, both, or off. It sits in the title bar, or centred at the top edge of a round display; without an available percentage it shows nothing. The icon fills green while charging and turns red at 10 percent or less with an explicit battery-only power reading; with the percentage alone, the number changes color instead. The percentage is the same value as in **Battery and power**. From the USB console, use `set battery_display icon`, `percentage`, `both` or `off`.

On voltage-only boards, first confirm that a compatible **single-cell 3.7 V Li-ion battery is actually fitted**, then enable the rough display estimate from the serial console:

```text
set battery_estimate liion
set battery_display percentage
```

Both settings persist; neither is enabled automatically. The Power page labels this value **voltage estimate**. It follows a typical single-cell Li-ion resting-voltage curve from 3300 mV (0%) to 4200 mV (100%), linear between points (3840 mV is 50%) and rounded down; readings outside 2000–4500 mV, missing readings, or an explicitly absent battery hide it. Charging raises the voltage, so while the board reports charging or external power the top of the screen shows the battery icon without a number, whatever the indicator style, and the Power page marks the value as high on a charger; 100% does not mean charging is complete. It does not detect battery presence, USB power, capacity, or runtime. Native driver percentages always take precedence, including zero. Raw status and gateway telemetry remain unchanged, and this display-only estimate does not add battery alarms.

Use `set battery_estimate off` (or `set battery_estimate` to clear it) to stop deriving percentages, and `set battery_display off` to hide any indicator. The percentage remains visible while charging; charging turns percentage-only text green. Red low-battery coloring requires an explicit battery-only power reading, which the LCD-1.54 does not supply.

Screen timeout is off by default. When enabled, a brightness-capable display dims halfway through the idle period, then goes dark. The first button press or touch wakes it without recording or answering a prompt. Incoming replies and prompts wake it too. Recording, playback, Wi-Fi setup, pairing (with Hermes or a phone) and updates keep it awake. The settings menu sleeps like any other screen, and the input that wakes it leaves the menu on the same item, including a running hardware check. The processor and Wi-Fi remain running; this is screen sleep. AMOLED panels also switch their output off while dark. Use the board's Power off control to shut down the device.

On the AMOLED-1.75C, a short press of PWR puts the screen to sleep the same way, whether or not Screen timeout is on. The same activities keep it awake: a press during recording, playback, Wi-Fi setup or an update does nothing. Press PWR again, touch the screen or press BOOT to wake it. Replies and prompts wake it as well; Wi-Fi or the connection to Hermes dropping and coming back does not.

## Language

The device's own text (the Ready screen, hints, status, settings and errors) is English unless you choose another language. Hermes's replies, cards and questions come in whatever language Hermes writes them, whatever this setting says.

Choose it in one of three ways:

- Select **Language** in device settings until the one you want shows. Each language is shown in its own name (Español), and the item keeps the English word beside its translation (Idioma / Language), so you can find it again in a language you can't read.
- Ask Hermes, for example "put the screen in Spanish". The firmware offers the agent a `screen.language` action.
- On the USB console, `set language es`; `set language` with no value goes back to English.

The languages are English (`en`) and Spanish (`es`). The font has no accented letters yet, so translated text shows them without accents ("Informacion"). To add a language, see [Translate the device text](development.md#translate-the-device-text).

Incoming confirmation prompts and firmware updates close settings. You cannot open settings during a prompt or update. The USB console also accepts `settings`, `settings close`, and `set brightness 50`.
