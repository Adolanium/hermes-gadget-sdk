# TailTiny (vendored)

`tailtiny/` and `wireguard_lwip/` are [TailTiny](https://github.com/Asimmimx/tailtiny) 3.0.2 at commit `450ad519810f0a7560306e1389c65acf2c48828c`: `components/tailtiny/` there, with its `LICENSE` and `x25519-license.txt` from the repository root, and the `wireguard_lwip` it nests in `components/` moved up beside it so ESP-IDF finds both. This repository stores the files with LF line endings; upstream's are CRLF. Licenses: [TailTiny-MIT](../../../LICENSES/TailTiny-MIT.txt), [wireguard-lwip-BSD-3-Clause](../../../LICENSES/wireguard-lwip-BSD-3-Clause.txt), [STROBE-x25519-MIT](../../../LICENSES/STROBE-x25519-MIT.txt).

Each change below has its own patch in `tailtiny-patches/`, applied in order with `patch -p1 -d firmware/esp32/components`. To compare with upstream, `diff -r --strip-trailing-cr` a fresh copy, laid out as above, against these directories.

To update: copy the new upstream release the same way, then reapply the patches in order and resolve what no longer applies.

## Changes

1. **Build** (`01-build.patch`). ESP-IDF 6 and this firmware:
   - Both `CMakeLists.txt` compile their sources only with `CONFIG_HG_TAILNET` (`main/Kconfig.projbuild`); their requirements stay unconditional, because ESP-IDF resolves requirements before the sdkconfig exists. TailTiny's Kconfig menu depends on `HG_TAILNET`.
   - `idf_component.yml` drops the nested `wireguard_lwip` path (it now sits beside TailTiny) and adds `espressif/cjson`: ESP-IDF 6 no longer ships cJSON as `json`. `REQUIRES` names `espressif__cjson`, and `esp_driver_uart` because ESP-IDF 6 dropped UART from `driver`.
   - `src/ml_cellular.c` (the 4G modem driver) isn't compiled: cellular stays off, and the file includes `driver/uart.h` outside its `#ifdef`.
   - `-Wno-unterminated-string-initialization` for both components: GCC 15 flags byte arrays initialised from strings of exactly their length (HSalsa20's sigma, WireGuard's protocol labels), which is deliberate.
