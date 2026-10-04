"""Expose one configured Home Assistant light and Celsius sensor to Hermes."""

import http.client
import json
import os
import re
from urllib.parse import urlsplit

from bridge import parser, run, temperature


class HomeAssistant:
    def __init__(self, url, token, lamp, sensor):
        self.url = urlsplit(url)
        if (self.url.scheme not in {"http", "https"} or not self.url.hostname or
                self.url.username or self.url.password or self.url.path not in {"", "/"} or
                self.url.query or self.url.fragment):
            raise ValueError("HA_URL must be an http(s) origin without credentials or a path")
        if not token or any(ord(c) < 32 or ord(c) > 126 for c in token):
            raise ValueError("HA_TOKEN must contain a valid access token")
        if not re.fullmatch(r"light\.[a-z0-9_]+", lamp):
            raise ValueError("--lamp must name one light entity")
        if not re.fullmatch(r"sensor\.[a-z0-9_]+", sensor):
            raise ValueError("--temperature must name one sensor entity")
        self.token, self.lamp, self.sensor = token, lamp, sensor

    def request(self, method, path, body=None):
        connection = (http.client.HTTPSConnection if self.url.scheme == "https" else
                      http.client.HTTPConnection)(self.url.hostname, self.url.port, timeout=5)
        try:
            connection.request(method, path, json.dumps(body) if body is not None else None,
                               {"Authorization": "Bearer " + self.token, "Content-Type": "application/json"})
            response = connection.getresponse()
            if response.status != 200:
                raise RuntimeError("Home Assistant request failed")
            data = response.read(65537)
            if len(data) > 65536:
                raise ValueError("Home Assistant response is too large")
            return json.loads(data)
        finally:
            connection.close()

    def read_temperature(self):
        state = self.request("GET", "/api/states/" + self.sensor)
        if not isinstance(state, dict) or not isinstance(state.get("attributes"), dict):
            return None
        if state["attributes"].get("unit_of_measurement") != "°C":
            return None
        return temperature(state.get("state"))

    def set_lamp(self, on):
        self.request("POST", "/api/services/light/" + ("turn_on" if on else "turn_off"),
                     {"entity_id": self.lamp})
        return {"service_executed": True, "requested_on": on}

    def close(self):
        pass


def main():
    cli = parser(__doc__)
    cli.add_argument("--lamp", required=True)
    cli.add_argument("--temperature", required=True)
    args = cli.parse_args()
    try:
        backend = HomeAssistant(os.environ.get("HA_URL", ""), os.environ.get("HA_TOKEN", ""),
                                args.lamp, args.temperature)
        run(args, backend)
    except (ValueError, OSError, RuntimeError) as exc:
        cli.exit(1, f"Home Assistant example: {exc}\n")


if __name__ == "__main__":
    main()
