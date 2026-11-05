// The Gadgets page: a virtual gadget you can talk to right here, and every real gadget this Hermes
// knows, mirrored live, with pairing approvals, cards for their screens and their actions.
import { useCallback, useEffect, useRef, useState } from 'react'
import { Codicon } from '@hermes/plugin-sdk'
import { chime, createPlayer, startMic } from '../audio.js'
import { createDevice, newKey } from '../device.js'
import { Screen } from './screen.jsx'
import { CSS } from './styles.js'

const VIRTUAL_NAME = 'Desktop Gadget'

function useNow(ms = 500) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms)
    return () => clearInterval(id)
  }, [ms])
  return now
}

/** Poll `fn` every `ms` while the page is visible. */
function usePoll(fn, ms) {
  const saved = useRef(fn)
  saved.current = fn
  useEffect(() => {
    let alive = true
    let timer = 0
    const tick = async () => {
      if (!document.hidden) {
        try {
          await saved.current()
        } catch {
          // the next tick tries again
        }
      }
      if (alive) timer = setTimeout(tick, ms)
    }
    tick()
    return () => {
      alive = false
      clearTimeout(timer)
    }
  }, [ms])
}

const ago = (t, now) => {
  if (!t) return 'never'
  const s = Math.max(0, Math.round(now / 1000 - t))
  if (s < 60) return 'just now'
  if (s < 3600) return `${Math.floor(s / 60)} min ago`
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`
  return `${Math.floor(s / 86400)} d ago`
}

// -- the virtual gadget --------------------------------------------------------------------------

function VirtualGadget({ rest, storage, info, onDevice }) {
  const [view, setView] = useState(null)
  const [held, setHeld] = useState(false)
  const [micError, setMicError] = useState(null)
  const [level, setLevel] = useState(0)
  const [draft, setDraft] = useState('')
  const [token, setToken] = useState(() => storage.get('token', '') || '')
  const [tokenDraft, setTokenDraft] = useState('')
  const now = useNow(250)
  const dev = useRef(null)
  const mic = useRef(null)
  const player = useRef(null)

  useEffect(() => {
    if (!info?.url || !info.enabled) return undefined
    let key = storage.get('deviceKey', null)
    if (!key) {
      key = newKey()
      storage.set('deviceKey', key)
    }
    player.current = createPlayer({ onIdle: () => dev.current?.audioDone() })
    const d = createDevice({
      url: info.url,
      key,
      name: VIRTUAL_NAME,
      token: token || undefined,
      onChange: s => setView(s),
      onAudio: ({ start, rate, pcm }) => {
        if (start) player.current?.start(rate)
        else if (pcm) player.current?.push(pcm)
      },
      onAudioEnd: ({ aborted }) => (aborted ? player.current?.stop() : player.current?.end()),
      onStopAudio: () => player.current?.stop()
    })
    dev.current = d
    setView(d.state)
    void d.connect().then(() => onDevice?.(d.id))
    return () => {
      d.disconnect()
      mic.current?.()
      player.current?.dispose()
      dev.current = null
    }
  }, [info?.url, info?.enabled, token, storage, onDevice])

  // The timer action's alarm: chime once when it reaches zero.
  const rung = useRef(null)
  const endsAt = view?.timer?.endsAt
  useEffect(() => {
    if (endsAt && now >= endsAt && rung.current !== endsAt) {
      rung.current = endsAt
      chime()
    }
  }, [endsAt, now])

  useEffect(() => {
    if (!held) return undefined
    const id = setInterval(() => setLevel(mic.current?.level?.() ?? 0), 80)
    return () => clearInterval(id)
  }, [held])

  // Bumped by every release and by leaving the page, so a microphone that starts after either
  // (the permission prompt was still open) is stopped at once instead of left recording.
  const holdGen = useRef(0)
  useEffect(() => () => {
    holdGen.current++
  }, [])

  // The turn starts only once the microphone runs, so a refused or failed microphone sends Hermes
  // no empty recording.
  const talking = useRef(false)
  const press = useCallback(async () => {
    const d = dev.current
    if (!d || held || !d.state.paired) return
    setMicError(null)
    setHeld(true)
    const gen = ++holdGen.current
    let stop
    try {
      stop = await startMic(frame => {
        if (talking.current) d.talkData(frame)
      })
    } catch (e) {
      if (gen !== holdGen.current) return
      setHeld(false)
      setMicError(e?.name === 'NotAllowedError' ? 'Microphone access is off for Hermes. Allow it in System Settings › Privacy › Microphone, or type below.' : `Microphone unavailable: ${e?.message ?? e}`)
      return
    }
    if (gen !== holdGen.current) {
      stop() // released, or the page left, while the permission prompt was open
      return
    }
    if (!d.talkStart()) {
      stop()
      setHeld(false)
      return
    }
    talking.current = true
    mic.current = stop
  }, [held])

  const release = useCallback(() => {
    if (!held) return
    holdGen.current++
    setHeld(false)
    mic.current?.()
    mic.current = null
    if (talking.current) {
      talking.current = false
      dev.current?.talkEnd()
    }
  }, [held])

  // Space bar holds to talk too, when the page (not a text field) has focus.
  useEffect(() => {
    const down = e => {
      // Space on another control (a field, a list, a button) keeps its own meaning there.
      if (e.code !== 'Space' || e.repeat || e.target.closest?.('input, textarea, select, button:not(.hg-talk), [contenteditable]')) return
      e.preventDefault()
      void press()
    }
    const up = e => {
      if (e.code === 'Space') release()
    }
    window.addEventListener('keydown', down)
    window.addEventListener('keyup', up)
    window.addEventListener('blur', release) // the keyup lands in another window
    return () => {
      window.removeEventListener('keydown', down)
      window.removeEventListener('keyup', up)
      window.removeEventListener('blur', release)
    }
  }, [press, release])

  if (!info) return <div className="hg-panel hg-stage">Loading…</div>
  if (!info.enabled) return <Setup />
  const v = view ?? { conn: 'connecting' }
  const busy = v.state === 'thinking' || v.state === 'speaking'
  const send = () => {
    if (dev.current?.sendText(draft)) setDraft('')
  }
  return (
    <section className="hg-panel hg-stage" aria-label="Desktop Gadget">
      <Screen size={380} view={v} now={now} onAnswer={yes => dev.current?.answer(yes)} onDismiss={() => dev.current?.dismiss()} />
      <div className="hg-stage-row">
        <button
          type="button"
          className="hg-talk"
          data-held={held}
          disabled={!v.paired || v.conn !== 'online'}
          onPointerDown={e => {
            e.currentTarget.setPointerCapture?.(e.pointerId)
            void press()
          }}
          onPointerUp={release}
          onPointerCancel={release}
        >
          <Codicon name="mic" /> {held ? 'Listening… release to send' : 'Hold to talk'}
          {held ? (
            <span className="hg-mic-meter">
              <i style={{ width: `${Math.min(100, level * 260)}%` }} />
            </span>
          ) : null}
        </button>
        {busy ? (
          <button type="button" className="hg-btn" onClick={() => dev.current?.cancel()}>
            <Codicon name="debug-stop" /> Stop
          </button>
        ) : null}
      </div>
      <form
        className="hg-type"
        onSubmit={e => {
          e.preventDefault()
          send()
        }}
      >
        <input className="hg-input" value={draft} onChange={e => setDraft(e.target.value)} placeholder={v.paired ? 'Or type to Hermes…' : 'Pair first, then type or talk'} disabled={!v.paired} maxLength={4000} />
        <button type="submit" className="hg-btn hg-btn--primary" disabled={!v.paired || !draft.trim()}>
          Send
        </button>
      </form>
      {micError ? <div className="hg-error">{micError}</div> : null}
      {v.error && v.conn !== 'online' ? <div className="hg-error">{v.error}</div> : null}
      {info.token_required && v.conn !== 'online' ? (
        <form
          className="hg-type"
          onSubmit={e => {
            e.preventDefault()
            const t = tokenDraft.trim()
            storage.set('token', t)
            setToken(t)
            setTokenDraft('')
          }}
        >
          <input
            className="hg-input"
            type="password"
            value={tokenDraft}
            onChange={e => setTokenDraft(e.target.value)}
            placeholder={token ? 'Access token saved; enter it again to change it' : 'This gateway asks gadgets for GADGET_ACCESS_TOKEN'}
            aria-label="Gadget access token"
          />
          <button type="submit" className="hg-btn" disabled={!tokenDraft.trim()}>
            Connect
          </button>
        </form>
      ) : null}
      <div className="hg-hintline">
        The same device protocol as a real gadget, running in this window. Hold the button or Space to talk; replies are spoken if Hermes has text-to-speech set up.
        {v.paired ? (
          <>
            {' '}
            <button type="button" className="hg-btn" style={{ height: 24, marginTop: 6 }} onClick={() => dev.current?.newSession()}>
              New conversation
            </button>
          </>
        ) : null}
      </div>
    </section>
  )
}

function Setup() {
  return (
    <section className="hg-panel hg-setup" aria-label="Set up">
      <h2>Turn on the Gadget platform</h2>
      <p>Gadgets connect to your Hermes gateway. The platform is off on this Hermes.</p>
      <ol>
        <li>
          Run <code>hermes gateway setup</code> and choose <b>Hermes Gadget</b>, or <code>hermes config set platforms.gadget.enabled true</code>.
        </li>
        <li>Restart the gateway (quit and reopen Hermes Desktop).</li>
        <li>Come back here: a virtual gadget connects on its own, and real ones appear on the right.</li>
      </ol>
    </section>
  )
}

// -- real devices --------------------------------------------------------------------------------

function DeviceRow({ d, selected, onSelect, now, virtualId }) {
  const a = d.activity ?? { state: 'ready' }
  const said = a.reply || a.heard
  const battery = d.battery?.battery_percent
  return (
    <div className="hg-dev" data-selected={selected} onClick={() => onSelect(d.id)} role="button" tabIndex={0} onKeyDown={e => e.key === 'Enter' && onSelect(d.id)}>
      <Screen size={96} compact offline={!d.online} view={{ ...a, conn: 'online', paired: d.paired !== false }} now={now} />
      <div style={{ minWidth: 0 }}>
        <div className="hg-dev-name">
          {d.name || d.id} {d.id === virtualId ? <span className="hg-tag">this window</span> : null}
        </div>
        <div className="hg-dev-meta">
          <span className={`hg-dot ${d.online ? 'hg-tone-ok' : 'hg-tone-idle'}`} />
          {d.online ? (d.paired === false ? 'waiting to pair' : 'online') : `seen ${ago(d.last_seen, now)}`}
          {d.board ? <span>{d.board}</span> : null}
          {Number.isFinite(battery) ? <span>{Math.round(battery)}%{d.battery?.charging ? ' ⚡' : ''}</span> : null}
        </div>
        {d.online && said ? <div className="hg-dev-said">{a.reply ? said : `“${said}”`}</div> : null}
      </div>
    </div>
  )
}

function Controls({ d, rest }) {
  const [title, setTitle] = useState('')
  const [text, setText] = useState('')
  const [seconds, setSeconds] = useState('20')
  const [action, setAction] = useState(d.actions?.[0] ?? '')
  const [args, setArgs] = useState('{}')
  const [result, setResult] = useState(null)
  const [busy, setBusy] = useState(false)
  useEffect(() => {
    setResult(null)
  }, [d.id])
  const actionsKey = (d.actions ?? []).join('|')
  useEffect(() => {
    // A device seen offline first has no actions; when it reconnects, select one it has.
    setAction(cur => (d.actions?.includes(cur) ? cur : d.actions?.[0] ?? ''))
  }, [d.id, actionsKey]) // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (action === 'light.set') setArgs('{"color": "#7c5cff"}')
    else if (action === 'timer.start') setArgs('{"seconds": 60, "label": "Tea"}')
    else setArgs('{}')
  }, [action])
  const run = async (path, label) => {
    setBusy(true)
    setResult(null)
    try {
      const answer = await rest(path, { method: 'POST' })
      setResult(answer?.ok ? `${label}${answer.held ? ' (the screen is off; it shows when it wakes)' : ''}` : answer?.error ?? 'Failed')
    } catch (e) {
      setResult(String(e?.message ?? e))
    } finally {
      setBusy(false)
    }
  }
  if (!d.online) return <div className="hg-empty">{d.name || d.id} is offline. Cards and actions work while it's connected.</div>
  if (d.paired === false) return <div className="hg-empty">Approve {d.name || d.id} first.</div>
  const q = (obj) => new URLSearchParams(obj).toString()
  return (
    <div className="hg-list">
      <div className="hg-field">
        <span>Show on its screen</span>
        <input className="hg-input" value={title} onChange={e => setTitle(e.target.value)} placeholder="Title (optional)" maxLength={64} />
        <textarea className="hg-input" value={text} onChange={e => setText(e.target.value)} placeholder="Shopping: milk, eggs, bread" maxLength={600} />
        <div className="hg-row">
          <select className="hg-input" style={{ flex: '0 0 130px' }} value={seconds} onChange={e => setSeconds(e.target.value)}>
            <option value="10">10 seconds</option>
            <option value="20">20 seconds</option>
            <option value="60">1 minute</option>
            <option value="300">5 minutes</option>
            <option value="0">Until dismissed</option>
          </select>
          <button type="button" className="hg-btn hg-btn--primary" disabled={busy || (!title.trim() && !text.trim())} onClick={() => run(`/devices/${d.id}/card?${q({ title, text, seconds })}`, 'Shown on the screen.')}>
            <Codicon name="device-mobile" /> Send to screen
          </button>
        </div>
      </div>
      {d.actions?.length ? (
        <div className="hg-field">
          <span>Run a device action</span>
          <div className="hg-row">
            <select className="hg-input" style={{ flex: '0 0 150px' }} value={action} onChange={e => setAction(e.target.value)}>
              {d.actions.map(a => (
                <option key={a} value={a}>
                  {a}
                </option>
              ))}
            </select>
            <input className="hg-input" value={args} onChange={e => setArgs(e.target.value)} aria-label="Arguments (JSON)" />
            <button type="button" className="hg-btn" disabled={busy || !action} onClick={() => run(`/devices/${d.id}/action?${q({ name: action, args })}`, `Ran ${action}.`)}>
              Run
            </button>
          </div>
        </div>
      ) : null}
      {result ? <div className="hg-result">{result}</div> : null}
    </div>
  )
}

export function GadgetsPage({ rest, storage }) {
  const [info, setInfo] = useState(null)
  const [devices, setDevices] = useState({ gateway: null, devices: [] })
  const [waiting, setWaiting] = useState([])
  const [selected, setSelected] = useState(null)
  const [approving, setApproving] = useState(null)
  const [virtualId, setVirtualId] = useState(null)
  const now = useNow(1000)

  usePoll(async () => {
    const answer = await rest('/info')
    if (answer?.ok) setInfo(answer)
  }, 10_000)
  usePoll(async () => {
    const answer = await rest('/devices')
    if (answer?.devices) setDevices(answer)
  }, 900)
  usePoll(async () => {
    const answer = await rest('/pairing')
    if (answer?.ok) setWaiting(answer.waiting ?? [])
  }, 1500)

  const approve = async id => {
    setApproving(id)
    try {
      await rest(`/pairing/${id}/approve`, { method: 'POST' })
    } finally {
      setApproving(null)
    }
  }

  const list = devices.devices ?? []
  const current = list.find(d => d.id === selected) ?? list.find(d => d.online && d.id !== virtualId) ?? list[0] ?? null
  const running = devices.gateway?.running
  const online = list.filter(d => d.online).length

  return (
    <div className="hg-root" data-motion={window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ? 'reduced' : 'full'}>
      <style>{CSS}</style>
      <div className="hg-wrap">
        <header className="hg-head">
          <div>
            <h1 className="hg-title">Gadgets</h1>
            <p className="hg-sub">Hold a button, ask Hermes, hear the answer. Small devices for your own Hermes, with its tools and memory.</p>
          </div>
          <span className="hg-chip">
            <span className={`hg-dot ${running ? 'hg-tone-ok' : info && !info.enabled ? 'hg-tone-idle' : 'hg-tone-warn'}`} />
            {running ? `Gadget platform on · ${online} online` : info && !info.enabled ? 'Gadget platform off' : 'Gateway not reporting'}
          </span>
        </header>
        <div className="hg-grid">
          <VirtualGadget rest={rest} storage={storage} info={info} onDevice={setVirtualId} />
          <aside className="hg-list">
            {waiting.length ? (
              <section className="hg-panel">
                <h2>Waiting to pair</h2>
                <div className="hg-list">
                  {waiting.map(w => (
                    <div key={w.id} className="hg-pair">
                      <div>
                        <div className="hg-dev-name">{w.name}</div>
                        <div className="hg-dev-meta">{w.board || w.id} · check the code on its screen</div>
                      </div>
                      <button type="button" className="hg-btn hg-btn--primary" disabled={approving === w.id} onClick={() => approve(w.id)}>
                        Approve
                      </button>
                      <div className="hg-pair-code">{w.code}</div>
                    </div>
                  ))}
                </div>
              </section>
            ) : null}
            <section className="hg-panel">
              <h2>Your gadgets</h2>
              {list.length ? (
                <div className="hg-list">
                  {list.map(d => (
                    <DeviceRow key={d.id} d={d} now={now} virtualId={virtualId} selected={current?.id === d.id} onSelect={setSelected} />
                  ))}
                </div>
              ) : (
                <div className="hg-empty">No gadgets yet. Set up a board with the browser installer, or talk to the virtual one here.</div>
              )}
            </section>
            {current ? (
              <section className="hg-panel">
                <h2>{current.name || current.id}</h2>
                <Controls d={current} rest={rest} />
              </section>
            ) : null}
          </aside>
        </div>
      </div>
    </div>
  )
}
