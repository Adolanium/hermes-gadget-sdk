"""Expose one MQTT lamp command topic and a numeric Celsius sensor to Hermes."""

import os
import threading
import time

import paho.mqtt.client as mqtt

from bridge import parser, run, temperature


class Mqtt:
    def __init__(self, host, port, lamp, sensor, *, tls=True, username=None, password=None):
        if not host or not 1 <= port <= 65535:
            raise ValueError("a broker host and valid port are required")
        for topic in (lamp, sensor):
            if not topic or len(topic.encode()) > 256 or any(c in topic for c in "#+\x00"):
                raise ValueError("topics must be exact names of 1 to 256 bytes, without wildcards")
        if lamp == sensor:
            raise ValueError("lamp and temperature topics must differ")
        self.host, self.port, self.lamp, self.sensor = host, port, lamp, sensor
        self.tls, self.username, self.password = tls, username, password
        self.lock = threading.Lock()
        self.sample = (None, 0.0)
        self.client = self.new_client(reconnect=True)
        self.client.on_connect = self.connected
        self.client.on_disconnect = self.disconnected
        self.client.on_message = self.message
        self.client.connect_async(host, port, keepalive=30)
        self.client.loop_start()

    def new_client(self, *, reconnect):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, reconnect_on_failure=reconnect)
        client.connect_timeout = 3
        client.max_queued_messages_set(1)
        if self.username:
            client.username_pw_set(self.username, self.password)
        if self.tls:
            client.tls_set()  # System CAs and hostname verification.
        return client

    def connected(self, client, userdata, flags, reason, properties):
        if not reason.is_failure:
            client.subscribe(self.sensor, qos=0)

    def disconnected(self, client, userdata, flags, reason, properties):
        with self.lock:
            self.sample = (None, 0.0)

    def message(self, client, userdata, message):
        if message.topic != self.sensor:
            return
        value = None
        # Retained numeric payloads have no timestamp, so their freshness is unknown.
        if not message.retain and len(message.payload) <= 128:
            try:
                value = temperature(message.payload.decode("ascii"))
            except UnicodeError:
                pass
        with self.lock:
            self.sample = (value, time.monotonic())

    def read_temperature(self):
        with self.lock:
            value, received = self.sample
        return value if self.client.is_connected() and time.monotonic() - received <= 60 else None

    def set_lamp(self, on):
        # A short-lived connection prevents a timed-out command from being
        # replayed on a later reconnect. ON/OFF commands are idempotent.
        client = self.new_client(reconnect=False)
        try:
            client.connect(self.host, self.port, keepalive=10)
            deadline = time.monotonic() + 5
            while not client.is_connected():
                if time.monotonic() >= deadline or client.loop(timeout=0.1) != mqtt.MQTT_ERR_SUCCESS:
                    raise RuntimeError("broker connection failed")
            info = client.publish(self.lamp, "ON" if on else "OFF", qos=1, retain=False)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise RuntimeError("broker rejected the command")
            while not info.is_published():
                if time.monotonic() >= deadline or client.loop(timeout=0.1) != mqtt.MQTT_ERR_SUCCESS:
                    raise RuntimeError("broker acknowledgement timed out")
            return {"broker_acknowledged": True, "requested_on": on}
        finally:
            client.disconnect()
            client.loop(timeout=0.1)

    def close(self):
        self.client.disconnect()
        self.client.loop_stop()


def main():
    cli = parser(__doc__)
    cli.add_argument("--host", required=True)
    cli.add_argument("--port", type=int, default=8883)
    cli.add_argument("--plain", action="store_true", help="Use plain MQTT on a trusted local network")
    cli.add_argument("--lamp-topic", required=True)
    cli.add_argument("--temperature-topic", required=True)
    args = cli.parse_args()
    try:
        backend = Mqtt(args.host, args.port, args.lamp_topic, args.temperature_topic, tls=not args.plain,
                       username=os.environ.get("MQTT_USERNAME"), password=os.environ.get("MQTT_PASSWORD"))
        run(args, backend)
    except (ValueError, OSError, RuntimeError) as exc:
        cli.exit(1, f"MQTT example: {exc}\n")


if __name__ == "__main__":
    main()
