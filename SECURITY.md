# Security policy

## Reporting a vulnerability

Please report vulnerabilities privately, through GitHub's **[Report a vulnerability](https://github.com/Adolanium/hermes-gadget-sdk/security/advisories/new)** form on this repository, rather than in a public issue or pull request. Include what you found, how to reproduce it, and which part it affects.

You'll get a reply on the advisory, and the fix is discussed there before anything is published. Reports are credited in the advisory unless you'd rather stay anonymous.

Problems in Hermes Agent itself go to [Nous Research](https://github.com/NousResearch/hermes-agent/security), and problems in ESP-IDF or esptool-js to Espressif.

## Supported versions

The latest release and `main` get fixes. Older releases don't; update the plugin and reinstall or update the firmware instead.

## Scope

In scope: the Hermes plugin (`plugin/`), the device firmware (`firmware/`), the `hermes-gadget` tools (`python/`), the browser installer (`site/`) and the release tooling.

## How the pieces trust each other

The design is described in [docs/architecture.md](docs/architecture.md#security-model). In short:

- **Device identity.** Each device makes a random 32-byte key and sends it once, when it enrolls. After that it proves it holds the key with an HMAC over a fresh nonce from Hermes, so recorded traffic can't be replayed, and no other device can take over an enrolled id.
- **Authorization.** Hermes decides who may talk to it, with its own allowlists and DM pairing: a new device shows a code that someone approves on the Hermes host. The optional `GADGET_ACCESS_TOKEN` adds a shared secret every device must present.
- **Transport.** Plain `ws://` is readable by anyone on the same network, though the device key isn't sent after enrollment. Use `wss://` (`tls_cert` and `tls_key`) on networks you don't trust.
- **Firmware updates.** An update is authorized with the device's own key, so only the Hermes that enrolled a device can update it. The images themselves aren't signed and Secure Boot isn't enabled: the device runs whatever its trusted Hermes sends. Anyone who can run commands on the Hermes host can install firmware.
- **Physical access.** The USB console can read and change a device's settings, including its Wi-Fi details and server. Treat a device like any other computer you leave lying around.
- **Firmware with settings built in.** Wi-Fi details or an access token set with `idf.py menuconfig` end up inside `firmware.bin`. Don't share such a build; the release images contain no settings.
- **The browser installer.** It's a static page with no server. The Wi-Fi password and other settings go from the browser straight to the board over USB, and each firmware download is checked against the release's checksums before it's written.

Weaknesses in these documented limits, such as the unsigned updates, are known; reports that find a way around them are very welcome.
