// The virtual gadget: a Hermes Gadget device that lives in the Desktop page. It speaks the same
// protocol as the firmware (docs/protocol.md): hello, challenge, auth with its own key, pairing,
// typed and spoken messages, streamed replies, spoken audio, cards, questions and actions. It knows
// nothing about React; the page renders its state and feeds it input.

export const SUBPROTOCOL = 'hermes-gadget.v1'
const AUTH_CONTEXT = 'hermes-gadget/v1|'
const CH_AUDIO = 0x01
const CH_IMAGE = 0x02
export const MIC_RATE = 16000
export const SPEAKER_RATE = 16000
export const SCREEN = 466 // a round 466 px panel, like the 1.43" AMOLED boards
export const IMAGE_PX = 300

const b64 = bytes => btoa(String.fromCharCode(...bytes))
const unb64 = text => Uint8Array.from(atob(text), c => c.charCodeAt(0))
const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('')

/** A new random 32-byte device key, base64. */
export function newKey(cryptoImpl = globalThis.crypto) {
  return b64(cryptoImpl.getRandomValues(new Uint8Array(32)))
}

/** device_id = "hg-" + hex(sha256(key))[0:16], as the firmware derives it. */
export async function deviceIdFor(keyB64, subtle = globalThis.crypto.subtle) {
  const digest = new Uint8Array(await subtle.digest('SHA-256', unb64(keyB64)))
  return 'hg-' + hex(digest).slice(0, 16)
}

