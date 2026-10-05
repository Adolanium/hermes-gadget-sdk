# Home Assistant and MQTT examples

These examples turn a Linux machine or Raspberry Pi into a paired gadget with one temperature reading and one lamp action. They use the production device core, identity storage and local controls. Each example runs as its own gadget process. No firmware or gateway changes are required.

| Example | Reads | Controls |
|---|---|---|
| [Home Assistant](../examples/automation/home_assistant.py) | One Celsius sensor entity | `turn_on` / `turn_off` for one configured light entity |
| [MQTT](../examples/automation/mqtt.py) | One topic containing a numeric Celsius value | `ON` / `OFF` on one configured command topic |

The action arguments cannot select another entity, service, broker, topic or payload. Choose a lamp for the first test. Credentials belong in environment variables, not source files or command arguments. The account's own permissions still apply; the example's fixed targets do not restrict the token outside this process.

## Prepare the Linux host

From a source checkout, follow the [Linux build instructions](linux.md#build-from-source), then activate that virtual environment. For MQTT, install the optional client:

```bash
python -m pip install -e '.[mqtt]'
```

Create `automation.json` with your Hermes gateway address:

```json
{"server":"wss://hermes.example.com/gadget","name":"Home automation"}
```

Use a private state directory, separate from any running gadget service. Run only one example against a given directory. The normal Linux `audio`, `gpio` and `display` options also work in this configuration.

## Home Assistant

Create a long-lived access token in your Home Assistant user profile. Set the origin and token in the shell that will start the example:

```bash
export HA_URL=https://home.example.com
read -r -s -p 'Home Assistant token: ' HA_TOKEN
export HA_TOKEN
printf '\n'
python examples/automation/home_assistant.py \
  --config automation.json --state-dir "$HOME/.local/state/hermes-ha" \
  --lamp light.desk --temperature sensor.room_temperature
```

Replace both entity IDs with existing entities. The temperature sensor must report `°C`. HTTP is supported for a trusted local network; HTTPS uses the system certificate store. Redirects are rejected instead of forwarding the token. The example uses Home Assistant's [REST state and service APIs](https://developers.home-assistant.io/docs/api/rest/).

## MQTT

Use a broker account that can read only the chosen sensor topic and publish only the chosen lamp topic. Supply `MQTT_USERNAME` and `MQTT_PASSWORD` through the environment when the broker requires authentication. TLS and certificate verification are enabled by default:

```bash
python examples/automation/mqtt.py \
  --config automation.json --state-dir "$HOME/.local/state/hermes-mqtt" \
  --host broker.example.com --port 8883 \
  --lamp-topic desk/lamp/set --temperature-topic room/temperature_c
```

For a local test broker without TLS, use `--host 127.0.0.1 --port 1883 --plain`. Publish a non-retained sensor reading, for example:

```bash
mosquitto_pub -h 127.0.0.1 -t room/temperature_c -m 22.5
mosquitto_sub -h 127.0.0.1 -t desk/lamp/set
```

The lamp must understand `ON` and `OFF`. Commands use QoS 1 and are never retained. Each command opens a separate connection, waits for the broker's acknowledgement, and closes it. A failed connection does not leave commands queued for a later reconnect. QoS 1 can duplicate a delivery; on/off commands must be idempotent. A broker acknowledgement does not confirm that the physical lamp changed.

The sensor connection reconnects and subscribes again after network loss. Retained numeric samples are ignored because they carry no timestamp. Send a fresh sample at least once a minute. See the [Paho client documentation](https://eclipse.dev/paho/files/paho.mqtt.python/html/client.html) for connection and acknowledgement behavior.

## Pair, read a sensor, and run an action

Keep the example running. In a second terminal, read its pairing code with the matching state directory:

```bash
hermes-gadget linux --state-dir "$HOME/.local/state/hermes-ha" status
hermes pairing approve gadget PAIRING_CODE
```

For MQTT, substitute `hermes-mqtt` for `hermes-ha`. Run the approval command on the Hermes host. Once paired, ask Hermes to list gadget devices. The example exposes:

- `temperature_c` and `temperature_available`. Read the temperature only when availability is 1. Failed requests, invalid numbers, wrong units, or expired MQTT samples set availability to 0. The last numeric reading remains present for reference.
- `lamp.set` with `{"on":true}` or `{"on":false}`. It returns `{"accepted":true,"job":"..."}` after queuing the request. Only one lamp request can be pending.
- `automation.status` with `{"job":"..."}`. It reports `pending`, `completed` or `failed` for the latest job. Completion means Home Assistant executed the service or MQTT acknowledged publication. It does not verify the physical output.

After a job finishes, the example emits `automation.completed` with `notify=false`. The event does not start a new agent turn. Poll `automation.status` to confirm the result. Jobs are kept in memory; restarting forgets them and does not replay actions. A network timeout can leave the outcome unknown, so inspect the lamp before retrying.

Temperature reads run every 30 seconds. MQTT samples expire after 60 seconds, and the next read reports their unavailability. Network work runs on bounded workers; pairing, display, local controls and the core clock continue while requests are pending.

## Test and extend

Run `pytest tests/test_automation_examples.py`. The tests use local HTTP and MQTT peers, the real Paho client, and the production gadget core. They check fixed targets, action completion, error reporting, invalid readings, and broker acknowledgements. Install the `mqtt` extra to run the MQTT tests. CI includes it.

Start with additional explicit action definitions and fixed targets. Validate each action's arguments before scheduling work. Pass readings and completion events back through the main device loop; never call the native core from a network callback. See [sensors and actions](porting.md#sensors) for the underlying API.

The examples are original project code. Paho MQTT is an optional dependency under EPL 2.0 / EDL 1.0 dual licensing. Its installed package carries the license texts; see [README](../README.md#license).
