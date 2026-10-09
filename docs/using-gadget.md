# Talk, type, and interrupt

Wait for the device to show Ready. On the LCD board, BOOT is TALK and PLUS is CANCEL. On the round touchscreen, hold the screen to talk and swipe down to cancel.

| What you want to do | Control |
|---|---|
| Ask a question | Hold TALK, speak, and release to send |
| Interrupt a spoken reply | Hold TALK to start speaking |
| Discard a recording, close a card, or stop a turn | Tap CANCEL |
| Start a fresh conversation | Hold CANCEL for two seconds |
| Answer a confirmation question | TALK means yes; CANCEL means no |
| Type in the simulator | Enter a message in the text box and press Enter |
| Save the simulator's device screen | Press Ctrl+S |

Long replies turn their own pages. Simulated boards with scroll buttons also accept Up and Down.

Try asking Hermes to show a reminder on the screen. The agent can use `gadget_display` to display a card. Devices with actions can also respond to requests such as "Turn the LED purple."

If speech is missing, check [audio setup](connect-hermes.md#4-enable-speech). For connection problems, use [troubleshooting](troubleshooting.md). To change Wi-Fi or install firmware updates, see [Manage an existing gadget](setup-board.md#manage-an-existing-gadget).

## Device settings and hardware checks

Hold TALK and CANCEL together for one second to open or close device settings. On a touchscreen, hold the title bar for one second. Round displays have no title bar; they show a **SETTINGS** target beneath the status dot instead. Hold it for one second. Touches in the target do not start recording. The target is hidden while a question, update or Wi-Fi setup is on screen, because holding it does nothing then. While settings is open, its label reads **BACK TO HERMES**; hold it again for one second to return. Settings also work while the device is offline. Opening settings stops the current recording or reply.

Tap CANCEL to move to the next item, then tap TALK to change it. On a touchscreen, swipe down for the next item and tap to select. Boards with Up and Down buttons can move in either direction. Select **Back to Hermes** to leave.

| Item | What it does |
|---|---|
| Speaker volume | Change volume in steps of 10 percent |
| Screen brightness | Choose 10, 25, 50, 75, or 100 percent when the display supports it |
| Talk mode | Choose hold-to-talk or tap-to-talk with silence detection |
| Microphone check | Show the live input level; nothing is saved or sent to Hermes |
| Speaker check | Play a short, quiet tone at the current volume |
| Display check | Show red, green, blue, white, and black bands |
| Input check | Show the last button pressed; release CANCEL to leave the check |
| Device information | Show the board, firmware, device ID, and available audio drivers |
| Battery and power | Show available power readings on boards with a power driver |
| Screen timeout | Choose always on, 30, 60, 120, or 300 seconds |
| Power off | Select twice to shut down a board with a power driver |
| Wi-Fi setup | Start [phone-based setup](setup-board.md#set-up-wi-fi-with-your-phone) on ESP32 boards, with a QR code for the temporary network where the screen has room |

Volume, brightness, talk mode, and screen timeout survive restarts. Unavailable drivers show as unavailable. These checks help you test the hardware; a completed tone does not prove that a physical speaker produced sound.

Screen timeout is off by default. When enabled, a brightness-capable display dims halfway through the idle period, then goes dark. The first button press or touch wakes it without recording or answering a prompt. Incoming replies and prompts wake it too. Recording, playback, Wi-Fi setup, pairing and updates keep it awake. The settings menu sleeps like any other screen, and the input that wakes it leaves the menu on the same item, including a running hardware check. The processor and Wi-Fi remain running; this is screen sleep. Use the board's Power off control to shut down the device.

On the AMOLED-1.75C, a short press of PWR puts the screen to sleep the same way at any time, whether or not Screen timeout is on. Press PWR again, touch the screen or press BOOT to wake it. Replies, prompts and connection changes wake it as well.

Incoming confirmation prompts and firmware updates close settings. You cannot open settings during a prompt or update. The USB console also accepts `settings`, `settings close`, and `set brightness 50`.
