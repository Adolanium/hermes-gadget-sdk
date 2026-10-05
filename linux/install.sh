#!/bin/sh
# Install a verified release without replacing device state or configuration.
set -eu
umask 022

if [ "$(id -u)" -ne 0 ]; then
    echo 'Run this installer with sudo.' >&2
    exit 1
fi
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$source_dir"
sha256sum --check --status SHA256SUMS
python3 -c '
import json, platform, re
p = json.load(open("package.json"))
arch = {"aarch64": "arm64", "x86_64": "amd64"}.get(platform.machine())
if p["architecture"] != arch:
    raise SystemExit("This package does not match the computer architecture")
if not re.fullmatch(r"[0-9.]+-[0-9a-f]{12}", p["release_id"]):
    raise SystemExit("Invalid package release ID")
if not re.fullmatch(r"hermes_gadget-[0-9.]+-py3-none-any.whl", p["wheel"]):
    raise SystemExit("Invalid package wheel name")
'
release_id=$(python3 -c 'import json; print(json.load(open("package.json"))["release_id"])')
wheel=$(python3 -c 'import json; print(json.load(open("package.json"))["wheel"])')
destination=/opt/hermes-gadget/releases/$release_id
install -d -m 755 /opt/hermes-gadget/releases
exec 9>/opt/hermes-gadget/install.lock
flock -n 9 || { echo 'Another installer is running.' >&2; exit 1; }

if [ ! -f "$destination/.installed" ]; then
    if [ -e "$destination" ]; then
        echo "An incomplete installation exists at $destination. Move it aside and retry." >&2
        exit 1
    fi
    install -d -m 755 "$destination"
    install -m 644 libhgsim.so "$destination/libhgsim.so"
    install -m 644 package.json "$destination/package.json"
    install -d -m 755 "$destination/licenses"
    cp LICENSE NOTICE THIRD_PARTY_NOTICES.md "$destination/licenses/"
    cp -R LICENSES "$destination/licenses/"
    python3 -m venv --system-site-packages "$destination/venv"
    "$destination/venv/bin/python" -m pip install "$source_dir/${wheel}[audio,gpio]"
    HGSIM_LIBRARY="$destination/libhgsim.so" "$destination/venv/bin/python" -c \
        'from hermes_gadget.sim.native import load_library; load_library()'
    touch "$destination/.installed"
fi
cmp package.json "$destination/package.json"
HGSIM_LIBRARY="$destination/libhgsim.so" "$destination/venv/bin/python" -c \
    'from hermes_gadget.sim.native import load_library; load_library()'

if ! getent passwd hermes-gadget >/dev/null; then
    useradd --system --user-group --home-dir /var/lib/hermes-gadget --shell /usr/sbin/nologin hermes-gadget
fi
for group in audio gpio; do
    if getent group "$group" >/dev/null; then
        usermod -a -G "$group" hermes-gadget
    fi
done
install -d -m 700 -o hermes-gadget -g hermes-gadget /var/lib/hermes-gadget
install -d -m 750 -o root -g hermes-gadget /etc/hermes-gadget
if [ ! -e /etc/hermes-gadget/config.json ]; then
    printf '%s\n' '{"server":"ws://127.0.0.1:8765/gadget","name":"Pi Gadget"}' > /etc/hermes-gadget/config.json
    chown root:hermes-gadget /etc/hermes-gadget/config.json
    chmod 640 /etc/hermes-gadget/config.json
fi

# Stop only after the new environment has installed and loaded successfully.
was_active=false
if systemctl is-active --quiet hermes-gadget.service; then
    was_active=true
    systemctl stop hermes-gadget.service
fi
if [ -L /opt/hermes-gadget/current ]; then
    previous=$(readlink -f /opt/hermes-gadget/current)
    if [ "$previous" != "$destination" ]; then
        ln -sfn "$previous" /opt/hermes-gadget/previous
    fi
fi
rm -f /opt/hermes-gadget/current.new
ln -s "$destination" /opt/hermes-gadget/current.new
mv -Tf /opt/hermes-gadget/current.new /opt/hermes-gadget/current
install -m 644 "$source_dir/hermes-gadget.service" /etc/systemd/system/hermes-gadget.service
install -m 755 "$source_dir/hermes-gadget-device" /usr/local/bin/hermes-gadget-device
systemctl daemon-reload
if [ "$was_active" = true ]; then
    systemctl start hermes-gadget.service
fi
echo "Installed $release_id. Configuration and device identity were preserved."
echo 'Edit /etc/hermes-gadget/config.json, then run: sudo systemctl enable --now hermes-gadget'
