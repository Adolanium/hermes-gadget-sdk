// The round gadget screen, drawn like the firmware draws it: a navy disc, a thin ring in the state's
// colour, the mascot, the state's name, the conversation text and a hint. The same component shows
// the virtual gadget and mirrors real devices. Colours are the device's own, not the theme's: this is
// a picture of hardware.
import { useEffect, useRef } from 'react'
import mascot from '../assets/mascot.png'

export const STATE_COLOR = { ready: '#1ee39a', listening: '#1ee39a', thinking: '#3aa8ff', speaking: '#ffad3b' }
const LABEL = { ready: 'Ready', listening: 'Listening', thinking: 'Thinking', speaking: 'Speaking' }
const HINT = { ready: 'Hold to talk', listening: 'Release to send', thinking: 'Tap to stop', speaking: 'Tap to stop' }

function ImageCanvas({ image }) {
  const ref = useRef(null)
  useEffect(() => {
    const c = ref.current
    if (!c || !image) return
    c.width = image.width
    c.height = image.height
    c.getContext('2d').putImageData(new ImageData(image.rgba, image.width, image.height), 0, 0)
  }, [image])
  return <canvas ref={ref} className="hg-screen-image" />
}

function fmtTimer(ms) {
  const s = Math.max(0, Math.ceil(ms / 1000))
  const m = Math.floor(s / 60)
  return `${m}:${String(s % 60).padStart(2, '0')}`
}

/**
 * props: { size, view: { conn, paired, pairingCode, state, heard, reply, status, card, image, notice,
 *          prompt, light, timer }, now, offline, compact, onAnswer, onDismiss }
 */
export function Screen({ size = 360, view, now = Date.now(), offline = false, compact = false, onAnswer, onDismiss, children }) {
  const state = view.state ?? 'ready'
  const color = offline ? '#5b6474' : STATE_COLOR[state] ?? STATE_COLOR.ready
  const card = view.card && (!view.card.until || view.card.until > now) ? view.card : null
  const image = view.image && (!view.image.until || view.image.until > now) ? view.image : null
  const notice = view.notice && view.notice.until > now ? view.notice : null
  const timer = view.timer && view.timer.endsAt > now - 4000 ? view.timer : null
  const unpaired = !offline && view.conn === 'online' && !view.paired
  const text = state === 'thinking' && !view.reply ? (view.status || view.heard) : view.reply || view.heard
  const quoted = state === 'thinking' && !view.reply && !view.status && view.heard

  return (
    <div
      className="hg-device"
      data-state={offline ? 'offline' : state}
      data-compact={compact}
      style={{ '--hg-size': `${size}px`, '--hg-ring': color, '--hg-light': view.light ?? 'transparent' }}
    >
      <div className="hg-glow" aria-hidden="true" />
      <div className="hg-bezel">
        <div className="hg-screen">
          <div className="hg-ring" aria-hidden="true" />
          {offline ? (
            <div className="hg-screen-center">
              <img className="hg-mascot hg-mascot--dim" src={mascot} alt="" />
              <span className="hg-label" style={{ color }}>
                Offline
              </span>
            </div>
          ) : view.prompt ? (
            <div className="hg-screen-center hg-prompt">
              <span className="hg-label" style={{ color: STATE_COLOR.thinking }}>
                {view.prompt.title || 'Question'}
              </span>
              <span className="hg-text">{view.prompt.text}</span>
              {onAnswer ? (
                <span className="hg-prompt-buttons">
                  <button type="button" onClick={() => onAnswer(true)}>
                    Yes
                  </button>
                  <button type="button" onClick={() => onAnswer(false)}>
                    No
                  </button>
                </span>
              ) : null}
            </div>
          ) : image ? (
            <div className="hg-screen-center" onClick={onDismiss} title={onDismiss ? 'Tap to dismiss' : undefined}>
              <ImageCanvas image={image} />
            </div>
          ) : card ? (
            <div className="hg-screen-center hg-card" onClick={onDismiss} title={onDismiss ? 'Tap to dismiss' : undefined}>
              {card.title ? <span className="hg-card-title">{card.title}</span> : null}
              <span className="hg-card-body">{card.body}</span>
              {onDismiss ? <span className="hg-hint">Tap to dismiss</span> : null}
            </div>
          ) : unpaired ? (
            <div className="hg-screen-center">
              <img className="hg-mascot hg-mascot--dim" src={mascot} alt="" />
              <span className="hg-label" style={{ color: STATE_COLOR.speaking }}>
                Pair with Hermes
              </span>
              {view.pairingCode ? <span className="hg-code">{view.pairingCode}</span> : <span className="hg-text">Waiting for a code…</span>}
              <span className="hg-hint">Approve it on the right</span>
            </div>
          ) : view.conn && view.conn !== 'online' ? (
            <div className="hg-screen-center">
              <img className="hg-mascot hg-mascot--dim" src={mascot} alt="" />
              <span className="hg-label" style={{ color: '#9aa4b2' }}>
                {view.conn === 'connecting' ? 'Connecting…' : 'Not connected'}
              </span>
            </div>
          ) : (
            <div className="hg-screen-center">
              <div className="hg-face">
                <img className="hg-mascot" src={mascot} alt="" />
                {state === 'thinking' ? (
                  <span className="hg-dots" aria-hidden="true">
                    <i />
                    <i />
                    <i />
                  </span>
                ) : null}
                {state === 'speaking' || state === 'listening' ? (
                  <span className="hg-waves" data-kind={state} aria-hidden="true">
                    <i />
                    <i />
                  </span>
                ) : null}
              </div>
              <span className="hg-label" style={{ color }}>
                {LABEL[state]}
              </span>
              {timer ? <span className="hg-timer">{timer.label ? `${timer.label} ` : ''}{fmtTimer(timer.endsAt - now)}</span> : null}
              {text ? <span className="hg-text">{quoted ? `“${text}”` : text}</span> : null}
              <span className="hg-hint">{HINT[state]}</span>
            </div>
          )}
          {notice ? <div className="hg-notice">{notice.text}</div> : null}
          <div className="hg-bar" aria-hidden="true" />
        </div>
      </div>
      {children}
    </div>
  )
}
