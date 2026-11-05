// Microphone and speaker for the virtual gadget. The mic is captured at the device rate (16 kHz
// mono PCM16, in 40 ms frames, as the firmware sends it); replies arrive as PCM16 and play through
// a small scheduled queue, so they sound continuous even when frames come in bursts.

const FRAME = 640 // 40 ms at 16 kHz

/** Linear resample + float -> int16, carrying the fractional position across calls. */
export function makeDownsampler(fromRate, toRate) {
  const step = fromRate / toRate
  let pos = 0
  let last = 0
  return input => {
    const out = []
    while (pos < input.length) {
      const i = Math.floor(pos)
      const frac = pos - i
      const a = i === 0 && pos < 0 ? last : input[i]
      const b = i + 1 < input.length ? input[i + 1] : a
      const v = a + (b - a) * frac
      out.push(Math.max(-32768, Math.min(32767, Math.round(v * 32767))))
      pos += step
    }
    pos -= input.length
    last = input[input.length - 1] ?? 0
    return Int16Array.from(out)
  }
}

/** Start the microphone; `onFrame(Int16Array)` gets 40 ms frames. Resolves to a stop function. */
export async function startMic(onFrame, rate = 16000) {
  const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true } })
  let ctx = null
  let source = null
  let node = null
  try {
    ctx = new AudioContext()
    source = ctx.createMediaStreamSource(stream)
    // ScriptProcessor is deprecated but runs everywhere without a worklet module file to load.
    node = ctx.createScriptProcessor(2048, 1, 1)
  } catch (e) {
    // Setup failed after access was granted: release the microphone before reporting it.
    stream.getTracks().forEach(t => t.stop())
    void ctx?.close()
    throw e
  }
  const down = makeDownsampler(ctx.sampleRate, rate)
  let pending = new Int16Array(0)
  let level = 0
  node.onaudioprocess = e => {
    const input = e.inputBuffer.getChannelData(0)
    let peak = 0
    for (let i = 0; i < input.length; i += 8) peak = Math.max(peak, Math.abs(input[i]))
    level = peak
    const chunk = down(input)
    const merged = new Int16Array(pending.length + chunk.length)
    merged.set(pending)
    merged.set(chunk, pending.length)
    let at = 0
    while (merged.length - at >= FRAME) {
      onFrame(merged.slice(at, at + FRAME))
      at += FRAME
    }
    pending = merged.slice(at)
  }
  source.connect(node)
  node.connect(ctx.destination) // a ScriptProcessor only runs while connected; it outputs silence
  const stop = () => {
    if (pending.length) onFrame(pending)
    pending = new Int16Array(0)
    node.disconnect()
    source.disconnect()
    stream.getTracks().forEach(t => t.stop())
    void ctx.close()
  }
  stop.level = () => level
  return stop
}

/** A PCM16 player: `push(Uint8Array)` queues samples at `rate`; `onIdle` fires when it drains. */
export function createPlayer({ onIdle } = {}) {
  let ctx = null
  let at = 0
  let rate = 16000
  let sources = []
  let ended = false
  let idleTimer = null

  const ensure = () => {
    if (!ctx) ctx = new AudioContext()
    if (ctx.state === 'suspended') void ctx.resume()
    return ctx
  }
  const scheduleIdle = () => {
    clearTimeout(idleTimer)
    if (!ctx) return
    const left = Math.max(0, at - ctx.currentTime)
    idleTimer = setTimeout(() => {
      if (ended && ctx && ctx.currentTime >= at - 0.02) onIdle?.()
    }, left * 1000 + 60)
  }
  return {
    start(newRate) {
      rate = newRate || 16000
      ended = false
      const c = ensure()
      at = Math.max(at, c.currentTime + 0.12) // a small jitter buffer before the first frame
    },
    push(bytes) {
      const c = ensure()
      const samples = new Int16Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + (bytes.byteLength & ~1)))
      if (!samples.length) return
      const buf = c.createBuffer(1, samples.length, rate)
      const ch = buf.getChannelData(0)
      for (let i = 0; i < samples.length; i++) ch[i] = samples[i] / 32768
      const src = c.createBufferSource()
      src.buffer = buf
      src.connect(c.destination)
      at = Math.max(at, c.currentTime)
      src.start(at)
      at += buf.duration
      sources.push(src)
      src.onended = () => {
        sources = sources.filter(s => s !== src)
      }
    },
    end() {
      ended = true
      scheduleIdle()
    },
    stop() {
      ended = true
      for (const s of sources) {
        try {
          s.stop()
        } catch {
          // already stopped
        }
      }
      sources = []
      if (ctx) at = ctx.currentTime
      clearTimeout(idleTimer)
      onIdle?.()
    },
    dispose() {
      this.stop()
      void ctx?.close()
      ctx = null
    }
  }
}

/** A short two-note chime (the timer's alarm), synthesized: no audio file to ship. */
export function chime() {
  try {
    const ctx = new AudioContext()
    const notes = [880, 1318.5, 880, 1318.5]
    notes.forEach((f, i) => {
      const osc = ctx.createOscillator()
      const gain = ctx.createGain()
      const t = ctx.currentTime + i * 0.22
      osc.frequency.value = f
      osc.type = 'sine'
      gain.gain.setValueAtTime(0, t)
      gain.gain.linearRampToValueAtTime(0.25, t + 0.02)
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.2)
      osc.connect(gain).connect(ctx.destination)
      osc.start(t)
      osc.stop(t + 0.22)
    })
    setTimeout(() => void ctx.close(), 1400)
  } catch {
    // no audio output: the screen still shows 0:00
  }
}
