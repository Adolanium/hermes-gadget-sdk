# TailTiny (vendored)

`tailtiny/` and `wireguard_lwip/` are [TailTiny](https://github.com/Asimmimx/tailtiny) 3.1.0 at commit `d1ec2ae45e252e8dc737452be0f6afe1ac2e3edb`: `components/tailtiny/` there, with its `LICENSE` and `x25519-license.txt` from the repository root, and the `wireguard_lwip` it nests in `components/` moved up beside it so ESP-IDF finds both. This repository stores the files with LF line endings; upstream's are CRLF. Licenses: [TailTiny-MIT](../../../LICENSES/TailTiny-MIT.txt), [wireguard-lwip-BSD-3-Clause](../../../LICENSES/wireguard-lwip-BSD-3-Clause.txt), [STROBE-x25519-MIT](../../../LICENSES/STROBE-x25519-MIT.txt).

Each change below has its own patch in `tailtiny-patches/`, applied in order with `patch -p1 -d firmware/esp32/components`. To compare with upstream, `diff -r --strip-trailing-cr` a fresh copy, laid out as above, against these directories.

To update: copy the new upstream release the same way, then reapply the patches in order and resolve what no longer applies.

## From upstream since 3.1.0

This firmware used to patch these in; they are in TailTiny 3.1.0, contributed from here ([#2](https://github.com/Asimmimx/tailtiny/pull/2), [#4](https://github.com/Asimmimx/tailtiny/pull/4), [#6](https://github.com/Asimmimx/tailtiny/pull/6) to [#12](https://github.com/Asimmimx/tailtiny/pull/12)):

- **ESP-IDF 6:** cJSON from the component registry, the UART and GPIO driver components, GCC 15's `nonstring` constants, Mbed TLS 4's PSA randomness for the relay's TLS, and the Noise channel's ChaCha20-Poly1305 from wireguard-lwip (`src/ml_noise_aead.h`). `firmware/tests/test_tailtiny_aead.cpp` still checks that wrapper against RFC 8439.
- **The relay's certificate is verified** (`CONFIG_ML_DERP_VERIFY_CERT`, on by default): against the firmware's CA bundle and the relay's host name. `tests/test_firmware_source.py` checks that the vendored code requires it and that no board turns it off.
- **One lock for WireGuard's state** (`wireguard-platform.h`, `src/wireguardif.c`): the timers and incoming packets on TailTiny's WireGuard task and the outgoing packets on lwIP's thread no longer share keypairs and counters without a lock. `tests/test_firmware_source.py` checks the rule.
- **Light sleep while the tailnet is idle:** the DERP and WireGuard tasks wait for work instead of polling. TailTiny registers ESP-IDF's eventfd VFS in `tailtiny_init()`; this firmware doesn't register it itself.

## Changes

1. **Build** (`01-build.patch`). For this firmware:
   - Both `CMakeLists.txt` compile their sources only with `CONFIG_HG_TAILNET` (`main/Kconfig.projbuild`); their requirements stay unconditional, because ESP-IDF resolves requirements before the sdkconfig exists. TailTiny's Kconfig menu depends on `HG_TAILNET`.
   - `idf_component.yml` drops the nested `wireguard_lwip` path: it sits beside TailTiny here.
2. **Why registration failed is kept** (`02-registration-error.patch`). `src/ml_coord.c` and `include/tailtiny_internal.h`: a new `last_error` field holds why registration last failed (the auth key was rejected, the device waits for approval, or the control server's own reason), and is cleared on connect, so the device screen can say what's wrong. Upstream only logs it.
3. **Smaller task stacks, one quieter log line** (`03-stacks-and-log.patch`). `include/tailtiny_internal.h`: the four task stacks go from 8/14/12/8 KB (net_io, derp_tx, coord, wg_mgr) to 5/8/9/5 KB: the peaks measured with `diag` on an AMOLED-1.75C after a voice reply (2.9, 4.4, 6.4 and 2.5 KB), plus 2 to 3.6 KB each. They live in internal RAM, which the audio path needs. `src/ml_wg_mgr.c`: the "WG peer UP" line in `ml_wg_mgr_peer_is_up()` logs at debug, not info, because the gadget polls it every second.
4. **A factory reset says whether it worked** (`04-factory-reset-result.patch`). `src/tailtiny.c`: `tailtiny_factory_reset()` returned `ESP_OK` even when erasing the keys or the cached peers failed. It now returns the error, so the port keeps its request to erase and tries again at the next boot.
5. **An expired key doesn't count as a live session** (`05-expired-keys-not-up.patch`). `src/wireguardif.c`: `wireguardif_peer_is_up()` counted the peer as up while either its current or its previous keypair was marked valid. The timers wipe an expired current key, but nothing ages out the previous one, so a session that had died long ago still read as up, and the port never showed the Hermes host as unreachable or started the reconnect grace. A key now counts only while it is younger than `REJECT_AFTER_TIME` (180 s), the age after which WireGuard won't use it.
