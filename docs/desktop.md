# Try the simulator

Run the same device core as the firmware in a desktop window. Start with a scripted demo, then connect your own Hermes. You do not need a board.

**Have a supported board already?** Use [Set up a board](setup-board.md). Its browser installer does not need a compiler.

## 1. Check your tools

Install Python 3.10+, CMake 3.16+, Git, and a C++17 compiler on the computer that runs the simulator.

| Platform | Compiler and desktop support |
|---|---|
| Windows | Visual Studio Build Tools with **Desktop development with C++**; include Tcl/Tk in the Python installer |
| macOS | Xcode command-line tools; a Python installation with Tkinter |
| Linux | GCC or Clang; your distribution's Tkinter package, often `python3-tk` |

Run `python -m tkinter` to check desktop support. A small test window opens. Use `python3` if that is your Python command.

## 2. Build the simulator

Choose the commands for your terminal.

### Windows

Run in PowerShell:

```powershell
git clone https://github.com/Adolanium/hermes-gadget-sdk.git
cd hermes-gadget-sdk
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -e ".[dev]"
hermes-gadget build-sim --test
```

If PowerShell blocks activation, use `.\.venv\Scripts\python.exe -m pip` and `.\.venv\Scripts\hermes-gadget.exe` directly. You do not need to change the computer's execution policy.

### macOS

Run in Terminal:

```bash
git clone https://github.com/Adolanium/hermes-gadget-sdk.git
cd hermes-gadget-sdk
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
hermes-gadget build-sim --test
```

### Linux

Run in your terminal:

```bash
git clone https://github.com/Adolanium/hermes-gadget-sdk.git
cd hermes-gadget-sdk
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
hermes-gadget build-sim --test
```

**You know it worked when:** the build completes and the core tests pass. The library is in `build/host/`. Rebuild after changing `firmware/core`.

## 3. Try the demo

In the activated terminal, start the development server:

```bash
hermes-gadget devserver --pairing
```

Keep that terminal open. In a second terminal, enter the same checkout, activate the same virtual environment, and start the simulator:

```bash
hermes-gadget sim --url ws://127.0.0.1:8765/gadget --board sim-466x466-round
```

1. Read the pairing code on the simulator screen.
2. In the first terminal, type `approve <CODE>`, replacing `<CODE>` with that code.
3. Type `hello` in the simulator's text box and press Enter.

**You know it worked when:** the device reaches Ready and displays a streamed echo of your message. This is a scripted reply, not an agent response.

In the development server terminal, try `card Shopping | eggs, milk` to display a card, or `action led.set {"color": "red"}` to change the virtual LED.

## 4. Try audio

In your checkout's activated environment, install the audio extra:

```bash
python -m pip install -e ".[audio]"
```

Close the simulator, then start it with your computer's microphone and speakers enabled:

```bash
hermes-gadget sim --url ws://127.0.0.1:8765/gadget --board sim-466x466-round --live-audio
```

Hold **Space**, speak, and release. The demo reports the clip and plays it back. Without live audio, TALK supplies silence. **Speak WAV...** sends a recording instead. Linux may also need the distribution's PortAudio package.

## 5. Connect your own Hermes

Stop the demo server before starting a gateway on the same port. Follow [Connect Hermes](connect-hermes.md), then point the simulator at the gateway's address. Real voice conversations also need Hermes speech recognition and text-to-speech configured.

Next: [everyday controls](using-gadget.md), [simulator boards and scripting](simulator.md), or [troubleshooting](troubleshooting.md).
