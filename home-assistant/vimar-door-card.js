/* Vimar K40955 door card: one dashboard card instead of the Door + Door live views (T43).
 *
 * Layout (top to bottom): video, "slide to open the gate" row, status line, round Answer / Hang up buttons.
 * Video: the AlexxIT WebRTC card (custom:webrtc-camera) is put in place of the placeholder without navigation
 * and without a page reload: when the view is opened (autostart), on a tap on the placeholder, or when the HA
 * call of the logged-in user starts. Hang up / end of that call removes it again (the WebRTC card then closes
 * the stream and releases the microphone after its own 5 s disconnect delay); it stays off until the placeholder
 * is tapped or the view is opened again.
 * While the video is shown the indoor monitor is in use ("viewing"): with autostart do not make this view the
 * first view of the dashboard.
 *
 * Runs in the browser only: no backend component, no polling, no timers. It only reacts to state updates HA
 * already sends to the dashboard. It sends the same commands as the two views did:
 *   Answer    -> script.vimar_answer {user: <id of the logged-in user>}
 *   Hang up   -> mqtt.publish <topic>/end_remote, except in state viewing/idle (T38: no "41" while mainapp's
 *                live session is still open)
 *   Open gate -> mqtt.publish <topic>/open_gate
 *
 * Install: copy this file to /config/www/, add the dashboard resource /local/vimar-door-card.js (JavaScript
 * module), then use the card:
 *   type: custom:vimar-door-card
 * Optional keys with their defaults: see DEFAULTS below.
 */

export const DEFAULTS = {
  stream: 'vimar_k40955',                           // go2rtc stream name
  call_state: 'sensor.vimar_k40955_call_state',
  owner: 'input_text.vimar_call_owner',             // HA user id of the person who answered (set by the script)
  priority: 'input_button.vimar_monitor_priority',  // pressed when someone swipes the monitor's remote page (T57)
  answer_script: 'vimar_answer',
  topic: 'vimar/k40955/cmd',
  mode: 'webrtc',
  media: 'video,audio,microphone',
  muted: false,                                     // needs "Autoplay videos" in the Android companion app (T42)
  autostart: true,                                  // open the video when the view is opened (idle / viewing only)
  autostart_ringing: false,                         // also during a ring. mainapp treats a live session opened
                                                    // during a ring as an app answer (T41): only with the T41
                                                    // bridge installed (Answer then takes the call over)
};

/* ---- logic (no DOM; tested with node, test/logic_test.mjs) --------------------------------------------- */

/** New model after a state update. m = {live, myCall}: live = stream shown, myCall = this user holds the HA call. */
export function onState(m, st, owner, me, ringVideo) {
  const mine = st === 'answered_HA' && !!me && owner === me;
  if (mine && !m.myCall) return {live: true, myCall: true};       // my call started (button, notification, reload)
  if (!mine && m.myCall) {
    // my call is over: stop the stream. Owner cleared while the call still runs: keep the picture.
    return {live: st === 'answered_HA' ? m.live : false, myCall: false};
  }
  // R3 (docs/09): someone else answered -> video off here. Not in answered_app: with the cloud blocked that is
  // our own viewer opened during the ring (T41), which the takeover Answer needs. Owner still empty: wait.
  if (m.live && !m.myCall && (st === 'answered_monitor' || (st === 'answered_HA' && !!owner && owner !== me))) {
    return {live: false, myCall: false};
  }
  // T50: a ring while this phone only watches. mainapp freezes the picture, ends the session 10 s later, and the
  // reconnect makes it stand down with the ringtone still playing (answered_app, Answer locked ~40 s). Closing
  // the video now keeps the ring a normal ring; it comes back after Answer. Not with autostart_ringing (B1 fix).
  if (m.live && !m.myCall && st === 'ringing' && !ringVideo) return {live: false, myCall: false};
  return m;
}

/** May the video open by itself when the view is opened? Never during a call held elsewhere (a live session
 *  opened after an answer at the indoor unit gets no frames and is reset), never with an unknown state. */
export function autoStartAllowed(cfg, st) {
  return !!cfg.autostart && startAllowed(cfg, st);
}

/** May the video be started here at all (view opened or Start video tapped)? Only in idle / viewing. Not during a
 *  ring: a viewer opened then makes mainapp stand down with its ringtone still playing (answered_app, T41/T48),
 *  unless autostart_ringing (needs the B1 fix, docs/09 step C). Not during a call held elsewhere (R3). */
/** Swipe on the indoor monitor (input_button pressed, T57) less than 60 s ago: no autostart, so the phone does not
 *  take the monitor back. state = the input_button's state (ISO time of the last press). */
