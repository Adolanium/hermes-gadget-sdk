#!/bin/sh
# Run only in a disposable CI container, never on a user's installed device.
set -eu
if [ ! -f /.dockerenv ]; then
    echo 'The install test requires a disposable Docker container.' >&2
    exit 1
fi
archive=$1
apt-get update -qq
apt-get install -y --no-install-recommends python3-venv libportaudio2 ca-certificates
mkdir /package
tar -xzf "$archive" -C /package
cd /package/hermes-gadget-*-linux-*
# The container has no init system. Record the calls; service behavior is tested separately.
mkdir /test-bin
cat > /test-bin/systemctl <<'SCRIPT'
#!/bin/sh
echo "$*" >> /systemctl.log
if [ "$1" = is-active ]; then exit 1; fi
SCRIPT
chmod +x /test-bin/systemctl
export PATH="/test-bin:$PATH"
sh install.sh
cli=/opt/hermes-gadget/current/venv/bin/hermes-gadget
export HGSIM_LIBRARY=/opt/hermes-gadget/current/libhgsim.so
"$cli" --version
runuser -u hermes-gadget -- "$cli" linux --state-dir /var/lib/hermes-gadget run --config /etc/hermes-gadget/config.json &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
attempt=0
until [ -S /var/lib/hermes-gadget/control.sock ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ]
    sleep 0.1
done
hermes-gadget-device status > /status.json
python3 -c 'import json; s=json.load(open("/status.json")); assert s["board"] == "linux" and s["name"] == "Pi Gadget"'
kill "$pid"
wait "$pid" || [ "$?" -eq 143 ]
trap - EXIT
sha256sum /var/lib/hermes-gadget/device.json /etc/hermes-gadget/config.json > /saved-state.sha256
sh install.sh
sha256sum --check /saved-state.sha256
test -f /opt/hermes-gadget/current/licenses/NOTICE
test -f /opt/hermes-gadget/current/licenses/LICENSES/Apache-2.0.txt
# Install a second release ID to exercise switching while retaining the first.
python3 -c '
import json
p=json.load(open("package.json"))
p["release_id"] = p["version"] + "-ffffffffffff"
open("package.json", "w").write(json.dumps(p))
'
find . -type f ! -name SHA256SUMS -exec sha256sum {} + > /updated-checksums
mv /updated-checksums SHA256SUMS
sh install.sh
sha256sum --check /saved-state.sha256
test -L /opt/hermes-gadget/previous
test -f /opt/hermes-gadget/previous/.installed
test "$(readlink /opt/hermes-gadget/current)" != "$(readlink /opt/hermes-gadget/previous)"
echo 'Install, service startup, reinstall and release switching passed.'
