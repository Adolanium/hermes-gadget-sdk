// The Desktop's virtual gadget against the protocol: identity and auth vectors match the Python
// server (plugin/protocol.py), and a full handshake, a typed turn and an action work over a fake socket.
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { authMac, createDevice, deviceIdFor, frame, parseFrame, rgb565ToRgba } from '../src/device.js'

// bytes 0..31; the same key and nonce give these values in plugin/protocol.py
const KEY = 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8='

test('device id and auth MAC match the server', async () => {
  assert.equal(await deviceIdFor(KEY), 'hg-630dcd2966c43366')
  assert.equal(await authMac(KEY, 'hg-630dcd2966c43366', 'bm9uY2Ux'), 'ldURlmsN/WvXFaeRU14c4QgVEKaaQ7y2Ho/9SG0DGHE=')
})

test('binary frames carry channel, stream and a little-endian sequence', () => {
  const f = frame(1, 7, 0x1234, new Uint8Array([9, 8]))
  assert.deepEqual([...f], [1, 7, 0x34, 0x12, 9, 8])
  const back = parseFrame(f.buffer)
  assert.deepEqual([back.channel, back.stream, back.seq, [...back.payload]], [1, 7, 0x1234, [9, 8]])
})

test('RGB565 converts to RGBA', () => {
  assert.deepEqual([...rgb565ToRgba(new Uint8Array([0x00, 0xf8, 0xe0, 0x07]))], [255, 0, 0, 255, 0, 255, 0, 255])
})

class FakeSocket {
  static last = null
  constructor(url, proto) {
    this.url = url
    this.proto = proto
    this.sent = []
    this.readyState = 1
    FakeSocket.last = this
    queueMicrotask(() => this.onopen?.())
  }
  send(data) {
    this.sent.push(typeof data === 'string' ? JSON.parse(data) : data)
  }
  close() {
    this.readyState = 3
    this.onclose?.({})
  }
  serve(msg) {
    this.onmessage?.({ data: JSON.stringify(msg) })
  }
}

const tick = () => new Promise(r => setTimeout(r, 5))

test('handshake, pairing, a typed turn and an action', async () => {
  const states = []
  const dev = createDevice({ url: 'ws://127.0.0.1:8765/gadget', key: KEY, WebSocket: FakeSocket, onChange: s => states.push(s) })
  await dev.connect()
  await tick()
  const ws = FakeSocket.last
  assert.equal(ws.proto, 'hermes-gadget.v1')
  const hello = ws.sent[0]
  assert.equal(hello.type, 'hello')
  assert.equal(hello.device_id, 'hg-630dcd2966c43366')
  assert.deepEqual(hello.actions.map(a => a.name), ['light.set', 'timer.start'])

  ws.serve({ type: 'challenge', nonce: 'bm9uY2Ux', enrolled: false })
  await tick()
  assert.deepEqual(ws.sent[1], { type: 'auth', key: KEY })
  ws.serve({ type: 'welcome', paired: false })
  ws.serve({ type: 'pairing', code: 'ABCD2345', command: 'hermes pairing approve gadget ABCD2345' })
  assert.equal(dev.state.pairingCode, 'ABCD2345')
  assert.equal(dev.sendText('hi'), false, 'nothing is sent before pairing')
  ws.serve({ type: 'paired' })
  assert.equal(dev.state.paired, true)

  assert.equal(dev.sendText('Hey Hermes'), true)
  assert.deepEqual(ws.sent.at(-1), { type: 'text', id: 't1', text: 'Hey Hermes' })
  ws.serve({ type: 'turn.start', turn: 'x' })
  ws.serve({ type: 'reply.delta', turn: 'x', text: 'Hel' })
  assert.equal(dev.state.state, 'thinking')
  ws.serve({ type: 'reply', turn: 'x', text: 'Hello!' })
  ws.serve({ type: 'turn.end', turn: 'x', outcome: 'success' })
  assert.equal(dev.state.reply, 'Hello!')
  assert.equal(dev.state.state, 'ready')

  ws.serve({ type: 'action', id: 'x1', name: 'light.set', args: { color: 'teal' } })
  assert.equal(dev.state.light, 'teal')
  assert.deepEqual(ws.sent.at(-1), { type: 'action.result', id: 'x1', ok: true, result: { light: 'teal' } })
  ws.serve({ type: 'action', id: 'x2', name: 'nope', args: {} })
  assert.equal(ws.sent.at(-1).ok, false)

  ws.serve({ type: 'challenge', nonce: 'bm9uY2Ux', enrolled: true })
  await tick()
  assert.deepEqual(ws.sent.at(-1), { type: 'auth', mac: 'ldURlmsN/WvXFaeRU14c4QgVEKaaQ7y2Ho/9SG0DGHE=' })
  ws.serve({ type: 'ping', ts: 5 })
  assert.deepEqual(ws.sent.at(-1), { type: 'pong', ts: 5 })
  dev.disconnect()
  assert.equal(dev.state.conn, 'offline')
})

test('hold to talk sends an utterance; a tap is cancelled', async () => {
  let t = 1000
  const dev = createDevice({ url: 'ws://x', key: KEY, WebSocket: FakeSocket, now: () => t })
  await dev.connect()
  await tick()
  const ws = FakeSocket.last
  ws.serve({ type: 'welcome', paired: true })
  assert.equal(dev.talkStart(), true)
  const start = ws.sent.at(-1)
  assert.equal(start.type, 'audio.start')
  dev.talkData(new Int16Array([1, 2, 3]))
  assert.deepEqual([...ws.sent.at(-1)].slice(0, 4), [1, start.stream, 0, 0])
  t += 1500
  dev.talkEnd()
  assert.equal(ws.sent.at(-1).type, 'audio.end')
  assert.equal(dev.state.state, 'thinking')
  dev.talkStart()
  t += 100
  dev.talkEnd()
  assert.equal(ws.sent.at(-1).type, 'audio.cancel')
  dev.disconnect()
})