export function priorityRecent(state, now) {
  const t = Date.parse(state);
  return !isNaN(t) && now - t >= 0 && now - t < 60000;
}

export function startAllowed(cfg, st) {
  if (st === 'idle' || st === 'viewing') return true;
  return st === 'ringing' && !!cfg.autostart_ringing;
}

/** R4 (docs/09, option 1): only the owner of the HA call ends it with end_remote; on every other phone and in
 *  every other state Hang up only removes the local stream. myCall covers an owner cleared during the call. */
export function hangupPublishes(st, owner, me, myCall) {
  return st === 'answered_HA' && (myCall || (!!me && owner === me));
}

/** Text under the buttons; '' = hidden. persons = [{user_id, name}]. */
export function statusText(st, owner, persons) {
  if (st === 'ringing') return 'Incoming call';
  if (st === 'answered_monitor') return 'Answered at the indoor unit';
  if (st === 'answered_app') return 'Video opened during the ring: Answer after the ringtone (~40 s)';
  if (st === 'answered_HA') {
    const p = persons.find(x => x.user_id === owner);
    return 'Answered by ' + (p ? p.name : 'another device');
  }
  return '';
}

/* ---- card ---------------------------------------------------------------------------------------------- */

const CSS = `
  ha-card { overflow: hidden; }
  .video { position: relative; width: 100%; aspect-ratio: 11 / 9; background: #000; }
  .video.live { aspect-ratio: auto; }
  .ph { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center;
        justify-content: center; gap: 8px; color: #fff; background: none; border: 0; width: 100%;
        font: inherit; cursor: pointer; }
  .ph ha-icon { --mdc-icon-size: 96px; }
  .ph[hidden] { display: none; }
  .ph.off { cursor: default; opacity: 0.6; }
  .slide { position: relative; margin: 12px; height: 56px; border-radius: 28px; overflow: hidden;
        background: var(--secondary-background-color); touch-action: pan-y; user-select: none;
        -webkit-user-select: none; }
  .slide .label { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
        color: var(--secondary-text-color); pointer-events: none; }
  .slide .fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0; background: var(--primary-color);
        opacity: 0.25; pointer-events: none; }
  .slide .thumb { position: absolute; left: 4px; top: 4px; width: 48px; height: 48px; border-radius: 50%;
        display: flex; align-items: center; justify-content: center; cursor: grab; color: #fff;
        background: var(--primary-color); touch-action: none; }
  .slide.done .thumb { background: #43a047; }
  .slide:not(.drag) .thumb, .slide:not(.drag) .fill { transition: transform 0.2s, width 0.2s; }
  .status { min-height: 1.4em; padding: 4px 12px; text-align: center; font-weight: 500; }
  .row { display: flex; justify-content: space-evenly; padding: 8px 12px 20px; }
  .row button { width: 76px; height: 76px; border-radius: 50%; border: 0; cursor: pointer; color: #fff;
        display: flex; align-items: center; justify-content: center; }
  .row button ha-icon { --mdc-icon-size: 36px; }
  .row button:active { filter: brightness(0.85); }
  .answer { background: #43a047; }
  .hangup { background: #e53935; }
  .hangup.local { background: #757575; }
  .answer.on { box-shadow: 0 0 0 5px rgba(67,160,71,0.35); }
  .answer.ring { animation: ring 0.8s ease-in-out infinite alternate; }
  @keyframes ring { from { transform: scale(1); } to { transform: scale(1.12); } }
  .err { padding: 12px; color: var(--error-color, red); }
`;

// The WebRTC card's own style: as in door_views.yaml (no fullscreen / picture-in-picture buttons).
const CAM_STYLE = `
  .fullscreen { display: none !important; }
  .pictureinpicture { display: none !important; }
  .volume { --mdc-icon-size: 50px !important; width: 50px !important; color: white !important;
            background: rgba(100,0,0,0.65) !important; border-radius: 50%; }
`;

