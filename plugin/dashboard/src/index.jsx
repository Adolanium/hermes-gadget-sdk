// The Gadgets tab of the Hermes web dashboard: the devices this Hermes knows, read-only.
//
// Source of dist/index.js. Build it with `npm run build:dashboard-tab` in site/ (esbuild, React
// from the dashboard's plugin SDK, IIFE output); site's `npm test` fails when dist/index.js is
// older than this file. React and the UI components come from window.__HERMES_PLUGIN_SDK__.

const SDK = window.__HERMES_PLUGIN_SDK__;
const React = SDK.React;
const { useCallback, useEffect, useState } = SDK.hooks;
const { Badge, Button, Card, CardContent, CardHeader, CardTitle } = SDK.components;
const { timeAgo } = SDK.utils;

const API = "/api/plugins/gadget";
const REFRESH_MS = 10000;

function errorText(err) {
  return String((err && err.message) || err).replace(/^Error:\s*/, "");
}

function when(ts) {
  if (typeof ts !== "number") return "-";
  return timeAgo(ts);
}

function fullDate(ts) {
  return typeof ts === "number" ? new Date(ts * 1000).toLocaleString() : "";
}

// Newer dashboards style a Badge by `tone`, older ones by `variant`; each ignores the other.
function Status({ device }) {
  if (device.online) return <Badge tone="success" variant="default">Online</Badge>;
  if (device.pending) return <Badge tone="warning" variant="outline">Waiting to pair</Badge>;
  return <Badge tone="outline" variant="secondary">Offline</Badge>;
}

function Battery({ battery }) {
  if (!battery) return <span className="text-muted-foreground">-</span>;
  const parts = [];
  if (typeof battery.battery_percent === "number") parts.push(`${Math.round(battery.battery_percent)}%`);
  else if (typeof battery.battery_mv === "number") parts.push(`${(battery.battery_mv / 1000).toFixed(2)} V`);
  if (battery.charging) parts.push("charging");
  else if (battery.external_power) parts.push("on USB");
  if (battery.battery_present === false && !parts.length) parts.push("no battery");
  return <span>{parts.join(", ") || "-"}</span>;
}

function GatewayNote({ gateway, count }) {
  if (gateway.running || !count) return null;
  const since = typeof gateway.updated_at === "number" ? ` Its last report was ${when(gateway.updated_at)}.` : "";
  return (
    <p className="text-sm text-muted-foreground">
      The gateway is not running, or has not reported yet, so online state and battery are unknown.{since}
      {" "}Start it with <code>hermes gateway run</code>.
    </p>
  );
}

function DeviceTable({ devices }) {
  const cell = "px-3 py-2 text-left align-top";
  return (
    <div className="overflow-x-auto">
      <table className="w-full text-sm">
        <thead className="text-xs uppercase text-muted-foreground">
          <tr>
            <th className={cell}>Device</th>
            <th className={cell}>Status</th>
            <th className={cell}>Battery</th>
            <th className={cell}>Board</th>
            <th className={cell}>Firmware</th>
            <th className={cell}>Last seen</th>
          </tr>
        </thead>
        <tbody>
          {devices.map((d) => (
            <tr key={d.id} className="border-t border-border">
              <td className={cell}>
                <div className="font-medium">{d.name || d.id}</div>
                <div className="font-mono text-xs text-muted-foreground">{d.id}</div>
              </td>
              <td className={cell}><Status device={d} /></td>
              <td className={cell}><Battery battery={d.online ? d.battery : null} /></td>
              <td className={cell}>{d.board || "-"}</td>
              <td className={cell + " font-mono"}>{d.firmware || "-"}</td>
              <td className={cell} title={fullDate(d.online ? d.connected_at : d.last_seen)}>
                {d.online ? "now" : when(d.last_seen)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function GadgetsPage() {
  const [data, setData] = useState(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);

  const load = useCallback(() => {
    setLoading(true);
    SDK.fetchJSON(`${API}/devices`)
      .then((d) => { setData(d); setError(""); })
      .catch((e) => setError(errorText(e)))
      .finally(() => setLoading(false));
  }, []);

  useEffect(() => {
    load();
    const timer = setInterval(load, REFRESH_MS);
    return () => clearInterval(timer);
  }, [load]);

  const devices = (data && data.devices) || [];
  const online = devices.filter((d) => d.online).length;

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader>
          <div className="flex items-center justify-between gap-2">
            <CardTitle>Gadgets</CardTitle>
            <Button variant="outline" size="sm" onClick={load} disabled={loading}>Refresh</Button>
          </div>
          {data && (
            <p className="text-sm text-muted-foreground">
              {devices.length} known, {online} online.
            </p>
          )}
        </CardHeader>
        <CardContent>
          <div className="flex flex-col gap-3">
            {error && <p className="text-sm text-destructive">Could not load the devices: {error}</p>}
            {data && <GatewayNote gateway={data.gateway} count={devices.length} />}
            {data && !devices.length && (
              <p className="text-sm text-muted-foreground">
                No gadgets have connected yet. Set up a board from the installer, then approve it
                with <code>hermes gadget pair</code>.
              </p>
            )}
            {devices.length > 0 && <DeviceTable devices={devices} />}
          </div>
        </CardContent>
      </Card>
    </div>
  );
}

window.__HERMES_PLUGINS__.register("gadget", GadgetsPage);
