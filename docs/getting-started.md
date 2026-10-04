# Getting started

Choose how you want to use Hermes Gadget. A board connects to your own Hermes Agent. The desktop simulator can also run a scripted demo without Hermes.

| Your starting point | Guide | What you need |
|---|---|---|
| I have a supported board | [Set up a board](setup-board.md) | Chrome or Edge on a computer, a USB data cable, 2.4 GHz Wi-Fi, and Hermes |
| I want to try it on my computer | [Try the simulator](desktop.md) | Python 3.10+, CMake 3.16+, and a C++17 compiler |
| I have a Raspberry Pi | [Run a Linux gadget](linux.md) | Pi 4 or 5, 64-bit Raspberry Pi OS Lite Trixie, and Hermes; optional USB audio |
| I want to build with the SDK | [Development](development.md) | A repository checkout and the tools for the part you want to change |

Board setup uses prebuilt firmware. You do not need Python, CMake, or a compiler on the computer that flashes it.

## Connect your Hermes

<a id="3-connect-to-hermes"></a>

[Connect Hermes](connect-hermes.md) explains plugin installation, pairing, and speech configuration. Follow it when you want real agent replies, either on a board or in the simulator.

The scripted demo echoes messages. It does not run an agent or understand speech.

## Use your gadget

[Talk, type, and interrupt](using-gadget.md) covers everyday controls. For an existing device, see [change Wi-Fi or update firmware](setup-board.md#manage-an-existing-gadget).

## Find help

[Troubleshooting](troubleshooting.md) covers build failures, connection problems, pairing, and missing audio. The browser installer can save a diagnostics report without installing Python.

## Build with the SDK

- [Customize the face](faces.md).
- [Add a board, actions, or sensors](porting.md).
- [Run the tests](development.md#test-suites).
- Read the [architecture](architecture.md), [protocol](protocol.md), and [Hermes integration reference](hermes-integration.md).