if (typeof customElements !== 'undefined' && !customElements.get('vimar-door-card')) {
  class VimarDoorCard extends HTMLElement {
    setConfig(config) {
      this._cfg = Object.assign({}, DEFAULTS, config);
      this._m = {live: false, myCall: false};
      this._key = '';
      this._cam = null;
      if (this.shadowRoot) this._build();
    }

    getCardSize() { return 9; }

    connectedCallback() {
      if (!this.shadowRoot) { this.attachShadow({mode: 'open'}); this._build(); }
      this._auto = true;
      if (this._hass) this._update();
    }

    disconnectedCallback() {                 // view left: stop the stream; re-check the call state on return
      this._m = {live: false, myCall: false};
      this._key = '';
      this._auto = false;
      this._show();
    }

    set hass(hass) {
      this._hass = hass;
      if (this._cam && this._m.live) this._cam.hass = hass;
      if (this.isConnected && this.shadowRoot) this._update();
    }

    _state() { const s = this._hass.states[this._cfg.call_state]; return s ? s.state : 'unknown'; }
    _owner() { const s = this._hass.states[this._cfg.owner]; return s ? s.state : ''; }
    _prio() { const s = this._hass.states[this._cfg.priority]; return s ? s.state : ''; }
    _me() { return (this._hass.user && this._hass.user.id) || ''; }

    _build() {
      const r = this.shadowRoot;
      r.innerHTML = `<style>${CSS}</style>
        <ha-card>
          <div class="video"><button class="ph"><ha-icon icon="mdi:play-circle-outline"></ha-icon>
            <span>Start video</span></button></div>
          <div class="slide"><div class="fill"></div><div class="label">Slide to open the gate</div>
            <div class="thumb"><ha-icon icon="mdi:key-variant"></ha-icon></div></div>
          <div class="status"></div>
          <div class="row">
            <button class="answer" aria-label="Answer"><ha-icon icon="mdi:phone"></ha-icon></button>
            <button class="hangup" aria-label="Hang up"><ha-icon icon="mdi:phone-hangup"></ha-icon></button>
          </div>
        </ha-card>`;
      r.querySelector('.ph').addEventListener('click', () => this._start());
      r.querySelector('.answer').addEventListener('click', () => this._answer());
      r.querySelector('.hangup').addEventListener('click', () => this._hangup());
      this._slider(r.querySelector('.slide'));
    }

    /** Slide-to-open: the gate command is sent only when the thumb is released at the right end. */
    _slider(el) {
      const thumb = el.querySelector('.thumb'), fill = el.querySelector('.fill'), label = el.querySelector('.label');
      let x0 = 0, max = 0, dx = 0, on = false;
      const set = v => { thumb.style.transform = `translateX(${v}px)`; fill.style.width = v ? `${v + 52}px` : '0'; };
      const end = fire => {
        if (!on) return;
        on = false;
        el.classList.remove('drag');
        if (fire && max > 0 && dx >= max * 0.9) {
          this._gate();
          el.classList.add('done');
          label.textContent = 'Gate command sent';
          set(max);
          setTimeout(() => { el.classList.remove('done'); label.textContent = 'Slide to open the gate'; set(0); }, 1500);
        } else set(0);
      };
      thumb.addEventListener('pointerdown', ev => {
        if (el.classList.contains('done')) return;
        on = true; dx = 0; x0 = ev.clientX; max = el.clientWidth - thumb.offsetWidth - 8;
        el.classList.add('drag');
        try { thumb.setPointerCapture(ev.pointerId); } catch (e) { /* ignore */ }
        ev.preventDefault();
      });
      thumb.addEventListener('pointermove', ev => {
        if (!on) return;
        dx = Math.max(0, Math.min(max, ev.clientX - x0));
        set(dx);
      });
      thumb.addEventListener('pointerup', () => end(true));
      thumb.addEventListener('pointercancel', () => end(false));
    }

    /** React to a HA state update. Does nothing unless call state, owner or the stream flag changed. */
    _update() {
      const st = this._state(), owner = this._owner();
      const prio = this._prio();
      if (this._prioSeen !== undefined && prio !== this._prioSeen && this._m.live) {
        this._m = {live: false, myCall: false};  // T57: swipe on the monitor: it has priority (R3, G6)
      }
      this._prioSeen = prio;
      if (this._auto) {                        // first update after the view was opened
        this._auto = false;
        if (autoStartAllowed(this._cfg, st) && !priorityRecent(prio, Date.now())) this._m = {live: true, myCall: this._m.myCall};
      }
      // T56: a ring that ends unanswered (ringing / answered_app -> idle) leaves the outdoor camera off while the
      // LAN session stays open: black video for as long as the view stays open. Close it like the monitor does.
      if (this._m.live && !this._m.myCall && st === 'idle' && (this._prevSt === 'ringing' || this._prevSt === 'answered_app')) {
        this._m = {live: false, myCall: false};
      }
      this._prevSt = st;
      this._m = onState(this._m, st, owner, this._me(), !!this._cfg.autostart_ringing);
      const key = st + '|' + owner + '|' + this._m.live;
      if (key === this._key) return;
      this._key = key;
      const r = this.shadowRoot;
      const answer = r.querySelector('.answer');
      answer.classList.toggle('on', st === 'answered_HA');
      answer.classList.toggle('ring', st === 'ringing');
      const ends = hangupPublishes(st, owner, this._me(), this._m.myCall);
      const hb = r.querySelector('.hangup');
      hb.classList.toggle('local', !ends);
      const can = startAllowed(this._cfg, st), ph = r.querySelector('.ph');
      ph.disabled = !can;
      ph.classList.toggle('off', !can);
      ph.querySelector('ha-icon').setAttribute('icon', can ? 'mdi:play-circle-outline' : 'mdi:video-off-outline');
      ph.querySelector('span').textContent = can ? 'Start video' : st === 'ringing' ? 'Video after Answer' : 'No video';
      hb.setAttribute('aria-label', ends ? 'Hang up' : 'Close video');
      hb.title = ends ? 'Hang up' : 'Close video (the call goes on)';
      const persons = Object.values(this._hass.states)
        .filter(s => s.entity_id.startsWith('person.'))
        .map(s => ({user_id: s.attributes.user_id, name: s.attributes.friendly_name || s.entity_id}));
      r.querySelector('.status').textContent = statusText(st, owner, persons);
      this._show();
    }

    /** Put the WebRTC card or the placeholder into the video area, according to this._m.live. */
    _show() {
      if (!this.shadowRoot) return;
      const box = this.shadowRoot.querySelector('.video');
      const ph = box.querySelector('.ph');
      if (!this._m.live) {
        if (this._cam && this._cam.parentNode) this._cam.remove();   // WebRTC card closes after its 5 s delay
        box.classList.remove('live');
        ph.hidden = false;
        const err = box.querySelector('.err'); if (err) err.remove();
        return;
      }
      if (this._cam && this._cam.parentNode) return;
      if (!customElements.get('webrtc-camera')) {
        // T53: opened from the notification, the app starts cold and this card can be ready before the WebRTC
        // card's resource has loaded. Wait for it instead of giving up (before: error until the next update).
        if (!box.querySelector('.err')) {
          const e = document.createElement('div');
          e.className = 'err';
          e.textContent = 'Loading the WebRTC Camera card…';
          box.appendChild(e);
        }
        if (!this._waitCam) {
          this._waitCam = true;
          customElements.whenDefined('webrtc-camera').then(() => {
            this._waitCam = false;
            const err = box.querySelector('.err'); if (err) err.remove();
            if (this._m.live && this.isConnected) this._show();
          });
        }
        return;
      }
      if (!this._cam) {                       // one element, reused: a quick stop/start keeps one connection
        this._cam = document.createElement('webrtc-camera');
        this._cam.setConfig({
          streams: [{url: this._cfg.stream, mode: this._cfg.mode, media: this._cfg.media}],
          muted: this._cfg.muted, ui: true, style: CAM_STYLE,
        });
      }
      this._cam.hass = this._hass;
      ph.hidden = true;
      box.classList.add('live');
      box.appendChild(this._cam);
    }

    _start() {
      if (!startAllowed(this._cfg, this._state())) return;
      this._m = {live: true, myCall: this._m.myCall};
      this._key = '';
      this._update();
    }

    _answer() {
      // If the video already plays muted (autoplay refused sound), this tap may switch the sound on.
      try { if (this._cam && this._cam.video && this._cam.video.muted) this._cam.video.muted = false; } catch (e) { /* ignore */ }
      // The stream is not opened here: it opens when the bridge reports answered_HA for this user (onState),
      // so a live session never starts during the ring (T41: mainapp would treat it as an app answer).
      this._hass.callService('script', this._cfg.answer_script, {user: this._me()});
    }

    _hangup() {
      if (hangupPublishes(this._state(), this._owner(), this._me(), this._m.myCall)) {
        this._hass.callService('mqtt', 'publish', {topic: this._cfg.topic + '/end_remote', payload: 'PRESS'});
      }
      this._m = {live: false, myCall: this._m.myCall};
      this._key = '';
      this._update();
    }

    _gate() {
      this._hass.callService('mqtt', 'publish', {topic: this._cfg.topic + '/open_gate', payload: 'PRESS'});
    }
  }

  customElements.define('vimar-door-card', VimarDoorCard);
  window.customCards = window.customCards || [];
  window.customCards.push({type: 'vimar-door-card', name: 'Vimar door card',
    description: 'Vimar K40955: video, slide to open the gate, Answer / Hang up in one card'});
}