/** HMAC-SHA256(key, "hermes-gadget/v1|" + id + "|" + nonce), base64: the auth answer. */
export async function authMac(keyB64, deviceId, nonce, subtle = globalThis.crypto.subtle) {
  const key = await subtle.importKey('raw', unb64(keyB64), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign'])
  const sig = await subtle.sign('HMAC', key, new TextEncoder().encode(`${AUTH_CONTEXT}${deviceId}|${nonce}`))
  return b64(new Uint8Array(sig))
}

/** A binary frame: channel, stream, little-endian sequence number, payload. */
export function frame(channel, stream, seq, payload) {
  const out = new Uint8Array(4 + payload.byteLength)
  out[0] = channel
  out[1] = stream & 0xff
  out[2] = seq & 0xff
  out[3] = (seq >> 8) & 0xff
  out.set(new Uint8Array(payload.buffer ?? payload, payload.byteOffset ?? 0, payload.byteLength), 4)
  return out
}

export function parseFrame(data) {
  const bytes = new Uint8Array(data)
  if (bytes.length < 4) return null
  return { channel: bytes[0], stream: bytes[1], seq: bytes[2] | (bytes[3] << 8), payload: bytes.subarray(4) }
}

/** RGB565 little-endian pixels -> RGBA for a canvas ImageData. */
export function rgb565ToRgba(bytes, out = new Uint8ClampedArray((bytes.length >> 1) * 4)) {
  for (let i = 0, j = 0; i + 1 < bytes.length; i += 2, j += 4) {
    const v = bytes[i] | (bytes[i + 1] << 8)
    out[j] = ((v >> 11) & 0x1f) * 255 / 31
    out[j + 1] = ((v >> 5) & 0x3f) * 255 / 63
    out[j + 2] = (v & 0x1f) * 255 / 31
    out[j + 3] = 255
  }
  return out
}

/** The device's actions: what the agent can do with it (gadget_action). */
export const ACTIONS = [
  {
    name: 'light.set',
    description: "Set the colour of the virtual gadget's glow, around its screen. Use a CSS colour name or #rrggbb, or 'off'.",
    params: { type: 'object', properties: { color: { type: 'string' } }, required: ['color'] }
  },
  {
    name: 'timer.start',
    description: 'Start a countdown timer on the virtual gadget. It shows on the screen and chimes at zero.',
    params: { type: 'object', properties: { seconds: { type: 'integer' }, label: { type: 'string' } }, required: ['seconds'] }
  }
]

export const INITIAL = {
  conn: 'offline', // offline | connecting | online | error
  error: null,
  paired: false,
  pairingCode: null,
  state: 'ready', // ready | listening | thinking | speaking
  heard: '',
  reply: '',
  status: '',
  notice: null,
  card: null, // { title, body, until }
  image: null, // { width, height, rgba, until }
  prompt: null, // { id, title, text }
  light: null,
  timer: null // { label, endsAt }
}

/**
 * Create a virtual gadget. `opts`: { url, name, key, token, WebSocket, onChange, onAudio, onAudioEnd, now }.
 * `onChange(state)` gets a fresh state object after every change.
 */
export function createDevice(opts) {
  const WS = opts.WebSocket ?? globalThis.WebSocket
  const now = opts.now ?? (() => Date.now())
  let ws = null
  let deviceId = null
  let state = { ...INITIAL }
  let msgSeq = 0
  let streamSeq = 1
  let upload = null // { id, stream, seq }
  const images = new Map() // stream -> { width, height, ttl, chunks, size }
  let closedByUser = false
  let retry = null

  const set = patch => {
    state = { ...state, ...patch }
    opts.onChange?.(state)
  }
  const send = obj => {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj))
  }

  async function connect() {
    closedByUser = false
    clearTimeout(retry)
    deviceId = deviceId ?? (await deviceIdFor(opts.key))
    if (closedByUser) return // disconnected while the key was hashing: open nothing
    set({ conn: 'connecting', error: null })
    try {
      ws = new WS(opts.url, SUBPROTOCOL)
    } catch (e) {
      set({ conn: 'error', error: String(e?.message ?? e) })
      return
    }
    ws.binaryType = 'arraybuffer'
    ws.onopen = () => {
      send({
        type: 'hello',
        proto: 1,
        device_id: deviceId,
        name: opts.name ?? 'Desktop Gadget',
        board: 'hermes-desktop',
        firmware: opts.version ?? '0.5.0',
        ...(opts.token ? { token: opts.token } : {}),
        caps: {
          display: { width: SCREEN, height: SCREEN, color: true, charset: 'latin1-cyrillic', text_cols: 30, text_rows: 9, image: { width: IMAGE_PX, height: IMAGE_PX, format: 'rgb565' } },
          mic: { rate: MIC_RATE, format: 'pcm16' },
          speaker: { rate: SPEAKER_RATE, format: 'pcm16', formats: ['pcm16'], buffer_ms: 2000 },
          inputs: ['talk', 'cancel'],
          talk_mode: 'hold'
        },
        actions: ACTIONS
      })
    }
    ws.onmessage = ev => {
      if (typeof ev.data === 'string') {
        let msg
        try {
          msg = JSON.parse(ev.data)
        } catch {
          return
        }
        void handle(msg)
      } else onBinary(ev.data)
    }
    ws.onclose = ev => {
      ws = null
      upload = null
      if (closedByUser) set({ conn: 'offline' })
      else {
        set({ conn: 'error', error: state.error ?? (ev.reason || 'The gateway closed the connection.'), state: 'ready' })
        retry = setTimeout(connect, 4000)
      }
    }
    ws.onerror = () => set({ error: state.error ?? "Can't reach the Gadget platform on this Hermes." })
  }

  async function handle(msg) {
    switch (msg.type) {
      case 'challenge':
        if (msg.enrolled) send({ type: 'auth', mac: await authMac(opts.key, deviceId, String(msg.nonce)) })
        else send({ type: 'auth', key: opts.key })
        break
      case 'welcome':
        set({ conn: 'online', error: null, paired: !!msg.paired, pairingCode: msg.paired ? null : state.pairingCode })
        break
      case 'pairing':
        set({ paired: false, pairingCode: String(msg.code ?? '') })
        break
      case 'paired':
        set({ paired: true, pairingCode: null, notice: { text: 'Paired with Hermes', until: now() + 4000 } })
        break
      case 'unpaired':
        set({ paired: false })
        break
      case 'turn.start':
        set({ state: 'thinking', reply: '', status: '' })
        break
      case 'transcript':
        set({ heard: String(msg.text ?? '') })
        break
      case 'status':
        set({ status: String(msg.text ?? '') })
        break
      case 'reply.delta':
        set({ reply: String(msg.text ?? '') })
        break
      case 'reply':
        if (!msg.interim) set({ reply: String(msg.text ?? '') })
        else set({ status: String(msg.text ?? '') })
        break
      case 'turn.end':
        set({ state: state.state === 'speaking' ? 'speaking' : 'ready', status: '' })
        break
      case 'audio.start':
        set({ state: 'speaking' })
        opts.onAudio?.({ stream: msg.stream, rate: msg.rate ?? SPEAKER_RATE, start: true })
        break
      case 'audio.end':
        opts.onAudioEnd?.({ stream: msg.stream, aborted: false })
        break
      case 'audio.abort':
        opts.onAudioEnd?.({ stream: msg.stream, aborted: true })
        set({ state: 'ready' })
        break
      case 'notice':
        set({ notice: { text: String(msg.text ?? ''), until: now() + 1000 * (Number(msg.ttl_s) || 8) } })
        break
      case 'display': {
        const ttl = Number(msg.ttl_s ?? 15)
        set({ card: { title: String(msg.title ?? ''), body: String(msg.body ?? ''), until: ttl > 0 ? now() + ttl * 1000 : null }, image: null })
        break
      }
      case 'image.start':
        images.set(msg.stream, { width: msg.width, height: msg.height, ttl: Number(msg.ttl_s ?? 30), chunks: [], size: 0 })
        break
      case 'image.end': {
        const img = images.get(msg.stream)
        images.delete(msg.stream)
        if (!img) break
        const bytes = new Uint8Array(img.size)
        let at = 0
        for (const c of img.chunks) {
          bytes.set(c, at)
          at += c.length
        }
        if (bytes.length >= img.width * img.height * 2)
          set({ image: { width: img.width, height: img.height, rgba: rgb565ToRgba(bytes), until: img.ttl > 0 ? now() + img.ttl * 1000 : null }, card: null })
        break
      }
      case 'prompt':
        set({ prompt: { id: String(msg.id), title: String(msg.title ?? ''), text: String(msg.text ?? '') } })
        break
      case 'prompt.close':
        if (state.prompt?.id === String(msg.id)) set({ prompt: null })
        break
      case 'action':
        runAction(msg)
        break
      case 'ping':
        send({ type: 'pong', ts: msg.ts })
        break
      case 'error':
        set({ error: String(msg.message ?? msg.code ?? 'error') })
        break
      default:
        break // receivers ignore unknown types (protocol: Versioning)
    }
  }

  function onBinary(data) {
    const f = parseFrame(data)
    if (!f) return
    if (f.channel === CH_AUDIO) opts.onAudio?.({ stream: f.stream, pcm: f.payload })
    else if (f.channel === CH_IMAGE) {
      const img = images.get(f.stream)
      if (img && img.size < IMAGE_PX * IMAGE_PX * 2) {
        img.chunks.push(f.payload.slice())
        img.size += f.payload.length
      }
    }
  }

  function runAction(msg) {
    const reply = (ok, extra) => send({ type: 'action.result', id: msg.id, ok, ...extra })
    const args = msg.args && typeof msg.args === 'object' ? msg.args : {}
    if (msg.name === 'light.set') {
      const color = String(args.color ?? '').trim().slice(0, 32)
      if (!color) return reply(false, { error: 'color is required' })
      set({ light: color === 'off' ? null : color })
      return reply(true, { result: { light: color } })
    }
    if (msg.name === 'timer.start') {
      const seconds = Math.round(Number(args.seconds))
      if (!(seconds > 0 && seconds <= 24 * 3600)) return reply(false, { error: 'seconds must be between 1 and 86400' })
      set({ timer: { label: String(args.label ?? '').slice(0, 40), endsAt: now() + seconds * 1000 } })
      return reply(true, { result: { seconds } })
    }
    return reply(false, { error: `unknown action ${msg.name}` })
  }

  return {
    get state() {
      return state
    },
    get id() {
      return deviceId
    },
    connect,
    disconnect() {
      closedByUser = true
      clearTimeout(retry)
      const old = ws
      ws = null
      upload = null
      if (old) {
        // Detach first: a late event from the old socket must not change this gadget's state.
        old.onopen = old.onmessage = old.onerror = old.onclose = null
        old.close()
      }
      set({ conn: 'offline' })
    },
    sendText(text) {
      const t = String(text ?? '').trim().slice(0, 4000)
      if (!t || !state.paired) return false
      set({ state: 'thinking', heard: t, reply: '', status: '', card: null, image: null })
      send({ type: 'text', id: `t${++msgSeq}`, text: t })
      return true
    },
    /** Hold to talk: start an utterance; feed PCM16 with talkData; release with talkEnd. */
    talkStart() {
      if (!state.paired || upload) return false
      upload = { id: `a${++msgSeq}`, stream: (streamSeq = (streamSeq % 250) + 1), seq: 0, started: now() }
      opts.onStopAudio?.()
      set({ state: 'listening', heard: '', reply: '', status: '', card: null, image: null })
      send({ type: 'audio.start', id: upload.id, stream: upload.stream, rate: MIC_RATE, format: 'pcm16', mode: 'hold' })
      return true
    },
    talkData(pcm16) {
      if (!upload || !ws || ws.readyState !== 1) return
      ws.send(frame(CH_AUDIO, upload.stream, upload.seq++, pcm16))
    },
    talkEnd() {
      if (!upload) return
      const up = upload
      upload = null
      const ms = now() - up.started
      if (ms < 300) {
        send({ type: 'audio.cancel', id: up.id, stream: up.stream, reason: 'too short' })
        set({ state: 'ready', notice: { text: 'Hold to talk, then release', until: now() + 3000 } })
        return
      }
      set({ state: 'thinking' })
      send({ type: 'audio.end', id: up.id, stream: up.stream, duration_ms: ms })
    },
    cancel() {
      opts.onStopAudio?.()
      send({ type: 'cancel' })
      set({ state: 'ready', status: '' })
    },
    newSession() {
      send({ type: 'session.new' })
    },
    answer(yes) {
      if (!state.prompt) return
      send({ type: 'prompt.reply', id: state.prompt.id, answer: yes ? 'yes' : 'no' })
      set({ prompt: null })
    },
    audioDone() {
      if (state.state === 'speaking') set({ state: 'ready' })
    },
    dismiss() {
      set({ card: null, image: null, notice: null })
    },
    clearTimer() {
      set({ timer: null })
    }
  }
}
