// Generated from the same HTML exercised by the browser tests.
#pragma once
constexpr char setup_page[] = R"hgsetup(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connect your Hermes gadget</title>
<style>
:root{color-scheme:dark;font:16px/1.5 system-ui,sans-serif;background:#101318;color:#eef1f4}
body{max-width:30rem;margin:3rem auto;padding:0 1.25rem}h1{font-size:2rem;line-height:1.15}
p{color:#bac3cc}label{display:block;margin-top:1.2rem;font-weight:600}
input,button{box-sizing:border-box;width:100%;padding:.8rem;border-radius:.6rem;font:inherit}
input{background:#1b222b;color:inherit;border:1px solid #596574;margin:.35rem 0}
button{margin-top:1.4rem;background:#aadcc5;color:#10291d;border:0;font-weight:700;cursor:pointer}
button:disabled{opacity:.55;cursor:wait}small{display:block;color:#bac3cc}
:focus-visible{outline:3px solid #aadcc5;outline-offset:3px}#status{min-height:3rem}
</style>
<main>
<p>HERMES GADGET</p>
<h1>Connect to your Wi-Fi</h1>
<p>Use a 2.4 GHz network. Your gadget saves these settings after it connects. Keep this page open while it checks.</p>
<form id="setup">
<label for="ssid">Wi-Fi network name</label><input id="ssid" required maxlength="32" autocomplete="off">
<label for="password">Wi-Fi password</label><input id="password" type="password" maxlength="64" autocomplete="new-password">
<small>Leave empty for an open network. Saved passwords are never shown here.</small>
<label for="server">Hermes gateway address</label><input id="server" required maxlength="200" placeholder="ws://192.168.1.20:8765/gadget" inputmode="url" autocomplete="off">
<small>Use the address shown by the Hermes gadget setup command.</small>
<button id="connect" disabled>Check connection and save</button>
</form>
<p id="status" role="status" aria-live="polite">Loading device settings...</p>
<small>This temporary network closes after setup or ten minutes. Press CANCEL on the gadget to leave. USB setup is always available.</small>
</main>
<script>
const form = document.querySelector('#setup');
const status = document.querySelector('#status');
const button = document.querySelector('#connect');
const ssid = document.querySelector('#ssid');
const password = document.querySelector('#password');
const server = document.querySelector('#server');
let nonce = '', waiting = false;
async function readState(initial = false) {
  try {
    const response = await fetch('/api/setup', {cache:'no-store'});
    if (!response.ok) throw new Error();
    const data = await response.json();
    nonce = data.nonce;
    if (initial) { ssid.value = data.ssid; server.value = data.server; }
    if (data.state === 'connected') {
      waiting = false; button.disabled = true;
      status.textContent = 'Wi-Fi connected and settings saved. Reconnect your phone to your usual network. The gadget will now contact Hermes.';
    } else if (data.state === 'failed') {
      waiting = false; button.disabled = false;
      status.textContent = 'Could not connect. Previous settings were kept. Check the network name and password, then try again.';
    } else if (data.state === 'joining') {
      waiting = true; button.disabled = true;
      status.textContent = 'Checking Wi-Fi. This can take 30 seconds. If your phone disconnects, rejoin the gadget network.';
    } else {
      button.disabled = false; status.textContent = 'Ready to connect.';
    }
  } catch {
    status.textContent = 'Cannot reach the gadget. Check its screen and rejoin its temporary Wi-Fi network. Reload this page after reconnecting.';
  }
  if (waiting) setTimeout(() => readState(), 1500);
}
form.addEventListener('submit', async event => {
  event.preventDefault(); button.disabled = true;
  try {
    const response = await fetch('/api/setup', {method:'POST', headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:ssid.value,password:password.value,server:server.value,nonce})});
    const data = await response.json();
    if (!response.ok) { status.textContent = data.error; button.disabled = false; return; }
    password.value = ''; waiting = true; await readState();
  } catch { status.textContent = 'Connection interrupted. Rejoin the gadget network and reload this page.'; button.disabled = false; }
});
readState(true);
</script>
</html>
)hgsetup";
