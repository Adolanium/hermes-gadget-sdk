# Reach Hermes over your tailnet

If Hermes runs on a computer that is only reachable on your [Tailscale](https://tailscale.com/) tailnet, the gadget can join the tailnet itself and connect from any network, such as a phone hotspot. Nothing on the Hermes side is published to the internet.

This is experimental, and release firmware doesn't include it: you build the firmware yourself with the two changes in [hardware verification](hardware-validation.md#tailnet), on a board with PSRAM. It adds two things to the gadget:

- **A third-party Tailscale client.** The gadget uses [TailTiny](https://github.com/Asimmimx/tailtiny), an independent Tailscale client for ESP32. It isn't made or supported by Tailscale. This project adapts it for the gadget; [the list of changes](https://github.com/Adolanium/hermes-gadget-sdk/blob/main/firmware/esp32/components/TAILTINY.md) includes two to its encryption and TLS code.
- **An auth key stored on the device.** It lets the gadget join your tailnet, so treat it like a password: see [keep the key safe](#keep-the-key-safe).

If you would rather not build firmware, [Tailscale Funnel](tailscale-funnel.md) works with every board. It makes the gadget endpoint public, and device pairing guards it.

## Set up the Hermes computer

1. [Connect Hermes](connect-hermes.md) and keep its gateway running. The computer must be on your tailnet.
2. Find its tailnet address:

   ```bash
   tailscale ip -4
   ```

3. Let the gadget plugin listen on that address, or on every interface, in `~/.hermes/config.yaml`, then restart the gateway:

   ```yaml
   platforms:
     gadget:
       extra:
         host: 100.71.189.23   # this computer's tailnet address
   ```

4. The gadget's Hermes address is `ws://` with that address, the port and the path from `hermes gadget info`, for example `ws://100.71.189.23:8765/gadget`.

Use the numeric address. The gadget doesn't look up MagicDNS names, and a `wss://` address would fail its certificate check on a bare IP. Plain `ws://` is fine here: WireGuard encrypts everything between the gadget and the Hermes computer.

## Prepare your tailnet

In the Tailscale admin console, under **Settings → Keys**, generate an auth key for the gadget:

- **Reusable:** off. One key per gadget.
- **Ephemeral:** off, or the gadget leaves the tailnet when it powers off.
- **Pre-approved:** on, if your tailnet requires device approval.
- **Tags:** `tag:gadget` is recommended. A tagged device doesn't expire, and your access rules can limit it to the gadget port.

For example, this tailnet policy lets gadgets reach only the gadget port on the Hermes computer (replace the address):

```json
{
  "tagOwners": {"tag:gadget": ["autogroup:admin"]},
  "grants": [
    {"src": ["tag:gadget"], "dst": ["100.71.189.23"], "ip": ["tcp:8765"]}
  ]
}
```

Merge it into your existing policy. The gadget also needs outbound internet access to Tailscale's servers, which access rules don't affect.

## Set up the gadget

From the [USB console](setup-board.md#manage-an-existing-gadget):

```text
set ts_key tskey-auth-...
set server ws://100.71.189.23:8765/gadget
set tailnet on
```

Or, on the [phone setup page](setup-board.md#set-up-wi-fi-with-your-phone), tick **Reach Hermes over my Tailscale tailnet**, paste the key and enter the `ws://100.x.y.z` address with the network details. The page shows this section only on firmware that has it.

The key is only needed once: after the first start, the gadget keeps its tailnet identity across restarts.

## Keep the key safe

The auth key is a secret, like the Wi-Fi password:

- The gadget never logs it and never shows it: `get ts_key` shows `<set>`, and `status` and `diag` leave it out.
- It is stored unencrypted in the device's flash, so someone with the device in hand can read it over USB.
- `factory-reset` erases it, with the gadget's tailnet identity.

Use a tagged key with the access rules above, so a lost gadget can reach only the gadget port on the Hermes computer and nothing else on your tailnet.

## Revoke the key

If a gadget is lost, or you no longer use the tailnet on it:

1. In the Tailscale admin console, under **Machines**, find the gadget (its tailnet name is the device name), open its menu and choose **Remove**. It can't rejoin with its saved identity.
2. Under **Settings → Keys**, revoke the auth key if it's still listed. A one-off key that was used is already spent.
3. If you still have the gadget, run `factory-reset`, or `set tailnet off` and `set ts_key` with no value to clear the key.

## What the screen shows

After Wi-Fi connects, the gadget waits for the tailnet before it contacts Hermes. The connecting screen shows what it is waiting for:

| Shown | Meaning |
|---|---|
| Joining tailnet | Registering with Tailscale. This can take 15 seconds or more. |
| Reaching Hermes over the tailnet | On the tailnet; setting up the encrypted session with the Hermes computer. |
| Tailnet: auth key rejected | The key is wrong, used up or expired. Make a new one and `set ts_key` again. |
| Tailnet: approve this device in the admin console | Your tailnet requires approval and the key wasn't pre-approved. |
| Tailnet: no auth key (set ts_key) | `tailnet` is on but no key is saved. |
| Tailnet: Hermes host unreachable | The session with the Hermes computer stopped. Check that it's online, and that your access rules allow the gadget port. |
| Tailnet reconnecting | The gadget lost its tailnet connection and is getting it back. |

Once it is up, the gadget connects as usual: pairing, Ready and replies don't change.

## Check and change it

`diag` shows a `tailnet` section: the gadget's tailnet name and address, its state, and for the Hermes computer whether a session is up and whether traffic goes `direct` or through a Tailscale `relay`. On a phone hotspot traffic often goes through a relay, which adds a little delay to spoken replies.

- `set tailnet off` restarts the gadget, which then connects to the `server` address directly.
- Setting a different key while the tailnet is running also restarts the gadget.
- `factory-reset` removes the key and the gadget's tailnet identity. Remove the device from the admin console too ([revoke the key](#revoke-the-key)).

## Limits

- The gadget keeps a connection to Tailscale open at all times while this is on, so it uses more battery than a direct connection.
- Only numeric tailnet addresses work, not MagicDNS names.
- Only Tailscale's own coordination servers have been considered; Headscale isn't tried.
- The key is saved unencrypted in the device's flash ([keep the key safe](#keep-the-key-safe)).
