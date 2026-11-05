// Page chrome follows the Hermes theme (--ui-* variables); the device itself keeps the hardware's
// own colours, since it is a picture of a real screen.
export const CSS = `
.hg-root { position: absolute; inset: 0; overflow: auto; color: var(--ui-text-primary); font: 13px/1.45 var(--font-sans, system-ui, sans-serif);
  background: radial-gradient(ellipse 70% 60% at 30% 40%, color-mix(in oklab, var(--ui-accent) 7%, transparent), transparent 70%), var(--ui-bg-editor, var(--ui-base)); }
.hg-wrap { max-width: 1240px; margin: 0 auto; padding: 22px 28px 40px; display: flex; flex-direction: column; gap: 18px; }
.hg-head { display: flex; align-items: flex-end; justify-content: space-between; gap: 16px; flex-wrap: wrap; }
.hg-title { font-size: 22px; font-weight: 650; letter-spacing: -.01em; margin: 0; }
.hg-sub { color: var(--ui-text-secondary); margin: 2px 0 0; }
.hg-chip { display: inline-flex; align-items: center; gap: 7px; height: 26px; padding: 0 11px; border-radius: 999px; border: 1px solid var(--ui-stroke-tertiary, rgba(127,127,127,.25));
  font-size: 11.5px; color: var(--ui-text-secondary); white-space: nowrap; }
.hg-dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; flex: none; }
.hg-tone-ok { color: #1ec98a; } .hg-tone-warn { color: #e0a33a; } .hg-tone-bad { color: #e5534b; } .hg-tone-idle { color: var(--ui-text-tertiary, #888); }
.hg-grid { display: grid; grid-template-columns: minmax(0, 1fr) 380px; gap: 22px; align-items: start; }
.hg-panel { border: 1px solid var(--ui-stroke-tertiary, rgba(127,127,127,.22)); border-radius: 14px; background: color-mix(in oklab, var(--ui-bg-editor, #111) 86%, var(--ui-text-primary) 3%); padding: 16px 18px; }
.hg-panel h2 { font-size: 11px; letter-spacing: .14em; text-transform: uppercase; color: var(--ui-text-tertiary, #888); margin: 0 0 12px; font-weight: 600; }
.hg-stage { display: flex; flex-direction: column; align-items: center; gap: 18px; padding: 26px 18px 22px; }
.hg-stage-row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; justify-content: center; }
.hg-talk { appearance: none; border: 0; cursor: pointer; user-select: none; touch-action: none; display: inline-flex; align-items: center; gap: 10px;
  height: 52px; padding: 0 26px; border-radius: 26px; font: 600 14px/1 inherit; color: #04130d; background: #1ee39a; box-shadow: 0 6px 24px rgba(30,227,154,.25);
  transition: transform .12s, box-shadow .2s, background .2s; }
.hg-talk:active, .hg-talk[data-held="true"] { transform: scale(.97); background: #48f0b2; box-shadow: 0 0 0 8px rgba(30,227,154,.18), 0 6px 24px rgba(30,227,154,.35); }
.hg-talk:disabled { background: color-mix(in oklab, var(--ui-text-primary) 18%, transparent); color: var(--ui-text-tertiary); box-shadow: none; cursor: default; }
.hg-mic-meter { width: 46px; height: 6px; border-radius: 3px; background: rgba(0,0,0,.18); overflow: hidden; }
.hg-mic-meter i { display: block; height: 100%; background: #04130d; transition: width .08s; }
.hg-type { display: flex; gap: 8px; width: min(460px, 100%); }
.hg-input { flex: 1; min-width: 0; height: 36px; padding: 0 12px; border-radius: 9px; border: 1px solid var(--ui-stroke-tertiary, rgba(127,127,127,.3)); background: transparent; color: inherit; font: inherit; }
.hg-input:focus { outline: 2px solid var(--ui-accent); outline-offset: -1px; }
textarea.hg-input { height: auto; min-height: 64px; padding: 8px 12px; resize: vertical; }
.hg-btn { appearance: none; height: 34px; padding: 0 13px; border-radius: 9px; border: 1px solid var(--ui-stroke-tertiary, rgba(127,127,127,.3)); background: transparent; color: inherit;
  font: 500 12.5px/1 inherit; cursor: pointer; display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; }
.hg-btn:hover { background: color-mix(in oklab, var(--ui-text-primary) 6%, transparent); }
.hg-btn:disabled { opacity: .45; cursor: default; }
.hg-btn--primary { background: var(--ui-accent); border-color: transparent; color: var(--ui-accent-foreground, #fff); }
.hg-btn--primary:hover { background: color-mix(in oklab, var(--ui-accent) 88%, #fff); }
.hg-hintline { color: var(--ui-text-tertiary, #888); font-size: 11.5px; text-align: center; max-width: 460px; }
.hg-error { color: #e5534b; font-size: 12px; text-align: center; max-width: 460px; }
.hg-list { display: flex; flex-direction: column; gap: 10px; }
.hg-dev { display: grid; grid-template-columns: 96px minmax(0, 1fr); gap: 12px; align-items: center; padding: 10px; border-radius: 12px; cursor: pointer;
  border: 1px solid transparent; transition: background .15s, border-color .15s; }
.hg-dev:hover { background: color-mix(in oklab, var(--ui-text-primary) 4%, transparent); }
.hg-dev[data-selected="true"] { border-color: color-mix(in oklab, var(--ui-accent) 55%, transparent); background: color-mix(in oklab, var(--ui-accent) 7%, transparent); }
.hg-dev-name { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hg-dev-meta { color: var(--ui-text-tertiary, #888); font-size: 11.5px; display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
.hg-dev-said { color: var(--ui-text-secondary); font-size: 12px; margin-top: 4px; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
.hg-tag { font-size: 10.5px; padding: 1px 6px; border-radius: 5px; background: color-mix(in oklab, var(--ui-accent) 16%, transparent); color: var(--ui-text-primary); }
.hg-pair { display: grid; grid-template-columns: 1fr auto; gap: 4px 12px; align-items: center; padding: 12px; border-radius: 12px;
  border: 1px solid color-mix(in oklab, #ffad3b 55%, transparent); background: color-mix(in oklab, #ffad3b 9%, transparent); }
.hg-pair-code { font: 700 18px/1.2 var(--font-mono, ui-monospace, monospace); letter-spacing: .12em; }
.hg-field { display: flex; flex-direction: column; gap: 5px; }
.hg-field > span { font-size: 11px; color: var(--ui-text-tertiary, #888); }
.hg-row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.hg-result { font-size: 12px; color: var(--ui-text-secondary); }
.hg-empty { color: var(--ui-text-tertiary, #888); font-size: 12.5px; padding: 6px 2px; }
.hg-setup ol { margin: 6px 0 0; padding-left: 18px; color: var(--ui-text-secondary); }
.hg-setup code { font: 12px var(--font-mono, ui-monospace, monospace); background: color-mix(in oklab, var(--ui-text-primary) 8%, transparent); padding: 1px 5px; border-radius: 4px; }
@media (max-width: 960px) { .hg-grid { grid-template-columns: 1fr; } }

/* ---- the device ---- */
.hg-device { --s: var(--hg-size); position: relative; width: var(--s); height: var(--s); flex: none; }
.hg-glow { position: absolute; inset: -16%; border-radius: 50%; background: radial-gradient(circle, transparent 50%, var(--hg-light) 60%, transparent 74%); opacity: .95; filter: blur(calc(var(--s) * .035)); transition: background .4s; pointer-events: none; }
.hg-bezel { position: absolute; inset: 0; border-radius: 50%; padding: calc(var(--s) * .045);
  background: radial-gradient(circle at 35% 25%, #2b2f36, #0c0d10 62%); box-shadow: 0 calc(var(--s) * .05) calc(var(--s) * .12) rgba(0,0,0,.45), inset 0 1px 0 rgba(255,255,255,.08); }
.hg-screen { position: relative; width: 100%; height: 100%; border-radius: 50%; overflow: hidden; background: radial-gradient(circle at 50% 38%, #12224a, #0a1330 70%, #070d22);
  color: #fff; font-family: 'Montserrat', 'Inter', system-ui, sans-serif; }
.hg-ring { position: absolute; inset: 3.2%; border-radius: 50%; border: calc(var(--s) * .009 + 1px) solid var(--hg-ring);
  box-shadow: 0 0 calc(var(--s) * .03) var(--hg-ring), inset 0 0 calc(var(--s) * .025) color-mix(in srgb, var(--hg-ring) 55%, transparent); transition: border-color .35s, box-shadow .35s; }
.hg-device[data-state="listening"] .hg-ring { animation: hg-breathe 1.2s ease-in-out infinite; }
@keyframes hg-breathe { 50% { box-shadow: 0 0 calc(var(--s) * .06) var(--hg-ring), inset 0 0 calc(var(--s) * .04) var(--hg-ring); } }
.hg-screen-center { position: absolute; inset: 11% 13% 14%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: calc(var(--s) * .012); text-align: center; }
.hg-face { position: relative; width: calc(var(--s) * .36); height: calc(var(--s) * .36); flex: none; }
.hg-mascot { width: calc(var(--s) * .36); height: calc(var(--s) * .36); object-fit: contain; display: block; }
.hg-mascot--dim { opacity: .35; }
.hg-label { font-weight: 700; font-size: calc(var(--s) * .05); line-height: 1.1; }
.hg-text { font-weight: 600; font-size: calc(var(--s) * .036); line-height: 1.22; max-height: 4.9em; overflow: hidden; display: -webkit-box; -webkit-line-clamp: 4; -webkit-box-orient: vertical; color: #f3f5f9; }
.hg-hint { font-size: calc(var(--s) * .03); color: #8fb4ff; margin-top: calc(var(--s) * .01); }
.hg-code { font: 700 calc(var(--s) * .085)/1 var(--font-mono, ui-monospace, monospace); letter-spacing: .1em; color: #fff; }
.hg-timer { font: 700 calc(var(--s) * .07)/1 var(--font-mono, ui-monospace, monospace); color: #ffad3b; }
.hg-card { justify-content: center; gap: calc(var(--s) * .025); }
.hg-card-title { font-weight: 700; font-size: calc(var(--s) * .055); color: #1ee39a; }
.hg-card-body { font-weight: 600; font-size: calc(var(--s) * .042); line-height: 1.25; white-space: pre-wrap; }
.hg-screen-image { width: 74%; aspect-ratio: 1; object-fit: contain; border-radius: 8px; image-rendering: auto; }
.hg-prompt { gap: calc(var(--s) * .025); }
.hg-prompt-buttons { display: flex; gap: 10px; }
.hg-prompt-buttons button { appearance: none; border: 0; border-radius: 999px; padding: 6px 16px; font: 700 calc(var(--s) * .035) inherit; cursor: pointer; background: #1ee39a; color: #04130d; }
.hg-prompt-buttons button + button { background: #e5534b; color: #fff; }
.hg-notice { position: absolute; left: 50%; bottom: 17%; transform: translateX(-50%); max-width: 70%; padding: 4px 10px; border-radius: 999px; font-size: calc(var(--s) * .028);
  background: rgba(255,255,255,.12); color: #fff; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hg-bar { position: absolute; left: 34%; right: 34%; bottom: 4.6%; height: calc(var(--s) * .009 + 1px); border-radius: 2px; background: rgba(255,255,255,.85); box-shadow: 0 0 8px rgba(255,255,255,.4); }
.hg-dots { position: absolute; right: 6%; top: 8%; display: flex; gap: calc(var(--s) * .008); }
.hg-dots i { width: calc(var(--s) * .016); height: calc(var(--s) * .016); border-radius: 50%; background: #3aa8ff; animation: hg-dot 1.1s ease-in-out infinite; }
.hg-dots i:nth-child(2) { animation-delay: .18s; } .hg-dots i:nth-child(3) { animation-delay: .36s; }
@keyframes hg-dot { 0%, 100% { opacity: .25; transform: translateY(0); } 40% { opacity: 1; transform: translateY(-30%); } }
.hg-waves { position: absolute; left: 4%; top: 42%; width: calc(var(--s) * .06); height: calc(var(--s) * .08); }
.hg-waves i { position: absolute; inset: 0; border: calc(var(--s) * .006 + 1px) solid var(--hg-ring); border-right: 0; border-top-color: transparent; border-bottom-color: transparent; border-radius: 50%; animation: hg-wave 1s ease-out infinite; }
.hg-waves i:nth-child(2) { animation-delay: .5s; }
@keyframes hg-wave { from { transform: scale(.4) translateX(40%); opacity: 1; } to { transform: scale(1.3) translateX(-10%); opacity: 0; } }
.hg-device[data-compact="true"] .hg-text, .hg-device[data-compact="true"] .hg-hint, .hg-device[data-compact="true"] .hg-notice, .hg-device[data-compact="true"] .hg-card-body { display: none; }
.hg-device[data-compact="true"] .hg-label { font-size: calc(var(--s) * .085); }
.hg-device[data-compact="true"] .hg-card-title { font-size: calc(var(--s) * .085); }
.hg-root[data-motion="reduced"] * { animation: none !important; }
`
