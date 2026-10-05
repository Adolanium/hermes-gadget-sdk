# Remote access with Tailscale Funnel

Use Tailscale Funnel on your Hermes computer to let a gadget connect from outside your home network, including a phone hotspot. The gadget does not need Tailscale installed.

Funnel makes the gadget endpoint public. Device pairing still controls access to Hermes. Publish only the gadget plugin's port.

## Set up the Hermes computer

First [connect Hermes](connect-hermes.md) and keep its gateway running. Install and sign in to [Tailscale](https://tailscale.com/docs/features/tailscale-funnel) on that computer. Your tailnet must allow Funnel; follow any authorization link the command displays.

1. Find the device URL:

   ```bash
   hermes gadget info
   ```

   Note its port and path. For example, `ws://192.168.1.20:8765/gadget` uses port `8765` and path `/gadget`.

2. Publish that port. Replace `8765` if your plugin uses another port:

   ```bash
   tailscale funnel --bg 8765
   tailscale funnel status
   ```

   Funnel prints a public HTTPS address such as `https://your-hostname.tail1234.ts.net` and proxies requests to the local gadget port. Check that the target port matches `hermes gadget info`.

3. Build the gadget address: change `https://` to `wss://` and append the path from step 1:

   ```text
   wss://your-hostname.tail1234.ts.net/gadget
   ```

   Keep a custom path if you configured one. The gadget requires a `ws://` or `wss://` address; it does not convert HTTPS URLs. Do not add the local port `8765` to the public address.

With `--bg`, Funnel resumes after a reboot while Tailscale is running. Hermes must also be running. To remove all Funnel configuration on this computer:

```bash
tailscale funnel reset
```

See the [Funnel command reference](https://tailscale.com/docs/reference/tailscale-cli/funnel) for managing individual published services.

## Connect the gadget

Enter the full `wss://` address above as the Hermes address in the browser installer's Wi-Fi step, [phone setup](setup-board.md#set-up-wi-fi-with-your-phone), or the [USB console](setup-board.md#manage-an-existing-gadget).

To use a phone hotspot:

1. Enable a 2.4 GHz hotspot and leave it running.
2. Open **Wi-Fi setup** on the gadget.
3. From a device that can join the gadget's `Hermes-XXXX` setup network, open `http://192.168.4.1`. Enter the hotspot name, password, and the full `wss://` Hermes address.
4. Choose **Check connection and save**. Keep the hotspot available during the connection check.

If your phone cannot keep its hotspot active while joined to the setup network, use a second phone or computer for setup, or configure the gadget through USB.

The board stores one Wi-Fi network. A successful setup replaces its Wi-Fi credentials; a failed connection check restores the previous settings. Re-enter your home Wi-Fi credentials when you return. The same Funnel address works on both networks, and changing Wi-Fi preserves the gadget's identity and pairing with the same Hermes host.

## Verify the connection

Test with the gadget on a network outside your home, such as the hotspot. If it shows a pairing code, run `hermes gadget pair` on the Hermes computer and approve the matching device. Wait for **Ready**, then send a message and confirm that Hermes replies.

The plugin serves a WebSocket endpoint, not a website. Opening the bare Funnel URL in a browser can return `404 Not a Hermes gadget endpoint`; this does not test pairing or a working gadget connection.

## Security and troubleshooting

- Funnel terminates TLS on the Hermes computer and forwards traffic to the local service. The endpoint is reachable from the public internet, even by devices outside your tailnet.
- Device pairing and the protocol's per-device authentication still apply. If you configured an access token, the gadget also needs that token. See [authentication](protocol.md#handshake).
- For connection errors, check `tailscale funnel status`, the local port, the `wss://` scheme, and the complete path. Confirm that Hermes is running.
- A successful phone setup check confirms Wi-Fi connectivity. **Ready** and a reply confirm the connection to Hermes.
- The gadget cannot complete a captive portal login. Use a network that permits its connection, such as a phone hotspot.
