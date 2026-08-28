/* SLWF-12 web interface.
 *
 * Deliberately dependency-free. Everything here is served from a 2 MB LittleFS
 * partition by a single-threaded async web server, so there is no bundler, no
 * framework and no CDN — a CDN would also break the moment the device is used
 * on a network without internet access, which is exactly when you most want
 * the setup page to work.
 */
'use strict';

const REDACTED = '••••••••';

const state = {
  status: null,
  config: null,
  ac: null,
  learning: null,
  schedules: [],
  strings: {},
  language: 'en',
  logSeq: 0,
  socket: null,
  dbIndex: null,
};

/* ══════════════════════════════════ helpers ══════════════════════════════ */

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (value !== null && value !== undefined) node.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined) continue;
    node.append(child.nodeType ? child : document.createTextNode(child));
  }
  return node;
}

/* Line icons. Emoji came free but they are somebody else's drawing: a
 * different shape, weight and colour on every platform, and they read as
 * decoration. These are one stroke weight, they take the surrounding colour,
 * and they cost about a line each.
 *
 * A device type names an icon rather than carrying one, so the type database
 * stays a description of hardware. A name with no drawing here falls back to
 * a plain box instead of vanishing. */
const ICONS = {
  climate: 'M12 2.5v19M4.2 7l15.6 9M19.8 7L4.2 16M12 6.5L9.4 4M12 6.5L14.6 4M12 17.5l-2.6 2.5M12 17.5l2.6 2.5',
  light: 'M9.5 20h5M10.5 22.5h3M12 2.5a6 6 0 0 1 3.6 10.8c-.6.4-.9 1-.9 1.7H9.3c0-.7-.3-1.3-.9-1.7A6 6 0 0 1 12 2.5z',
  display: 'M3 4.5h18v12H3zM9 20.5h6M12 16.5v4',
  device: 'M5 3.5h14v17H5zM9 7.5h6M9 11.5h6',
  globe: 'M12 2.5a9.5 9.5 0 1 0 0 19 9.5 9.5 0 0 0 0-19zM2.5 12h19M12 2.5c2.5 2.6 3.8 5.8 3.8 9.5s-1.3 6.9-3.8 9.5M12 2.5C9.5 5.1 8.2 8.3 8.2 12s1.3 6.9 3.8 9.5',
  thermometer: 'M14 13.6V6a2 2 0 1 0-4 0v7.6a4 4 0 1 0 4 0zM12 9.5v6',
  sun: 'M12 7.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM12 2v2.5M12 19.5V22M4.2 4.2l1.8 1.8M18 18l1.8 1.8M2 12h2.5M19.5 12H22M4.2 19.8L6 18M18 6l1.8-1.8',
  link: 'M9.5 14.5l5-5M10.8 6.6l1.6-1.6a3.8 3.8 0 0 1 5.4 5.4l-1.6 1.6M8.2 12l-1.6 1.6a3.8 3.8 0 0 0 5.4 5.4l1.6-1.6',
  plug: 'M9 2.5v6M15 2.5v6M6.5 8.5h11v2.5a5.5 5.5 0 0 1-11 0zM12 16.5v5',
  wifi: 'M2.5 8.8a15 15 0 0 1 19 0M6 12.4a10 10 0 0 1 12 0M9.4 16a5 5 0 0 1 5.2 0M12 19.5h.01',
  lock: 'M7.5 10.5V7a4.5 4.5 0 0 1 9 0v3.5M5 10.5h14v10H5z',
  chip: 'M6.5 6.5h11v11h-11zM10 3.5v3M14 3.5v3M10 17.5v3M14 17.5v3M3.5 10h3M3.5 14h3M17.5 10h3M17.5 14h3',
  clock: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 7v5.2l3.4 2',
  update: 'M12 19V4.5M6.5 10L12 4.5 17.5 10M4 21h16',
  backup: 'M4 4h12.5L20 7.5V20H4zM8 4v6h8V4M8 20v-6h8v6',
  log: 'M4 6h16M4 10h16M4 14h11M4 18h8',
  bolt: 'M13.5 2.5L4.5 14h6.5l-1 7.5 9-11.5H13z',
  scenes: 'M4 7h9M17 7h3M4 17h4M12 17h8M15 4.8v4.4M8 14.8v4.4',
  calendar: 'M4 5.5h16v15H4zM4 9.5h16M8 3v4M16 3v4',
  book: 'M4 4.5h7.5a2.5 2.5 0 0 1 2.5 2.5v13H6.5A2.5 2.5 0 0 1 4 17.5zM20 4.5h-3.5A2.5 2.5 0 0 0 14 7v13h3.5a2.5 2.5 0 0 1 2.5 2.5z',
  code: 'M9 18l-6-6 6-6M15 6l6 6-6 6',
  facebook: 'M14.5 3.5h-2A3.5 3.5 0 0 0 9 7v2.5H6.5v3.5H9v8h3.5v-8h2.8l.7-3.5h-3.5V7c0-.3.2-.5.5-.5h2z',
  instagram: 'M7.5 3.5h9a4 4 0 0 1 4 4v9a4 4 0 0 1-4 4h-9a4 4 0 0 1-4-4v-9a4 4 0 0 1 4-4zM12 8.2a3.8 3.8 0 1 0 0 7.6 3.8 3.8 0 0 0 0-7.6zM17.2 6.6h.01',
  x: 'M4 4l7.2 8.6M4 20l6.6-7M13.4 11.4L20 4M12.8 11.4L20 20',
  youtube: 'M2.5 8.5a3 3 0 0 1 3-3h13a3 3 0 0 1 3 3v7a3 3 0 0 1-3 3h-13a3 3 0 0 1-3-3zM10 9.4l5 2.6-5 2.6z',
};

function icon(name, className = 'icon') {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('class', className);
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('d', ICONS[name] || ICONS.device);
  svg.append(path);
  return svg;
}

function toast(message, kind = '') {
  const node = el('div', { class: `toast ${kind}`, text: message });
  $('#toasts').append(node);
  setTimeout(() => node.remove(), kind === 'error' ? 6000 : 3000);
}

/* Translation. Missing keys fall back to the key itself, which makes gaps in a
 * language pack obvious rather than invisible. */
function t(key, fallback) {
  return state.strings[key] || fallback || key;
}

async function api(path, options = {}) {
  const init = { headers: { 'X-Requested-With': 'slwf12' }, ...options };
  if (init.body !== undefined && typeof init.body !== 'string') {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(init.body);
    init.method = init.method || 'POST';
  }
  const response = await fetch(path, init);
  const text = await response.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = { raw: text }; }

  if (!response.ok) {
    const message = (data && (data.error || data.code)) || `HTTP ${response.status}`;
    throw new Error(message);
  }
  return data;
}

/* ══════════════════════════════════ theme ════════════════════════════════ */

/* Three states rather than two: somebody who has their phone on a light/dark
 * schedule wants the panel to follow it, and somebody mounting this on a wall
 * in a dark room wants it pinned. "auto" is the default and is resolved by the
 * inline script in <head> before first paint. */
const THEME_ORDER = ['auto', 'light', 'dark'];

function applyTheme(choice, animate) {
  const dark = choice === 'dark' ||
    (choice === 'auto' && matchMedia('(prefers-color-scheme: dark)').matches);

  if (animate) {
    document.documentElement.setAttribute('data-theme-animating', '');
    setTimeout(() => document.documentElement.removeAttribute('data-theme-animating'), 250);
  }
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  document.documentElement.dataset.themeChoice = choice;
  localStorage.setItem('slwf12.theme', choice);

  const meta = document.querySelector('meta[name="theme-color"]');
  if (meta) meta.content = dark ? '#0e1014' : '#f2f4f8';
}

function initTheme() {
  // ?theme=dark pins the theme for this browser. Handy for a wall-mounted
  // tablet, for a bookmark, and for taking documentation screenshots.
  const requested = new URLSearchParams(location.search).get('theme');
  if (THEME_ORDER.includes(requested)) localStorage.setItem('slwf12.theme', requested);

  const stored = localStorage.getItem('slwf12.theme') || 'auto';
  applyTheme(stored, false);

  $('#theme-toggle').addEventListener('click', () => {
    const current = localStorage.getItem('slwf12.theme') || 'auto';
    const next = THEME_ORDER[(THEME_ORDER.indexOf(current) + 1) % THEME_ORDER.length];
    applyTheme(next, true);
    toast(t(`theme.${next}`, next));
  });

  // Follow the system while the choice is "auto".
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    if ((localStorage.getItem('slwf12.theme') || 'auto') === 'auto') {
      applyTheme('auto', true);
    }
  });
}

/* ══════════════════════════════════ i18n ═════════════════════════════════ */

async function loadLanguage(code) {
  try {
    const response = await fetch(`/lang/${code}.json`);
    if (!response.ok) throw new Error('missing');
    state.strings = await response.json();
    state.language = code;
    localStorage.setItem('slwf12.lang', code);
    renderLanguageButton();
  } catch {
    if (code !== 'en') return loadLanguage('en');
    state.strings = {};
  }
  applyTranslations();
}

function applyTranslations() {
  document.documentElement.lang = state.language;
  $$('[data-i18n]').forEach((node) => {
    const value = state.strings[node.dataset.i18n];
    if (value) node.textContent = value;
  });
  $$('[data-i18n-placeholder]').forEach((node) => {
    const value = state.strings[node.dataset.i18nPlaceholder];
    if (value) node.placeholder = value;
  });
  renderAll();
}

/* Where language packs come from. English is in the filesystem image; every
 * other pack is a file fetched from here — by the *browser*, and only when
 * somebody presses a button that says so.
 *
 * The device never opens a connection to anything on its own. That is a
 * deliberate line: a thing on your wall should not be talking to servers you
 * did not ask it to talk to. It also happens to be the practical choice —
 * the browser is nearly always the one with a route out, and one TLS session
 * costs most of this chip's free heap.
 */
const LANG_SOURCE =
    'https://raw.githubusercontent.com/smlight-tech/ir-remote-control/main/web/lang';

function languageName(code) {
  const known = [...(state.languageNames || []), ...(state.languagesAvailable || [])];
  const entry = known.find((one) => one.code === code);
  return (entry && entry.name) || code.toUpperCase();
}

function languageInstalled(code) {
  return (state.installedLanguages || ['en']).includes(code);
}

/* Sends a pack to the device. Whatever it came from — GitHub, or a file on
 * somebody's disk — this is the only thing that writes one. */
async function installLanguage(code, strings) {
  if (!strings || typeof strings !== 'object' || Array.isArray(strings)) {
    throw new Error(t('lang.not_a_pack', 'That file is not a language pack'));
  }
  // Re-serialised compactly: the source is indented for people to read, and
  // that indentation is a third of the bytes crossing the wire.
  const response = await fetch(
    `/api/lang/install?code=${encodeURIComponent(code)}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'slwf12' },
    body: JSON.stringify(strings),
  });
  if (!response.ok) {
    let message = `HTTP ${response.status}`;
    try { message = (await response.json()).error || message; } catch { /* not JSON */ }
    throw new Error(message);
  }
  state.installedLanguages =
      [...new Set([...(state.installedLanguages || []), code])];
  if (!(state.languageNames || []).some((one) => one.code === code)) {
    state.languageNames = [...(state.languageNames || []),
                           { code, name: strings['lang.name'] || code }];
  }
  renderLanguageButton();
  renderLocalisation();
}

/* The only thing here that reaches outside the local network, and it happens
 * once, from this browser, after somebody says yes. */
async function lookForLanguages() {
  const agreed = window.confirm(t('lang.look_ask',
    'This browser will now connect to GitHub to fetch the list of available '
    + 'languages. The device itself will not connect to anything. Continue?'));
  if (!agreed) return;

  toast(t('lang.looking', 'Looking…'));
  try {
    const response = await fetch(`${LANG_SOURCE}/index.json`);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    state.languagesAvailable = await response.json();
    renderLocalisation();
    const count = (state.languagesAvailable || []).length;
    toast(`${count} ${t('lang.found', 'languages available')}`, 'ok');
  } catch (error) {
    toast(`${t('lang.look_failed', 'Could not reach GitHub')}: ${error.message}`,
          'error');
  }
}

async function downloadLanguage(code) {
  toast(t('lang.downloading', 'Downloading…'));
  const response = await fetch(`${LANG_SOURCE}/${code}.json`);
  if (!response.ok) {
    throw new Error(`${t('lang.no_pack', 'It could not be downloaded')} ` +
                    `(HTTP ${response.status})`);
  }
  await installLanguage(code, await response.json());
  toast(t('lang.installed', 'Language installed'), 'ok');
}

/* The corner button: which language is showing, and the way to change it. */
function renderLanguageButton() {
  const button = $('#lang-button');
  if (!button) return;
  button.textContent = (state.language || 'en').toUpperCase();
  button.title = `${languageName(state.language || 'en')} — ` +
                 t('lang.title', 'Language');
}

async function useLanguage(code) {
  try {
    if (!languageInstalled(code)) await downloadLanguage(code);
    await loadLanguage(code);
    renderLanguageButton();
    renderLocalisation();
    api('/api/config', { body: { device: { language: code } } })
        .catch(() => { /* the browser's choice stands either way */ });
  } catch (error) { toast(error.message, 'error'); }
}

async function removeLanguage(code) {
  if (!window.confirm(`${t('lang.remove_ask', 'Remove')} ${languageName(code)}?`)) return;
  try {
    await api('/api/lang/remove', { body: { code } });
    state.installedLanguages =
        (state.installedLanguages || []).filter((one) => one !== code);
    if (state.language === code) await loadLanguage('en');
    renderLanguageButton();
    renderLocalisation();
    toast(t('lang.removed', 'Language removed'), 'ok');
  } catch (error) { toast(error.message, 'error'); }
}

/* The way in for a browser that cannot reach GitHub either — on the device's
 * own setup network, or behind something that blocks it. */
async function installLanguageFromFile(file) {
  try {
    const code = file.name.replace(/\.json$/i, '').toLowerCase();
    await installLanguage(code, JSON.parse(await file.text()));
    toast(t('lang.installed', 'Language installed'), 'ok');
  } catch (error) { toast(error.message, 'error'); }
}

function renderLocalisation() {
  const installed = $('#lang-installed');
  if (!installed) return;

  installed.replaceChildren(...(state.installedLanguages || ['en']).map((code) => {
    const showing = code === state.language;
    return el('div', { class: 'row' },
      el('span', { class: 'grow', text: languageName(code) }),
      el('span', { class: 'tag', text: code }),
      showing
        ? el('span', { class: 'muted small', text: t('lang.showing', 'showing') })
        : el('button', { class: 'ghost', onclick: () => useLanguage(code) },
             t('lang.use', 'Use')),
      // English is what everything falls back to; a device with no language
      // at all would need reflashing to recover.
      code === 'en' ? null
        : el('button', { class: 'danger ghost',
                         onclick: () => removeLanguage(code) }, '×'));
  }));

  const available = $('#lang-available');
  const remote = (state.languagesAvailable || [])
      .filter((entry) => !languageInstalled(entry.code));

  if (state.languagesAvailable === undefined) {
    available.replaceChildren();      // nobody has asked yet
  } else if (remote.length === 0) {
    available.replaceChildren(el('p', { class: 'muted small',
      text: t('lang.all_here', 'Every available language is already installed.') }));
  } else {
    available.replaceChildren(...remote.map((entry) =>
      el('div', { class: 'row' },
        el('span', { class: 'grow', text: entry.name }),
        el('span', { class: 'tag', text: entry.code }),
        el('button', { class: 'ghost',
                       onclick: () => useLanguage(entry.code) },
           t('lang.download', 'Download')))));
  }
}

async function initLanguages() {
  let installed = ['en'];
  let names = [];
  try {
    const data = await api('/api/languages');
    if (data.languages && data.languages.length) installed = data.languages;
    // Names for the packs the device has, read from the device. Knowing what
    // "uk" is called needs no network — knowing what *else* exists does, and
    // that only happens when asked.
    names = data.catalogue || [];
  } catch { /* device may not have language packs installed */ }

  state.installedLanguages = installed;
  state.languageNames = names;
  state.languagesAvailable = undefined;   // nothing fetched, nothing implied

  const preferred = new URLSearchParams(location.search).get('lang') ||
                    localStorage.getItem('slwf12.lang') ||
                    (state.config && state.config.device.language) ||
                    navigator.language.slice(0, 2);
  const chosen = installed.includes(preferred) ? preferred : 'en';
  state.language = chosen;
  await loadLanguage(chosen);
  renderLanguageButton();
}


/* ══════════════════════════════ view routing ═════════════════════════════ */

/* Two of the three destinations hold more than one page, so they get a menu
 * down the left rather than a landing page you have to go back out of. A page
 * is written here once — icon, label, owning destination — and both the rail
 * and the tab highlighting read it from here.
 *
 * Everything on a page is visible at once: scrolling costs nothing, whereas a
 * closed section hides the very setting somebody came to check. */
const SECTIONS = {
  automations: [
    { page: 'automations', icon: 'bolt', key: 'nav.automations', label: 'Automations' },
    { page: 'scenes',      icon: 'scenes', key: 'scenes.title',    label: 'Scenes' },
    { page: 'schedules',   icon: 'calendar', key: 'schedules.title',  label: 'Schedules and timers' },
  ],
  settings: [
    { page: 'teach',    icon: 'climate', key: 'settings.ac',           label: 'Air conditioner' },
    { page: 'clients',  icon: 'plug', key: 'settings.integrations', label: 'Integrations' },
    { page: 'devices',  icon: 'link', key: 'settings.devices',      label: 'Other devices' },
    { page: 'network',  icon: 'wifi', key: 'settings.network',      label: 'Network' },
    { page: 'security', icon: 'lock', key: 'settings.security',     label: 'Security' },
    { page: 'system',   icon: 'chip', key: 'settings.system',       label: 'System' },
    { page: 'localisation', icon: 'globe', key: 'locale.title',      label: 'Localisation' },
    { page: 'time',     icon: 'clock', key: 'system.time',           label: 'Clock' },
    { page: 'firmware', icon: 'update', key: 'system.update',         label: 'Firmware update' },
    { page: 'backup',   icon: 'backup', key: 'settings.backup',       label: 'Backup and reset' },
    { page: 'logs',     icon: 'log', key: 'system.log',            label: 'Log' },
  ],
};

function sectionOf(page) {
  for (const [name, pages] of Object.entries(SECTIONS)) {
    if (pages.some((entry) => entry.page === page)) return name;
  }
  return null;
}

function renderRail(section, current) {
  const rail = $('#rail');
  rail.hidden = !section;
  if (!section) { rail.replaceChildren(); return; }

  rail.replaceChildren(...SECTIONS[section].map((entry) => {
    const button = el('button', {
      class: `rail-item${entry.page === current ? ' active' : ''}`,
      onclick: () => showView(entry.page),
    },
      icon(entry.icon, 'rail-icon'),
      el('span', { 'data-i18n': entry.key, text: t(entry.key, entry.label) }));
    return button;
  }));
}

/* The device's clock, not the browser's — they are often in different
 * timezones, and a schedule fires by the device's reckoning. The status
 * document carries the epoch and the offset in force at that instant; between
 * refreshes this ticks it forward locally rather than asking every second.
 *
 * Rendering in UTC after adding the offset is what keeps the browser's own
 * timezone out of it. */
function deviceNow() {
  if (!state.clock || !state.clock.synced) return null;
  const elapsed = Date.now() - state.clock.receivedAt;
  return new Date((state.clock.epoch * 1000) + elapsed +
                  (state.clock.offset * 1000));
}

/* How this device writes a clock time and a date. The preference lives on
 * the device rather than in the browser so every client agrees — and so a
 * phone in another country still reads the device's own convention. */
const pad2 = (n) => String(n).padStart(2, '0');

function localeSettings() {
  const device = (state.config && state.config.device) || {};
  return {
    hour12: !!device.hour12,
    dateFormat: device.dateFormat || 'iso',
    weekStart: device.weekStart === 0 ? 0 : 1,
  };
}

function formatClock(hours, minutes, seconds) {
  const { hour12 } = localeSettings();
  const tail = seconds === undefined ? '' : `:${pad2(seconds)}`;
  if (!hour12) return `${pad2(hours)}:${pad2(minutes)}${tail}`;
  const suffix = hours < 12 ? 'am' : 'pm';
  const shown = hours % 12 === 0 ? 12 : hours % 12;
  return `${shown}:${pad2(minutes)}${tail} ${suffix}`;
}

function formatDate(year, month, day) {
  switch (localeSettings().dateFormat) {
    case 'dmy': return `${pad2(day)}/${pad2(month)}/${year}`;
    case 'mdy': return `${pad2(month)}/${pad2(day)}/${year}`;
    default:    return `${year}-${pad2(month)}-${pad2(day)}`;
  }
}

function formatDeviceTime(shape) {
  const now = deviceNow();
  if (!now) return null;
  const [y, mo, d] = [now.getUTCFullYear(), now.getUTCMonth() + 1, now.getUTCDate()];
  const [hh, mm, ss] = [now.getUTCHours(), now.getUTCMinutes(), now.getUTCSeconds()];

  if (shape === 'hhmm') return formatClock(hh, mm);
  // The one shape that is not for reading: <input type="datetime-local">
  // takes ISO and nothing else, whatever the page is set to show.
  if (shape === 'input') {
    return `${y}-${pad2(mo)}-${pad2(d)}T${pad2(hh)}:${pad2(mm)}:${pad2(ss)}`;
  }
  return `${formatDate(y, mo, d)} ${formatClock(hh, mm, ss)}`;
}

function tickClock() {
  const label = $('#clock-now');
  if (!label) return;
  const time = formatDeviceTime('hhmm');
  label.textContent = time || '--:--';
  label.classList.toggle('unset', !time);
  label.title = time
    ? `${formatDeviceTime('full')}\n${(state.clock && state.clock.tz) || ''}`
    : t('clock.unsynced', 'The device does not know what time it is');
  if ($('#view-time') && !$('#view-time').hidden) renderClock();
}

/* Common zones, and the POSIX string each one means. The device wants the
 * string; nobody should have to know that. Anything not listed can still be
 * typed in by hand, which is why the field stays. */
const TIMEZONES = [
  ['UTC', 'UTC0'],
  ['London', 'GMT0BST,M3.5.0/1,M10.5.0'],
  ['Lisbon', 'WET0WEST,M3.5.0/1,M10.5.0'],
  ['Berlin · Paris · Madrid · Warsaw', 'CET-1CEST,M3.5.0,M10.5.0/3'],
  ['Athens · Helsinki · Kyiv · Bucharest', 'EET-2EEST,M3.5.0/3,M10.5.0/4'],
  ['Istanbul', '<+03>-3'],
  ['Dubai', '<+04>-4'],
  ['Karachi', 'PKT-5'],
  ['Delhi', 'IST-5:30'],
  ['Bangkok · Jakarta', '<+07>-7'],
  ['Singapore · Hong Kong · Beijing', '<+08>-8'],
  ['Tokyo · Seoul', 'JST-9'],
  ['Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'],
  ['Auckland', 'NZST-12NZDT,M9.5.0,M4.1.0/3'],
  ['New York · Toronto', 'EST5EDT,M3.2.0,M11.1.0'],
  ['Chicago', 'CST6CDT,M3.2.0,M11.1.0'],
  ['Denver', 'MST7MDT,M3.2.0,M11.1.0'],
  ['Los Angeles · Vancouver', 'PST8PDT,M3.2.0,M11.1.0'],
  ['Sao Paulo · Buenos Aires', '<-03>3'],
  ['Johannesburg', 'SAST-2'],
];

function renderClock() {
  const clock = state.clock || {};
  const rows = [];
  const push = (label, value) =>
    rows.push(el('dt', { text: label }), el('dd', { text: String(value) }));

  push(t('clock.now', 'Now'),
       formatDeviceTime('full') || t('clock.unknown', 'not known'));
  push(t('clock.source', 'Set by'), clock.synced
    ? (clock.manual ? t('clock.by_hand', 'hand') : t('clock.by_ntp', 'time server'))
    : t('clock.by_nothing', 'nothing yet'));
  push(t('clock.zone_title', 'Timezone'), clock.tz || '—');

  // Sun times are only meaningful once somebody has said where the device is.
  const asClock = (minutes) => Number.isFinite(minutes) && minutes >= 0
    ? formatClock(Math.floor(minutes / 60), minutes % 60)
    : null;
  const sunrise = asClock(clock.sunrise);
  const sunset = asClock(clock.sunset);
  if (sunrise || sunset) {
    push(t('clock.sun', 'Sunrise · sunset'),
         `${sunrise || '—'} · ${sunset || '—'}` +
         (clock.daylight ? '' : ` · ${t('clock.dark', 'dark now')}`));
  }
  const info = $('#clock-info');
  if (info) info.replaceChildren(...rows);

  // Only seed the manual field while it is not being edited, so the value
  // does not jump out from under somebody halfway through typing it.
  const manual = $('#clock-manual');
  if (manual && document.activeElement !== manual && !manual.dataset.touched) {
    manual.value = formatDeviceTime('input') || '';
  }
}

/* A device that has never reached a time server has no idea what time it is,
 * and until it does its schedules and timers cannot run at all. The browser
 * looking at it does know — so it says so, once, rather than leaving somebody
 * to notice the clock is blank and go and find the right page.
 *
 * Once per page load: if the device still reports no time afterwards, that is
 * a failure to fix by asking again every fifteen seconds. */
let clockSeeded = false;

async function seedClockFromBrowser() {
  if (clockSeeded || !state.clock || state.clock.synced) return;
  clockSeeded = true;
  try {
    // Epoch is UTC, so no timezone arithmetic — the device applies its own.
    await api('/api/time', { method: 'POST',
                             body: { epoch: Math.round(Date.now() / 1000) } });
    toast(t('clock.auto_set', 'Clock set from this browser'), 'ok');
    await refreshStatus();
  } catch {
    // Not signed in, or the device refused it. The Clock page still offers
    // the same thing with a button behind it.
  }
}

async function applyManualTime() {
  const field = $('#clock-manual');
  const value = field.value;
  if (!value) { toast(t('clock.pick_first', 'Choose a date and time first'), 'error'); return; }

  // datetime-local has no timezone, and the device wants an epoch. The value
  // typed is the device's local time, so undo the device's offset to get UTC.
  const [date, time] = value.split('T');
  const [y, mo, d] = date.split('-').map(Number);
  const [hh, mm, ss] = time.split(':').map(Number);
  const asUtc = Date.UTC(y, mo - 1, d, hh, mm, ss || 0) / 1000;
  const offset = (state.clock && state.clock.offset) || 0;

  try {
    await api('/api/time', { method: 'POST', body: { epoch: Math.round(asUtc - offset) } });
    field.dataset.touched = '';
    toast(t('clock.set_ok', 'Clock set'), 'ok');
    await refreshStatus();
  } catch (error) { toast(error.message, 'error'); }
}

/* Where the product lives. One table, because these are the strings most
 * likely to need correcting later and hunting them through the markup is how
 * you end up with three of them out of date. An empty address is simply not
 * shown, so a network the product is not on costs nothing to leave out. */
const LINKS = {
  docs: 'https://github.com/smlight-tech/ir-remote-control/tree/main/docs',
  // GPL-3 asks that an interactive program tell the people using it what it
  // is and where the terms are. One link in the footer does that.
  licence: 'https://github.com/smlight-tech/ir-remote-control/blob/main/LICENSE',
  facebook: 'https://www.facebook.com/smlight.official',
  instagram: 'https://www.instagram.com/smlight.tech',
  x: 'https://x.com/smlight_tech',
  youtube: 'https://www.youtube.com/@smlight-tech',
};

/* The footer: what is on this device, and where to go next.
 *
 * The program and the interface are two images flashed separately — updating
 * one without the other is normal, not an error — so both versions are here,
 * alongside the settings format, which is what tells you whether a backup
 * taken from an older build will still read. */
function renderFooter() {
  const footer = $('#footer');
  const device = (state.status && state.status.device) || null;
  footer.hidden = false;

  const fact = (label, value, title) =>
    value ? el('span', { class: 'footer-fact', title: title || '' },
                el('span', { class: 'footer-label', text: label }),
                el('span', { class: 'footer-value', text: value })) : null;

  // Kept short on purpose: what the device is, and what is running on it.
  // The interface build, chip id and the rest stay on the System page, which
  // is where you go when you actually need them.
  const facts = el('div', { class: 'footer-facts' },
    fact(t('footer.device', 'Model'), device && device.model),
    fact(t('system.firmware', 'Firmware'),
         device && `${device.firmware}`, device ? device.commit : ''),
    fact(t('system.schema', 'Settings format'),
         device && device.schema ? `v${device.schema}` : ''));

  const link = (href, name, label) => href
    ? el('a', { href, target: '_blank', rel: 'noopener noreferrer',
                title: label, 'aria-label': label }, icon(name))
    : null;

  const links = el('div', { class: 'footer-links' },
    LINKS.docs ? el('a', { class: 'footer-text-link', href: LINKS.docs,
                           target: '_blank', rel: 'noopener noreferrer',
                           text: t('footer.manual', 'Manual') }) : null,
    LINKS.licence ? el('a', {
      class: 'footer-text-link', href: LINKS.licence,
      target: '_blank', rel: 'noopener noreferrer',
      title: t('footer.licence_title',
               'Free software, with no warranty. Click for the terms.'),
    }, 'GPL-3.0') : null,
    link(LINKS.facebook, 'facebook', 'Facebook'),
    link(LINKS.instagram, 'instagram', 'Instagram'),
    link(LINKS.x, 'x', 'X'),
    link(LINKS.youtube, 'youtube', 'YouTube'));

  footer.replaceChildren(facts, links);
}

function showView(name) {
  // A destination that is really a menu opens at its first page.
  if (SECTIONS[name]) name = SECTIONS[name][0].page;
  if (!document.getElementById(`view-${name}`)) name = 'control';

  $$('.view').forEach((view) => { view.hidden = view.id !== `view-${name}`; });

  const section = sectionOf(name);
  renderRail(section, name);
  $('main').classList.toggle('railed', !!section);
  $$('.tab').forEach((tab) =>
    tab.classList.toggle('active', tab.dataset.view === (section || name)));

  window.scrollTo(0, 0);
  location.hash = name;

  // Fetched when the page that needs it opens, one at a time. Overlapping
  // requests each want a response buffer, and on this device two at once is
  // the difference between a page and a reboot.
  if (name === 'teach') {
    (async () => {
      if (!$('#protocol-select').options.length) await refreshProtocols();
      await refreshIrLast();
      await refreshCodes();
      await refreshRemotes();
    })();
  }
  if (name === 'network') refreshWifi();
  // Leaving the page releases the scan results the Wi-Fi stack is holding.
  // They are a few kilobytes, which on this chip is a few kilobytes worth
  // caring about.
  else if (state.scannedOnce) {
    state.scannedOnce = false;
    api('/api/wifi/forget-scan', { body: {} }).catch(() => {});
  }
  if (name === 'devices') refreshPeers();
  if (name === 'scenes') refreshScenes();
  if (name === 'schedules') refreshSchedules();
  if (name === 'automations') { refreshPeers(); refreshScenes(); refreshAutomations(); }
  // The device streams the log only while somebody says they are reading it,
  // so leaving the page has to say so too.
  watchLog(name === 'logs');
  if (name === 'logs') refreshLog(true);
  if (name === 'time') renderClock();
  if (name === 'localisation') renderLocalisation();
}

/* A one-line answer to "is this bit working?", shown beside the heading. */
function setSectionNote(headingKey, text) {
  for (const heading of $$('h3[data-i18n]')) {
    if (heading.dataset.i18n !== headingKey) continue;
    let note = heading.querySelector('.summary-note');
    if (!note) {
      note = el('span', { class: 'summary-note' });
      heading.append(note);
    }
    note.textContent = text || '';
  }
}

/* ══════════════════════════════ climate card ═════════════════════════════ */

const MODES = ['auto', 'cool', 'heat', 'dry', 'fan_only'];
const FANS = ['auto', 'min', 'low', 'medium', 'medium_high', 'high', 'max'];
const SWINGS = ['off', 'auto', 'highest', 'high', 'middle', 'low', 'lowest'];

const MODE_COLOUR = {
  auto: 'var(--auto)', cool: 'var(--cool)', heat: 'var(--heat)',
  dry: 'var(--dry)', fan_only: 'var(--fan)',
};

async function send(delta) {
  try {
    const data = await api('/api/state', { body: delta });
    state.ac = data.state;
    if (data.result === 'deferred') {
      state.hold = data.hold || 0;
      toast(data.message || t('control.held', 'Held by compressor protection'));
    } else {
      state.hold = data.hold || 0;
    }
    renderClimate();
  } catch (error) {
    toast(error.message, 'error');
    refreshStatus();
  }
}

/* ══════════════════════════════════ scenes ═══════════════════════════════ */

function renderSceneChipsInto(row) {
  const active = state.status && state.status.scene;
  row.replaceChildren(...(state.scenes || []).map((scene) =>
    el('button', {
      class: 'chip' + (scene.id === active ? ' active' : ''),
      onclick: () => applyScene(scene.id),
    }, `${scene.icon || ''} ${scene.name}`.trim())));
}

async function applyScene(id) {
  try {
    const data = await api('/api/scenes/apply', { body: { id } });
    state.ac = data.state;
    state.hold = data.hold || 0;
    renderClimate();
    refreshStatus();
  } catch (error) { toast(error.message, 'error'); }
}

async function refreshScenes() {
  try {
    const data = await api('/api/scenes');
    state.scenes = data.scenes || [];
    renderSceneEditor();
    renderControl();
  } catch { /* optional */ }
}

function renderSceneEditor() {
  const rows = (state.scenes || []).map((scene, index) => {
    const icon = el('input', { type: 'text', value: scene.icon || '',
                               style: 'width:3.2rem;text-align:center' });
    icon.addEventListener('input', () => { scene.icon = icon.value; });

    const name = el('input', { type: 'text', value: scene.name || '' });
    name.addEventListener('input', () => { scene.name = name.value; });

    const action = el('input', { type: 'text', value: JSON.stringify(scene.action || {}) });
    action.addEventListener('input', () => {
      try { scene.action = JSON.parse(action.value); action.style.outline = ''; }
      catch { action.style.outline = '2px solid var(--danger)'; }
    });

    return el('div', { class: 'card', style: 'box-shadow:none;margin:.5rem 0' },
      el('div', { class: 'row' },
        icon,
        el('span', { class: 'grow' }, name),
        el('button', {
          class: 'ghost', onclick: () => applyScene(scene.id),
        }, t('scenes.test', 'Try it')),
        el('button', {
          class: 'danger ghost',
          onclick: () => { state.scenes.splice(index, 1); renderSceneEditor(); },
        }, '×')),
      el('label', {}, el('span', { text: t('scenes.action', 'What it sets (JSON)') }), action));
  });

  $('#scene-list').replaceChildren(...(rows.length ? rows :
    [el('p', { class: 'muted', text: t('scenes.empty', 'No scenes yet.') })]));
}

/* ══════════════════════════════════ usage ═══════════════════════════════ */

function renderStats() {
  const stats = state.status && state.status.stats;
  if (!stats) return;

  const tile = (value, label) => el('div', { class: 'tile' },
    el('div', { class: 'value', text: value }),
    el('div', { class: 'label', text: label }));

  const tiles = [
    tile(formatHours(stats.runtimeTodaySeconds), t('stats.today', 'Running today')),
    tile(formatHours(stats.runtimeSeconds), t('stats.total', 'Running total')),
    tile(String(stats.starts), t('stats.starts', 'Times started')),
  ];
  if (stats.energyKwh !== undefined) {
    tiles.push(tile(`${stats.energyKwh} kWh`, t('stats.energy', 'Energy (estimated)')));
  }
  $('#stat-tiles').replaceChildren(...tiles);
  $('#energy-note').hidden = stats.energyKwh === undefined;
}

function formatHours(seconds) {
  if (!seconds) return '0 m';
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.round((seconds % 3600) / 60);
  return hours ? `${hours} h ${minutes} m` : `${minutes} m`;
}

/* ══════════════════════════════ live updates ═════════════════════════════ */

/* The dot means "the device is answering", not "the WebSocket is up". Those
 * differ: a browser extension or a proxy can block the socket while ordinary
 * requests keep working perfectly, and a dot that says otherwise sends people
 * hunting for a fault that is not there. */
function setLink(online) {
  $('#link-dot').className = `dot ${online ? 'online' : 'offline'}`;
}

/* Tells the device whether this browser wants live log lines. Sent on opening
 * and leaving the page, and again whenever the socket reconnects — the device
 * forgets watchers when their socket goes. */
let wantsLog = false;

function watchLog(on) {
  wantsLog = on;
  const socket = state.socket;
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ t: 'log', on }));
  }
}

function connectSocket() {
  const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${protocol}://${location.host}/ws`);
  state.socket = socket;

  socket.addEventListener('open', () => {
    setLink(true);
    // A new socket is an unknown one to the device: say again whether this
    // browser is reading the log.
    if (wantsLog) socket.send(JSON.stringify({ t: 'log', on: true }));
  });
  socket.addEventListener('close', () => {
    // Do not declare the device dead on a dropped socket alone; the periodic
    // status poll is the authority.
    setTimeout(connectSocket, 3000);
  });
  socket.addEventListener('message', (event) => {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }

    if (message.state) { state.ac = message.state; renderClimate(); }
    if (message.learning) {
      const wasBinding = state.learning && state.learning.phase === 'bind';
      state.learning = message.learning;
      renderWizard();
      // A binding just finished: the list needs to show the new button.
      if (wasBinding && message.learning.phase !== 'bind') refreshRemotes();
    }
    if (message.t === 'log') appendLog(message.lines);
    if (message.t === 'notice') toast(t(message.message, message.message),
                                      message.level === 'error' ? 'error' : '');
    if (message.t === 'state' && message.source) {
      state.status && (state.status.lastSource = message.source);
      renderClimate();
    }
  });
}

/* ══════════════════════════════════ teach ════════════════════════════════ */

function renderWizard() {
  const learning = state.learning || { phase: 'idle', active: false };
  $('#wiz-phase').textContent = t(`phase.${learning.phase}`, learning.phase);
  $('#wiz-prompt').textContent = learning.prompt
    ? t(learning.prompt, learning.prompt)
    : (learning.message ? t(learning.message, learning.message) : '');

  let progress = '';
  if (learning.sweep) progress = `${learning.sweep.index}/${learning.sweep.total} · ${learning.sweep.protocol}`;
  if (learning.record) progress = `${learning.record.index}/${learning.record.total}`;
  $('#wiz-progress').textContent = progress;

  $('#wiz-target').textContent = learning.record && learning.record.target
    ? describeCodeKey(learning.record.target) : '';

  const actions = [];
  if (learning.phase === 'confirm' || learning.phase === 'sweep') {
    actions.push(el('button', { class: 'primary', onclick: () => confirmLearn(true) },
                    t('teach.yes', 'Yes, it reacted')));
    actions.push(el('button', { class: 'ghost', onclick: () => confirmLearn(false) },
                    t('teach.no', 'No, nothing happened')));
  }
  if (learning.phase === 'record') {
    actions.push(el('button', { class: 'ghost', onclick: () => learnAction('skip') },
                    t('teach.skip', 'Skip this one')));
  }
  if (learning.active) {
    actions.push(el('button', { class: 'danger ghost', onclick: () => learnAction('cancel') },
                    t('common.cancel', 'Cancel')));
  }
  if (learning.phase === 'failed') {
    actions.push(el('button', { class: 'ghost', onclick: () => startLearn('sweep') },
                    t('teach.sweep', 'Try every protocol')));
    actions.push(el('button', { class: 'ghost', onclick: () => startLearn('record') },
                    t('teach.record', 'Record raw codes')));
  }
  $('#wiz-actions').replaceChildren(...actions);
  $('#teach-start-row').hidden = !!learning.active;
}

async function startLearn(mode) {
  const body = { mode };
  if (mode === 'record') {
    const modes = prompt(t('teach.record_modes', 'Which modes? (comma separated)'), 'cool');
    if (modes === null) return;
    const range = prompt(t('teach.record_range', 'Temperature range, e.g. 18-26'), '18-26');
    if (range === null) return;
    const [min, max] = range.split('-').map((value) => parseFloat(value));
    body.plan = {
      modes: modes.split(',').map((value) => value.trim()).filter(Boolean),
      fans: ['auto'],
      minTemp: min, maxTemp: max, step: 1, includeOff: true,
    };
  }
  try {
    state.learning = await api('/api/learn/start', { body });
    renderWizard();
  } catch (error) { toast(error.message, 'error'); }
}

async function learnAction(action) {
  try {
    state.learning = await api(`/api/learn/${action}`, { method: 'POST', body: {} });
    renderWizard();
    refreshStatus();
    refreshRemotes();
  } catch (error) { toast(error.message, 'error'); }
}

async function confirmLearn(ok) {
  try {
    state.learning = await api('/api/learn/confirm', { body: { ok } });
    renderWizard();
    refreshStatus();
  } catch (error) { toast(error.message, 'error'); }
}

/* "cool_24_auto" reads better as "Cool · 24° · fan auto". */
function describeCodeKey(key) {
  if (key === 'off') return t('teach.key_off', 'Switch the air conditioner OFF');
  if (key.startsWith('btn_')) return `${t('teach.key_button', 'Button')}: ${key.slice(4)}`;
  const parts = key.split('_');
  const fan = parts.slice(2).join('_');
  return `${t(`mode.${parts[0]}`, parts[0])} · ${parts[1]}° · ${t(`fan.${fan}`, fan)}`;
}

async function refreshIrLast() {
  try {
    const data = await api('/api/ir/last');
    const rows = [];
    const push = (label, value) => rows.push(el('dt', { text: label }), el('dd', { text: value }));
    push(t('teach.captures', 'Frames heard'), data.captures);
    if (data.last) {
      push(t('teach.protocol', 'Protocol'), data.last.protocol);
      push(t('teach.bits', 'Bits'), data.last.bits);
      push(t('teach.marks', 'Timings'), data.last.marks);
      push(t('teach.decoded', 'Understood as a state'),
           data.last.decoded ? t('common.yes', 'yes') : t('common.no', 'no'));
      push(t('teach.synthesisable', 'Can be synthesised'),
           data.last.synthesisable ? t('common.yes', 'yes') : t('common.no', 'no'));
      if (data.last.overflow) push(t('teach.overflow', 'Warning'),
                                   t('teach.overflow_body', 'the frame was longer than the capture buffer'));
    } else {
      push(t('teach.nothing', 'Nothing yet'), t('teach.nothing_body', 'press a button on your remote'));
    }
    $('#ir-last').replaceChildren(...rows);
  } catch (error) { /* the panel is informational */ }
}

async function refreshProtocols() {
  try {
    const data = await api('/api/protocols');
    const select = $('#protocol-select');
    // The device sends plain names; older builds sent {id, name} objects.
    const names = (data.protocols || []).map(
      (entry) => (typeof entry === 'string' ? entry : entry.name));
    select.replaceChildren(
      el('option', { value: '', text: t('teach.pick_protocol', 'Choose a protocol…') }),
      ...names.map((name) => el('option', { value: name, text: name })));
    if (state.config) select.value = state.config.ac.protocol || '';
  } catch (error) {
    // Not optional after all: an empty list looks like the device supports
    // nothing, which is alarming and wrong. Say what happened.
    toast(`${t('teach.no_protocols', 'Could not load the protocol list')}: ` +
          error.message, 'error');
  }
}

async function refreshCodes() {
  try {
    const data = await api('/api/codes');
    const rows = data.codes.map((code) => el('div', { class: 'row' },
      el('span', { class: 'grow', text: describeCodeKey(code.key) }),
      el('span', { class: 'muted', text: `${code.marks}` }),
      el('button', {
        class: 'ghost', onclick: async () => {
          try { await api('/api/ir/send', { body: { key: code.key } }); toast(t('teach.sent', 'Sent'), 'ok'); }
          catch (error) { toast(error.message, 'error'); }
        },
      }, t('teach.play', 'Play')),
      el('button', {
        class: 'danger ghost', onclick: async () => {
          try { await api('/api/code/delete', { body: { key: code.key } }); refreshCodes(); }
          catch (error) { toast(error.message, 'error'); }
        },
      }, '×')));

    $('#codes-list').replaceChildren(...(rows.length ? rows :
      [el('p', { class: 'muted', text: t('teach.no_codes', 'No raw codes stored — the protocol is doing the work.') })]));
  } catch { /* optional */ }
}

/* ══════════════════════════ buttons on other remotes ═════════════════════ */

async function refreshRemotes() {
  try {
    const data = await api('/api/remotes');
    state.remoteActions = data.actions || [];
    state.remoteButtons = data.buttons || [];
    renderRemoteActions();
    renderRemoteList();
  } catch { /* optional panel */ }
}

function renderRemoteActions() {
  const select = $('#remote-action');
  if (select.options.length === state.remoteActions.length &&
      select.options.length > 0) {
    return;   // already built; keep the user's choice
  }
  select.replaceChildren(...state.remoteActions.map((action) =>
    el('option', { value: action, text: t(`action.${action}`, action) })));

  const scenes = $('#remote-scene');
  scenes.replaceChildren(...(state.scenes || []).map((scene) =>
    el('option', { value: scene.id, text: `${scene.icon || ''} ${scene.name}`.trim() })));

  // The scene picker only makes sense for the "apply a scene" action.
  const sync = () => { scenes.hidden = select.value !== 'scene'; };
  select.onchange = sync;
  sync();
}

function renderRemoteList() {
  const rows = (state.remoteButtons || []).map((button, index) =>
    el('div', { class: 'row' },
      el('span', { class: 'grow', text: button.label || t('remotes.unnamed', 'Unnamed button') }),
      el('span', { class: 'tag', text: t(`action.${button.action}`, button.action) }),
      el('span', { class: 'muted small', text: `${button.protocol} ${button.value}` }),
      el('button', {
        class: 'danger ghost',
        onclick: async () => {
          try {
            const data = await api('/api/remotes/delete', { body: { index } });
            state.remoteButtons = data.buttons || [];
            renderRemoteList();
          } catch (error) { toast(error.message, 'error'); }
        },
      }, '×')));

  $('#remote-list').replaceChildren(...(rows.length ? rows :
    [el('p', { class: 'muted', text: t('remotes.empty', 'No buttons bound yet.') })]));
}

async function startBind() {
  const action = $('#remote-action').value;
  const body = {
    mode: 'bind',
    action,
    label: $('#remote-label').value.trim(),
    argument: action === 'scene' ? $('#remote-scene').value : '',
  };
  try {
    state.learning = await api('/api/learn/start', { body });
    renderWizard();
    // The prompt lives in the wizard panel at the top of this page; without
    // this the user is left staring at a button that appears to do nothing.
    $('#wizard').scrollIntoView({ behavior: 'smooth', block: 'center' });
  } catch (error) { toast(error.message, 'error'); }
}

/* ── shared device database (fetched by the browser, not the device) ────── */

function dbBaseUrl() {
  const configured = state.config && state.config.cloud && state.config.cloud.dbUrl;
  return (configured && configured.trim()) ||
    'https://raw.githubusercontent.com/smlight-tech/ir-remote-control/main/codes';
}

async function loadDatabase() {
  try {
    const response = await fetch(`${dbBaseUrl()}/index.json`, { cache: 'no-cache' });
    state.dbIndex = await response.json();
    renderDatabase();
  } catch (error) {
    toast(t('teach.db_offline', 'Could not reach the database — this needs internet access.'), 'error');
  }
}

function renderDatabase() {
  const query = $('#db-search').value.trim().toLowerCase();
  const entries = ((state.dbIndex && state.dbIndex.profiles) || []).filter((entry) =>
    !query || `${entry.brand} ${entry.model} ${entry.protocol}`.toLowerCase().includes(query));

  $('#db-results').replaceChildren(...(entries.length ? entries.slice(0, 40).map((entry) =>
    el('div', { class: 'row' },
      el('span', { class: 'grow', text: `${entry.brand} ${entry.model}` }),
      el('span', { class: 'muted', text: entry.protocol || 'raw' }),
      el('button', { class: 'ghost', onclick: () => importProfile(entry) },
         t('teach.import', 'Import')))) :
    [el('p', { class: 'muted', text: t('teach.db_empty', 'No matching profiles.') })]));
}

async function importProfile(entry) {
  if (!confirm(t('teach.import_confirm', 'Replace the current air-conditioner configuration?'))) return;
  try {
    const response = await fetch(`${dbBaseUrl()}/${entry.file}`, { cache: 'no-cache' });
    const profile = await response.json();

    await api('/api/config', {
      body: {
        ac: {
          protocol: profile.protocol || '',
          model: profile.protocolModel ?? -1,
          brand: profile.brand || entry.brand,
          modelName: profile.model || entry.model,
          profileId: entry.id,
          minTemp: profile.minTemp ?? 16,
          maxTemp: profile.maxTemp ?? 30,
          tempStep: profile.tempStep ?? 1,
          useLearnedCodes: !!(profile.codes && profile.codes.length),
        },
      },
    });

    // Codes go up one at a time; a whole profile does not fit in the
    // device's RAM, and this also gives honest progress.
    const codes = profile.codes || [];
    for (let i = 0; i < codes.length; i++) {
      await api('/api/code', { body: codes[i] });
      toast(`${i + 1}/${codes.length}`);
    }

    toast(t('teach.import_done', 'Profile imported'), 'ok');
    await refreshStatus();
    await refreshConfig();
    refreshCodes();
  } catch (error) { toast(error.message, 'error'); }
}

async function buildProfileFile() {
  const profile = await api('/api/profile');
  const codes = [];
  for (const entry of profile.codes || []) {
    codes.push(await api(`/api/code?key=${encodeURIComponent(entry.key)}`));
  }
  profile.codes = codes;
  profile.submittedAt = new Date().toISOString().slice(0, 10);
  return profile;
}

async function exportProfile() {
  try {
    const profile = await buildProfileFile();
    const name = `${profile.brand || 'unknown'}-${profile.model || 'unknown'}`
      .toLowerCase().replace(/[^a-z0-9]+/g, '-');
    const blob = new Blob([JSON.stringify(profile, null, 1)], { type: 'application/json' });
    const link = el('a', { href: URL.createObjectURL(blob), download: `${name}.json` });
    link.click();
    URL.revokeObjectURL(link.href);
  } catch (error) { toast(error.message, 'error'); }
}

async function submitProfile() {
  try {
    const profile = await buildProfileFile();
    await exportProfile();
    const body = encodeURIComponent(
      `**Brand:** ${profile.brand}\n**Model:** ${profile.model}\n` +
      `**Protocol:** ${profile.protocol || '(raw codes only)'}\n` +
      `**Raw codes:** ${profile.codes.length}\n` +
      `**Firmware:** ${profile.firmware}\n\n` +
      'The profile file has been downloaded to this device — please attach it ' +
      'to this issue by dragging it into the comment box.\n');
    const title = encodeURIComponent(`AC profile: ${profile.brand} ${profile.model}`);
    window.open(
      `https://github.com/smlight-tech/ir-remote-control/issues/new?labels=ac-profile&title=${title}&body=${body}`,
      '_blank', 'noopener');
  } catch (error) { toast(error.message, 'error'); }
}

/* ══════════════════════════════════ clients ══════════════════════════════ */

/* Every client, in both directions. Most both accept commands and report
 * state, so only the exception is labelled — the webhook can never change
 * anything, and saying so here is better than leaving someone to wonder why
 * switching it on did not give them a way in. */
const CLIENTS = [
  { key: 'web' }, { key: 'api' }, { key: 'mqtt' }, { key: 'telegram' },
  { key: 'ir', note: 'direction.listens' },
  { key: 'uart' }, { key: 'modbus' },
  { key: 'schedule', note: 'direction.controls' },
  // Not a `source`: the webhook has its own settings block, because there is
  // no permission to grant — only somewhere to send.
  { key: 'webhook', note: 'direction.reports', config: 'webhook' },
];

function renderSources() {
  if (!state.config) return;
  const enabled = state.config.sources || {};

  $('#source-toggles').replaceChildren(...CLIENTS.map((client) => {
    const input = el('input', { type: 'checkbox' });
    input.checked = client.config
      ? !!(state.config[client.config] || {}).enabled
      : !!enabled[client.key];

    input.addEventListener('change', async () => {
      // A client with its own settings block toggles that; everything else is
      // a permission on the command bus.
      const patch = client.config
        ? { [client.config]: { enabled: input.checked } }
        : { sources: { [client.key]: input.checked } };
      try {
        const data = await api('/api/config', { body: patch });
        state.config = data.config;
        fillForms();
        toast(t('common.saved', 'Saved'), 'ok');
      } catch (error) {
        toast(error.message, 'error');
        input.checked = !input.checked;
      }
    });

    return el('label', { class: 'switch' }, input,
      el('span', {}, t(`source.${client.key}`, client.key)),
      client.note ? el('span', { class: 'tag', text: t(client.note, '') }) : null);
  }));
}

/* ══════════════════════════════════ network ══════════════════════════════ */

async function refreshWifi() {
  state.scannedOnce = true;
  try {
    const data = await api('/api/wifi/scan');
    const rows = (data.networks || []).map((network) => el('div', { class: 'row' },
      el('span', { class: 'grow', text: network.ssid || '(hidden)' }),
      el('span', { class: 'muted', text: `${t(`signal.${network.quality}`, network.quality)} · ${network.rssi} dBm` }),
      el('button', {
        class: 'ghost',
        onclick: () => { $('#wifi-ssid').value = network.ssid; $('#wifi-pass').focus(); },
      }, t('network.use', 'Use'))));
    $('#wifi-list').replaceChildren(...rows);
    if (data.scanning) setTimeout(refreshWifi, 1500);
  } catch (error) { toast(error.message, 'error'); }
}

/* ══════════════════════════════════ schedules ════════════════════════════ */

const DAY_KEYS = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];

async function refreshSchedules() {
  try {
    const data = await api('/api/schedules');
    state.schedules = data.schedules || [];
    $('#clock-line').textContent = data.timeSynced
      ? `${t('schedules.clock', 'Device clock')}: ${data.now}`
      : t('schedules.no_clock', 'The clock has not synchronised yet — schedules will not fire.');
    renderSchedules();
  } catch (error) { toast(error.message, 'error'); }
}

function renderSchedules() {
  const rows = state.schedules.map((rule, index) => {
    const enabled = el('input', { type: 'checkbox' });
    enabled.checked = rule.enabled !== false;
    enabled.addEventListener('change', () => { rule.enabled = enabled.checked; });

    const name = el('input', { type: 'text', value: rule.name || '' });
    name.addEventListener('input', () => { rule.name = name.value; });

    const when = rule.kind === 'timer'
      ? el('span', { class: 'muted', text: `${t('schedules.in', 'in')} ${Math.round((rule.inSeconds || 0) / 60)} min` })
      : (() => {
          const time = el('input', {
            type: 'time',
            value: `${String(rule.hour).padStart(2, '0')}:${String(rule.minute).padStart(2, '0')}`,
          });
          time.addEventListener('input', () => {
            const [hour, minute] = time.value.split(':').map(Number);
            rule.hour = hour; rule.minute = minute;
          });
          return time;
        })();

    const action = el('input', {
      type: 'text', value: JSON.stringify(rule.action || {}),
    });
    action.addEventListener('input', () => {
      try { rule.action = JSON.parse(action.value); action.style.outline = ''; }
      catch { action.style.outline = '2px solid var(--danger)'; }
    });

    // DAY_KEYS is indexed by the bit each day occupies, which is fixed by
    // the wire format; only the order they are *shown* in follows the
    // preference. Rotating the indices keeps the two from being confused.
    const order = localeSettings().weekStart === 1
      ? [1, 2, 3, 4, 5, 6, 0] : [0, 1, 2, 3, 4, 5, 6];
    const days = rule.kind === 'timer' ? null : el('div', { class: 'chip-row small' },
      ...order.map((bit) => {
        const key = DAY_KEYS[bit];
        const chip = el('button', {
          class: 'chip' + (((rule.days >> bit) & 1) ? ' active' : ''),
          'data-fan': 'x',
          onclick: () => {
            rule.days ^= (1 << bit);
            chip.classList.toggle('active');
          },
        }, t(`day.${key}`, key));
        return chip;
      }));

    return el('div', { class: 'card', style: 'box-shadow:none;margin:.5rem 0' },
      el('div', { class: 'row' },
        el('label', { class: 'switch' }, enabled, el('span', {}, '')),
        el('span', { class: 'grow' }, name),
        when,
        el('button', {
          class: 'danger ghost',
          onclick: () => { state.schedules.splice(index, 1); renderSchedules(); },
        }, '×')),
      days,
      el('label', {}, el('span', { text: t('schedules.action', 'Action (JSON)') }), action));
  });

  $('#schedule-list').replaceChildren(...(rows.length ? rows :
    [el('p', { class: 'muted', text: t('schedules.empty', 'No schedules yet.') })]));
}

/* ══════════════════════════════════ system ══════════════════════════════ */

function appendLog(lines) {
  if (!lines) return;
  const view = $('#log-view');
  const follow = $('#log-follow').checked;
  for (const line of lines.split('\n')) {
    if (!line.trim()) continue;
    const [, at, level, ...rest] = line.split('\t');
    const cls = { E: 'e', W: 'w', D: 'd' }[level] || '';
    view.append(el('span', { class: cls, text: `${(Number(at) / 1000).toFixed(1)}  ${rest.join(' ')}\n` }));
  }
  while (view.childNodes.length > 400) view.firstChild.remove();
  if (follow) view.scrollTop = view.scrollHeight;
}

async function refreshLog(reset = false) {
  try {
    if (reset) { $('#log-view').replaceChildren(); state.logSeq = 0; }
    const response = await fetch(`/api/log?since=${state.logSeq}`, {
      headers: { 'X-Requested-With': 'slwf12' },
    });
    state.logSeq = Number(response.headers.get('X-Log-Sequence') || state.logSeq);
    appendLog(await response.text());
  } catch { /* the log is informational */ }
}

function renderSystem() {
  const status = state.status;
  if (!status) return;

  const rows = [];
  const push = (label, value) => rows.push(el('dt', { text: label }), el('dd', { text: String(value) }));
  push(t('system.firmware', 'Firmware'), `${status.device.firmware} (${status.device.commit})`);
  push(t('system.built', 'Built'), status.device.built);
  // The interface is a second image on the same chip, updated on its own; when
  // a page looks stale this is the number that says whether it really is.
  push(t('system.interface', 'Interface'),
       `${status.device.web || '—'}${status.device.webBuilt ? ` · ${status.device.webBuilt}` : ''}`);
  push(t('system.schema', 'Settings format'), `v${status.device.schema}`);
  push(t('system.chip', 'Chip id'), status.device.id);
  push(t('system.uptime', 'Uptime'), formatDuration(status.device.uptime));
  push(t('system.heap', 'Free heap'),
       `${status.device.freeHeap} B · ${status.device.fragmentation}% ${t('system.fragmented', 'fragmented')}`);
  push(t('system.reset', 'Last reset'), status.device.resetReason);
  push(t('system.ir', 'IR'),
       `RX GPIO${status.ir.rxPin} / TX GPIO${status.ir.txPin} · ${status.ir.captures} ${t('system.in', 'in')} · ${status.ir.sends} ${t('system.out', 'out')}`);
  $('#sys-info').replaceChildren(...rows);
  renderFooter();

  if (status.clock) {
    state.clock = { ...status.clock, receivedAt: Date.now() };
    tickClock();
    seedClockFromBrowser();
  }

  const ota = [];
  const pushOta = (label, value) => ota.push(el('dt', { text: label }), el('dd', { text: String(value) }));
  pushOta(t('system.free_space', 'Free flash for updates'), `${Math.round(status.ota.freeSketchSpace / 1024)} kB`);
  pushOta(t('system.sketch_size', 'Program size'), `${Math.round(status.ota.sketchSize / 1024)} kB`);
  $('#ota-info').replaceChildren(...ota);
}

function formatDuration(seconds) {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days) return `${days}d ${hours}h`;
  if (hours) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function uploadFirmware() {
  const file = $('#ota-file').files[0];
  if (!file) { toast(t('system.pick_file', 'Choose a .bin file first'), 'error'); return; }

  const form = new FormData();
  form.append('firmware', file, file.name);

  const request = new XMLHttpRequest();
  const bar = $('#ota-progress');
  bar.hidden = false;

  request.upload.addEventListener('progress', (event) => {
    if (event.lengthComputable) {
      bar.firstElementChild.style.width = `${(event.loaded / event.total) * 100}%`;
    }
  });
  request.addEventListener('load', () => {
    let ok = false;
    try { ok = JSON.parse(request.responseText).ok; } catch { /* ignore */ }
    toast(ok ? t('system.update_ok', 'Update installed — restarting') :
               t('system.update_failed', 'Update failed'),
          ok ? 'ok' : 'error');
    if (ok) setTimeout(() => location.reload(), 8000);
  });
  request.addEventListener('error', () => toast(t('system.update_failed', 'Update failed'), 'error'));

  request.open('POST', '/api/ota/upload');
  request.setRequestHeader('X-Requested-With', 'slwf12');
  request.send(form);
}

/* ══════════════════════════════ config plumbing ══════════════════════════ */

/* Password inputs are pre-filled with the redaction sentinel. Sending it back
 * unchanged tells the device to keep whatever it already had, so a secret is
 * never round-tripped through the browser. */
function secretValue(input) {
  return input.value === REDACTED ? REDACTED : input.value;
}

function fillForms() {
  const config = state.config;
  if (!config) return;

  $('#device-name').textContent = config.device.name;
  $('#dev-name').value = config.device.name;

  const set = (id, value) => { const node = $(id); if (node) node.value = value ?? ''; };
  const check = (id, value) => { const node = $(id); if (node) node.checked = !!value; };

  set('#mqtt-host', config.mqtt.host); set('#mqtt-port', config.mqtt.port);
  set('#mqtt-user', config.mqtt.user); set('#mqtt-pass', config.mqtt.pass);
  set('#mqtt-base', config.mqtt.baseTopic); set('#mqtt-prefix', config.mqtt.discoveryPrefix);
  check('#mqtt-enabled', config.mqtt.enabled); check('#mqtt-discovery', config.mqtt.discovery);
  check('#mqtt-retain', config.mqtt.retain); check('#mqtt-homie', config.mqtt.homie);

  const hook = config.webhook || {};
  check('#hook-enabled', hook.enabled); set('#hook-url', hook.url);
  set('#hook-interval', hook.minIntervalSeconds); set('#hook-header', hook.headerName);
  set('#hook-value', hook.headerValue);

  const modbus = config.modbus || {};
  check('#modbus-enabled', modbus.enabled); set('#modbus-port', modbus.port);

  set('#tg-token', config.telegram.token); set('#tg-poll', config.telegram.pollSeconds);
  check('#tg-enabled', config.telegram.enabled); check('#tg-notify', config.telegram.notifyOnChange);
  check('#tg-open', config.telegram.openEnrolment);
  renderTelegramAllowed();

  check('#uart-enabled', config.uart.enabled); set('#uart-baud', config.uart.baud);
  check('#uart-events', config.uart.emitEvents);

  set('#net-hostname', config.wifi ? config.device.hostname : '');
  check('#net-static', config.wifi.useStatic);
  set('#net-ip', config.wifi.ip); set('#net-gw', config.wifi.gateway);
  set('#net-mask', config.wifi.mask); set('#net-dns', config.wifi.dns);

  check('#auth-enabled', config.auth.enabled); set('#auth-user', config.auth.user);
  set('#auth-pass', config.auth.pass); set('#auth-token', config.auth.token);
  set('#ap-pass', config.wifi.apPassword);

  set('#ac-brand', config.ac.brand); set('#ac-model', config.ac.modelName);
  set('#ac-min', config.ac.minTemp); set('#ac-max', config.ac.maxTemp);
  set('#ac-step', config.ac.tempStep); set('#ac-repeats', config.ac.sendRepeats);
  set('#ac-minoff', config.ac.minOffSeconds); set('#ac-watts', config.ac.ratedWatts);

  set('#pin-rx', config.pins.irRx); set('#pin-tx', config.pins.irTx);
  set('#pin-btn', config.pins.button); set('#pin-khz', config.pins.irCarrierKhz);
  check('#pin-inverted', config.pins.irTxInverted);

  const compat = config.compat || {};
  $('#compat-tasmota').checked = !!compat.tasmota;
  $('#compat-metrics').checked = !!compat.metrics;

  set('#fmt-unit', config.device.celsius === false ? 'f' : 'c');
  set('#fmt-clock', config.device.hour12 ? '12' : '24');
  set('#fmt-date', config.device.dateFormat || 'iso');
  set('#fmt-week', String(config.device.weekStart === 0 ? 0 : 1));
  set('#ntp-server', config.time.ntpServer); set('#ntp-tz', config.time.timezone);
  set('#geo-lat', config.time.latitude || ''); set('#geo-lon', config.time.longitude || '');
  const zonePick = $('#tz-pick');
  if (zonePick) {
    zonePick.value = TIMEZONES.some(([, posix]) => posix === config.time.timezone)
      ? config.time.timezone : '';
  }

  renderSources();
}

function renderTelegramAllowed() {
  const allowed = (state.config && state.config.telegram.allowed) || [];
  $('#tg-allowed').replaceChildren(...(allowed.length ? allowed.map((id, index) =>
    el('div', { class: 'row' },
      el('span', { class: 'grow', text: String(id) }),
      el('button', {
        class: 'danger ghost',
        onclick: () => { allowed.splice(index, 1); renderTelegramAllowed(); },
      }, '×'))) :
    [el('p', { class: 'muted', text: t('telegram.none', 'Nobody authorised yet.') })]));
}

const SAVERS = {
  device: () => ({ device: { name: $('#dev-name').value } }),
  mqtt: () => ({
    mqtt: {
      enabled: $('#mqtt-enabled').checked,
      host: $('#mqtt-host').value, port: Number($('#mqtt-port').value),
      user: $('#mqtt-user').value, pass: secretValue($('#mqtt-pass')),
      baseTopic: $('#mqtt-base').value, discoveryPrefix: $('#mqtt-prefix').value,
      discovery: $('#mqtt-discovery').checked, retain: $('#mqtt-retain').checked,
      homie: $('#mqtt-homie').checked,
    },
  }),
  webhook: () => ({
    webhook: {
      enabled: $('#hook-enabled').checked, url: $('#hook-url').value,
      minIntervalSeconds: Number($('#hook-interval').value),
      headerName: $('#hook-header').value,
      headerValue: secretValue($('#hook-value')),
    },
  }),
  modbus: () => ({
    modbus: {
      enabled: $('#modbus-enabled').checked, port: Number($('#modbus-port').value),
    },
  }),
  telegram: () => ({
    telegram: {
      enabled: $('#tg-enabled').checked, token: secretValue($('#tg-token')),
      pollSeconds: Number($('#tg-poll').value),
      notifyOnChange: $('#tg-notify').checked,
      openEnrolment: $('#tg-open').checked,
      allowed: (state.config.telegram.allowed || []).map(Number),
    },
  }),
  uart: () => ({
    uart: {
      enabled: $('#uart-enabled').checked, baud: Number($('#uart-baud').value),
      emitEvents: $('#uart-events').checked,
    },
  }),
  network: () => ({
    device: { hostname: $('#net-hostname').value },
    wifi: {
      useStatic: $('#net-static').checked, ip: $('#net-ip').value,
      gateway: $('#net-gw').value, mask: $('#net-mask').value, dns: $('#net-dns').value,
    },
  }),
  auth: () => ({
    auth: {
      enabled: $('#auth-enabled').checked, user: $('#auth-user').value,
      pass: secretValue($('#auth-pass')), token: secretValue($('#auth-token')),
    },
    wifi: { apPassword: secretValue($('#ap-pass')) },
  }),
  pins: () => ({
    pins: {
      irRx: Number($('#pin-rx').value), irTx: Number($('#pin-tx').value),
      button: Number($('#pin-btn').value), irCarrierKhz: Number($('#pin-khz').value),
      irTxInverted: $('#pin-inverted').checked,
    },
  }),
  compat: () => ({ compat: {
    tasmota: $('#compat-tasmota').checked,
    metrics: $('#compat-metrics').checked,
  } }),
  locale: () => ({ device: {
    celsius: $('#fmt-unit').value === 'c',
    hour12: $('#fmt-clock').value === '12',
    dateFormat: $('#fmt-date').value,
    weekStart: Number($('#fmt-week').value),
  } }),
  time: () => ({ time: {
    ntpServer: $('#ntp-server').value,
    timezone: $('#ntp-tz').value,
    latitude: Number($('#geo-lat').value) || 0,
    longitude: Number($('#geo-lon').value) || 0,
  } }),
};

async function saveSection(name) {
  try {
    const data = await api('/api/config', { body: SAVERS[name]() });
    state.config = data.config;
    fillForms();
    toast(t('common.saved', 'Saved'), 'ok');
    // The device reconnects the affected integration on save, so pull the
    // status straight away rather than leaving a stale panel until the next
    // poll — "did that work?" is the question right after pressing Save.
    setTimeout(refreshStatus, 600);
  } catch (error) { toast(error.message, 'error'); }
}

async function refreshConfig() {
  state.config = await api('/api/config');
  fillForms();
}

/* The full status document is two and a half kilobytes and almost none of it
 * changes while the page is open — the name, the pins, the protocol and the
 * network are settled once. Asking for all of it every fifteen seconds is how
 * a device with a fragmented heap ends up unable to allocate a TCP segment.
 *
 * So: everything once, then only the parts that move, merged over the top. */
function mergeStatus(fresh) {
  const previous = state.status || {};
  const merged = { ...previous, ...fresh };
  for (const section of ['device', 'network', 'clock', 'ac', 'ir', 'stats']) {
    if (fresh[section]) {
      merged[section] = { ...(previous[section] || {}), ...fresh[section] };
    }
  }
  return merged;
}

async function refreshStatus(brief = false) {
  try {
    const fresh = await api(`/api/status${brief ? '?brief=1' : ''}`);
    state.status = brief ? mergeStatus(fresh) : fresh;
    state.ac = state.status.state;
    state.learning = state.status.learning;
    state.hold = (state.status.ac && state.status.ac.restartHold) || 0;
    setLink(true);
    renderAll();
  } catch (error) { setLink(false); }
}

/* Every integration reports itself the same way, so one renderer explains all
 * of them — including *why* one is not connected, which is the question people
 * actually have. */
function renderIntegrations() {
  const all = (state.status && state.status.integrations) || {};

  const fill = (selector, info) => {
    const node = $(selector);
    if (!node) return;
    if (!info) { node.replaceChildren(); return; }

    const rows = [];
    const push = (label, value) =>
      rows.push(el('dt', { text: label }), el('dd', { text: String(value) }));

    push(t('status.state', 'Status'), info.enabled
      ? (info.connected ?? info.running ?? true
          ? t('status.ok', 'working')
          : t('status.trying', 'not connected'))
      : t('status.off', 'switched off'));

    if (info.deliveries !== undefined) push(t('webhook.delivered', 'Delivered'), info.deliveries);
    if (info.failures) push(t('webhook.failed', 'Failed'), info.failures);
    if (info.writes !== undefined) push(t('modbus.writes', 'Writes received'), info.writes);
    if (info.connects !== undefined) push(t('mqtt.connects', 'Connections'), info.connects);
    if (info.pausedFor) push(t('webhook.paused', 'Paused for'), `${info.pausedFor} s`);
    if (info.lastError) push(t('status.problem', 'Last problem'), info.lastError);
    if (info.reason) push(t('status.problem', 'Last problem'), info.reason);

    node.replaceChildren(...rows);
  };

  fill('#mqtt-status', all.mqtt);
  fill('#tg-status', all.telegram);
  fill('#hook-status', all.webhook);
  fill('#modbus-status', all.modbus);

  // The closed section says whether it is working, so a settings page can be
  // scanned without opening anything.
  const note = (info) => !info || !info.enabled
    ? t('status.off', 'switched off')
    : ((info.connected ?? info.running ?? true) ? t('status.ok', 'working')
                                                : t('status.trying', 'not connected'));
  setSectionNote('webhook.title', note(all.webhook));
  setSectionNote('modbus.title', note(all.modbus));

  const auth = (state.config && state.config.auth) || {};
  const summary = $('#security-summary');
  if (summary) {
    summary.textContent = auth.enabled
      ? t('settings.security_on', 'Password required')
      : t('settings.security_off', 'Anyone on your Wi-Fi');
  }

  const host = location.host;
  const other = [];
  const push = (label, value) =>
    other.push(el('dt', { text: label }), el('dd', { text: value }));
  push(t('other.rest', 'REST API'), `http://${host}/api/status`);
  // Only what is actually answering: listing a switched-off endpoint as an
  // address to try is worse than not listing it.
  const compatOn = (state.config && state.config.compat) || {};
  if (compatOn.tasmota) {
    push(t('other.tasmota', 'Tasmota commands'), `http://${host}/cm?cmnd=Power%20Toggle`);
  }
  if (compatOn.metrics) {
    push(t('other.metrics', 'Prometheus metrics'), `http://${host}/api/metrics`);
  }
  push(t('other.ws', 'Live updates'), `ws://${host}/ws`);
  $('#other-ways').replaceChildren(...other);
}

function renderAll() {
  renderControl();
  renderWizard();
  renderSystem();
  renderStats();
  renderSources();
  renderIntegrations();
}

/* ══════════════════════════════════ wiring ══════════════════════════════ */

function wireEvents() {
  $('#tabs').addEventListener('click', (event) => {
    const tab = event.target.closest('.tab');
    if (tab) showView(tab.dataset.view);
  });
  $$('[data-goto]').forEach((node) =>
    node.addEventListener('click', () => showView(node.dataset.goto)));

  $$('[data-learn]').forEach((node) =>
    node.addEventListener('click', () => startLearn(node.dataset.learn)));
  $$('[data-save]').forEach((node) =>
    node.addEventListener('click', () => saveSection(node.dataset.save)));

  $('#protocol-save').addEventListener('click', async () => {
    try {
      await api('/api/config', {
        body: {
          ac: {
            protocol: $('#protocol-select').value,
            brand: $('#ac-brand').value, modelName: $('#ac-model').value,
            minTemp: Number($('#ac-min').value), maxTemp: Number($('#ac-max').value),
            tempStep: Number($('#ac-step').value), sendRepeats: Number($('#ac-repeats').value),
            minOffSeconds: Number($('#ac-minoff').value),
            ratedWatts: Number($('#ac-watts').value),
            useLearnedCodes: !$('#protocol-select').value,
          },
        },
      });
      toast(t('common.saved', 'Saved'), 'ok');
      refreshStatus(); refreshConfig();
    } catch (error) { toast(error.message, 'error'); }
  });

  /* Does the LED light up at all?
   *
   * A real command is a few hundred microseconds of 38 kHz burst — far too
   * brief and too dim for a phone camera to catch reliably, so "I saw nothing"
   * proves nothing. This sends a second and a half of slow, even blinking
   * instead: two dozen 60 ms carrier bursts, which any camera that can see
   * infrared at all will show clearly.
   *
   * It needs no protocol and no air conditioner. It only asks whether the
   * emitter works. */
  $('#emitter-test').addEventListener('click', async () => {
    // Alternating mark and space. 60 ms is the longest a single timing can
    // be, and slow enough to read as blinking rather than a glow.
    const timings = Array.from({ length: 24 }, () => 60000);
    try {
      await api('/api/ir/send', { body: { timings, khz: 38 } });
      toast(t('teach.emitter_sent',
              'Blinking for a second and a half — point a phone camera at the '
              + 'front of the box. Use the selfie camera: most rear cameras '
              + 'filter infrared out.'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#protocol-test').addEventListener('click', async () => {
    const protocol = $('#protocol-select').value;
    if (!protocol) return;
    try {
      await api('/api/ir/send', { body: { protocol, power: true, mode: 'cool', temp: 24 } });
      toast(t('teach.test_sent', 'Test command sent — did the unit react?'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#remote-learn').addEventListener('click', startBind);
  $('#remotes-clear').addEventListener('click', async () => {
    if (!confirm(t('remotes.clear_confirm', 'Forget every bound button?'))) return;
    try {
      await api('/api/remotes/clear', { body: { confirm: 'clear-buttons' } });
      refreshRemotes();
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#db-refresh').addEventListener('click', loadDatabase);
  $('#db-search').addEventListener('input', renderDatabase);
  $('#profile-export').addEventListener('click', exportProfile);
  $('#profile-submit').addEventListener('click', submitProfile);

  $('#codes-clear').addEventListener('click', async () => {
    if (!confirm(t('teach.codes_clear_confirm', 'Erase every learned code?'))) return;
    try { await api('/api/codes/clear', { body: { confirm: 'clear-codes' } }); refreshCodes(); }
    catch (error) { toast(error.message, 'error'); }
  });

  $('#tg-add').addEventListener('click', () => {
    const id = Number($('#tg-add-id').value.trim());
    if (!id) return;
    state.config.telegram.allowed = state.config.telegram.allowed || [];
    state.config.telegram.allowed.push(id);
    $('#tg-add-id').value = '';
    renderTelegramAllowed();
  });

  $('#hook-test').addEventListener('click', async () => {
    try {
      await api('/api/webhook/test', { method: 'POST', body: {} });
      toast(t('webhook.sent', 'Test sent — check the receiving end'), 'ok');
      setTimeout(refreshStatus, 1200);
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#wifi-scan').addEventListener('click', refreshWifi);
  $('#wifi-connect').addEventListener('click', async () => {
    try {
      await api('/api/wifi/connect', {
        body: { ssid: $('#wifi-ssid').value, pass: $('#wifi-pass').value },
      });
      toast(t('network.connecting', 'Connecting — the device may change address.'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#auth-generate').addEventListener('click', () => {
    const bytes = crypto.getRandomValues(new Uint8Array(24));
    $('#auth-token').value = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
  });

  $('#scene-add').addEventListener('click', () => {
    state.scenes = state.scenes || [];
    state.scenes.push({ name: 'New scene', icon: '⭐',
                        action: { hvac_mode: 'cool', temp: 24 } });
    renderSceneEditor();
  });
  $('#scene-capture').addEventListener('click', () => {
    // Turning "what the unit is doing right now" into a scene is far easier
    // than hand-writing the JSON, and it is how most people will make one.
    const ac = state.ac || {};
    state.scenes = state.scenes || [];
    state.scenes.push({
      name: 'Captured', icon: '📌',
      action: ac.power
        ? { hvac_mode: ac.mode, temp: ac.temp, fan: ac.fan, swingv: ac.swingv }
        : { power: false },
    });
    renderSceneEditor();
  });
  $('#scene-save').addEventListener('click', async () => {
    try {
      const data = await api('/api/scenes', { body: { scenes: state.scenes } });
      state.scenes = data.scenes || [];
      renderSceneEditor();
      renderControl();
      toast(t('common.saved', 'Saved'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  // Timezone: the list writes the POSIX string into the field, which stays
  // editable for anywhere the list does not cover.
  const zone = $('#tz-pick');
  zone.replaceChildren(
    el('option', { value: '', text: t('clock.zone_other', 'Somewhere else') }),
    ...TIMEZONES.map(([name, posix]) => el('option', { value: posix, text: name })));
  zone.addEventListener('change', () => {
    if (zone.value) $('#ntp-tz').value = zone.value;
  });
  $('#ntp-tz').addEventListener('input', () => {
    zone.value = TIMEZONES.some(([, posix]) => posix === $('#ntp-tz').value)
      ? $('#ntp-tz').value : '';
  });

  $('#clock-manual').addEventListener('input', (event) => {
    event.target.dataset.touched = '1';
  });
  $('#clock-from-browser').addEventListener('click', () => {
    // The browser's wall clock, written as the device's local time — which is
    // what the field means — so the two agree on the instant, not the digits.
    const offset = (state.clock && state.clock.offset) || 0;
    const shifted = new Date(Date.now() + offset * 1000);
    $('#clock-manual').value = shifted.toISOString().slice(0, 19);
    $('#clock-manual').dataset.touched = '1';
  });
  $('#clock-apply').addEventListener('click', applyManualTime);

  // The browser can usually say where it is, and it is nearly always in the
  // same building as the device. Rounded to four decimals — about ten metres,
  // far finer than sunset cares about and no more than is anyone's business.
  $('#geo-locate').addEventListener('click', () => {
    if (!navigator.geolocation) {
      toast(t('clock.no_geo', 'This browser will not say where it is'), 'error');
      return;
    }
    navigator.geolocation.getCurrentPosition((position) => {
      $('#geo-lat').value = position.coords.latitude.toFixed(4);
      $('#geo-lon').value = position.coords.longitude.toFixed(4);
      toast(t('clock.geo_ok', 'Location filled in — save to keep it'), 'ok');
    }, () => toast(t('clock.no_geo', 'This browser will not say where it is'),
                   'error'));
  });

  $('#lang-look').addEventListener('click', lookForLanguages);
  $('#lang-upload').addEventListener('click', () => $('#lang-file').click());
  $('#lang-file').addEventListener('change', (event) => {
    const file = event.target.files[0];
    if (file) installLanguageFromFile(file);
    event.target.value = '';
  });

  $('#stats-reset').addEventListener('click', async () => {
    if (!confirm(t('stats.reset_confirm', 'Clear the usage counters?'))) return;
    try {
      await api('/api/stats/reset', { body: { confirm: 'reset-stats' } });
      refreshStatus();
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#sched-add-daily').addEventListener('click', () => {
    state.schedules.push({ name: 'New rule', enabled: true, kind: 'daily',
                           days: 0b1111111, hour: 8, minute: 0,
                           action: { hvac_mode: 'cool', temp: 24 } });
    renderSchedules();
  });
  $('#sched-add-timer').addEventListener('click', () => {
    const minutes = Number(prompt(t('schedules.timer_prompt', 'Fire in how many minutes?'), '60'));
    if (!minutes) return;
    state.schedules.push({
      name: `+${minutes} min`, enabled: true, kind: 'timer',
      fireAt: Math.floor(Date.now() / 1000) + minutes * 60,
      action: { power: false },
    });
    renderSchedules();
  });
  $('#sched-save').addEventListener('click', async () => {
    try {
      await api('/api/schedules', { body: { schedules: state.schedules } });
      toast(t('common.saved', 'Saved'), 'ok');
      refreshSchedules();
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#ota-upload').addEventListener('click', uploadFirmware);
  $('#ota-check').addEventListener('click', () => {
    const url = state.status && state.status.ota && state.status.ota.manifestUrl;
    if (url) window.open(url.replace('api.github.com/repos', 'github.com')
                            .replace('/releases/latest', '/releases'), '_blank', 'noopener');
  });

  $('#log-clear').addEventListener('click', async () => {
    try { await api('/api/log/clear', { method: 'POST', body: {} }); refreshLog(true); }
    catch (error) { toast(error.message, 'error'); }
  });

  $('#backup-download').addEventListener('click', async () => {
    const blob = new Blob([JSON.stringify(state.config, null, 1)], { type: 'application/json' });
    const link = el('a', { href: URL.createObjectURL(blob), download: 'slwf12-config.json' });
    link.click();
    URL.revokeObjectURL(link.href);
  });
  $('#backup-restore').addEventListener('click', () => $('#backup-file').click());
  $('#backup-file').addEventListener('change', async (event) => {
    const file = event.target.files[0];
    if (!file) return;
    try {
      await api('/api/config', { body: JSON.parse(await file.text()) });
      toast(t('system.restored', 'Settings restored'), 'ok');
      refreshConfig();
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#restart-btn').addEventListener('click', async () => {
    if (!confirm(t('system.restart_confirm', 'Restart the device?'))) return;
    try { await api('/api/restart', { method: 'POST', body: {} }); } catch { /* it went away */ }
    toast(t('system.restarting', 'Restarting…'));
    setTimeout(() => location.reload(), 6000);
  });
  $('#factory-btn').addEventListener('click', async () => {
    if (!confirm(t('system.factory_confirm', 'Erase every setting and learned code?'))) return;
    try { await api('/api/factory-reset', { body: { confirm: 'factory-reset' } }); } catch { /* ditto */ }
    toast(t('system.restarting', 'Restarting…'));
  });
}


/* ═══════════════════════════ paired devices ══════════════════════════════
 *
 * The device type database drives all of this. A control card is built from a
 * type's declared actions, and the automation editor from its conditions — so
 * supporting a new kind of device is a change to devicetypes.json, not to this
 * file.
 */

async function loadDeviceTypes() {
  if (state.types) return state.types;
  try {
    const response = await fetch('/devicetypes.json', { cache: 'no-cache' });
    state.types = await response.json();
  } catch {
    state.types = { families: {}, types: [] };
  }
  return state.types;
}

function typeInfo(typeId) {
  const db = state.types || { families: {}, types: [] };
  const type = (db.types || []).find((entry) => entry.id === typeId);
  if (!type) return null;
  return Object.assign({}, type, { family: (db.families || {})[type.family] || {} });
}

/* Every device this page can drive: this one first, then the paired ones. */
function deviceList() {
  const self = {
    id: 'self',
    name: (state.config && state.config.device.name) || 'This device',
    typeId: 'slwf12',
    online: true,
    state: {},
  };
  const live = {};
  for (const entry of state.peerStatus || []) live[entry.id] = entry;

  const others = (state.peers || [])
    .filter((peer) => peer.enabled !== false)
    .map((peer) => ({
      id: peer.id,
      name: peer.name,
      typeId: peer.type,
      online: !!(live[peer.id] && live[peer.id].online),
      state: (live[peer.id] || {}).state || {},
    }));
  return [self].concat(others);
}

function renderDeviceTabs() {
  const strip = $('#device-tabs');
  const devices = deviceList();

  // One device is not a choice, so do not present it as one.
  if (devices.length < 2) {
    strip.hidden = true;
    state.activeDevice = 'self';
    return;
  }

  strip.hidden = false;
  if (!devices.some((device) => device.id === state.activeDevice)) {
    state.activeDevice = 'self';
  }

  const tabFor = (device) => {
    const info = typeInfo(device.typeId);
    return el('button', {
      class: 'device-tab' + (device.id === state.activeDevice ? ' active' : '') +
             (device.online ? '' : ' offline'),
      onclick: () => { state.activeDevice = device.id; renderControl(); },
    },
      icon(info && info.icon, 'device-icon'),
      el('span', { text: device.name }));
  };

  // Two labelled groups, so it is obvious which one is the device you are
  // standing in front of. Without this the local unit is just the leftmost
  // button, which says nothing.
  const group = (labelKey, fallback, members) =>
    el('div', { class: 'device-group' },
      el('span', { class: 'device-group-label', text: t(labelKey, fallback) }),
      el('div', { class: 'device-group-tabs' }, ...members.map(tabFor)));

  strip.replaceChildren(
    group('devices.this_one', 'This device', [devices[0]]),
    group('devices.paired', 'Paired devices', devices.slice(1)));
}

/* Draws the control surface for whichever device is selected. */
function renderControl() {
  renderDeviceTabs();

  const device = activeDevice();
  const card = cardFor(device);
  const container = $('#device-cards');

  if (container.firstChild !== card.node) container.replaceChildren(card.node);
  card.update(card.kind === 'climate' ? climateContext() : { device });

  $('#not-configured').hidden =
    device.id !== 'self' ||
    !!(state.status && state.status.ac && state.status.ac.configured);
}

/* Kept as a name because several places mean "redraw the control surface". */
function renderClimate() { renderControl(); }

function activeDeviceById(deviceId) {
  return deviceList().find((entry) => entry.id === deviceId) ||
         { id: deviceId, typeId: '', state: {} };
}

/* The device the control page is currently showing. */
function activeDevice() {
  const devices = deviceList();
  return devices.find((entry) => entry.id === (state.activeDevice || 'self')) ||
         devices[0];
}

/* What the climate card should draw, wherever it comes from. A peer's limits
 * come from its type's temperature action, since the cached state carries
 * values rather than bounds. */
function climateContext() {
  const device = activeDevice();
  if (device.id === 'self') {
    return {
      device,
      ac: state.ac || {},
      limits: (state.status && state.status.ac) || { minTemp: 16, maxTemp: 30, tempStep: 1 },
      isSelf: true,
    };
  }
  const info = typeInfo(device.typeId);
  const adapter = climateAdapter(device);
  const spec = ((info && info.family && info.family.actions) || [])
    .find((action) => action.id === 'temp') || {};
  return {
    device,
    ac: adapter.fromNative(device.state || {}),
    limits: adapter.limits || {
      minTemp: spec.min !== undefined ? spec.min : 16,
      maxTemp: spec.max !== undefined ? spec.max : 30,
      tempStep: spec.step || 1,
    },
    isSelf: false,
  };
}

function renderAction(device, action, values) {
  const key = action.path || action.id;
  const current = values[key];
  const send_ = (value) => commandDevice(
    device.id,
    key.includes('.') ? commandFromPath(key, value) : { [key]: value });
  const label = el('div', { class: 'action-label', text: action.label });

  if (action.type === 'button') {
    // An endpoint of its own where the type declares one (ESPHome presses a
    // separate URL per button), otherwise just its name — which is all a
    // transport like Wake-on-LAN needs, having only one thing it can do.
    const press = () => commandDevice(device.id,
      action.endpoint ? { _path: action.endpoint } : { [action.id]: true });
    return el('div', { class: 'action-row' }, label,
      el('button', { class: 'chip', onclick: press },
         t('common.press', 'Press')));
  }

  if (action.type === 'color') {
    const picker = el('input', { type: 'color', value: '#ffffff',
                                 style: 'width:3rem;height:2rem;padding:2px' });
    picker.addEventListener('change', () => send_(hexToRgb(picker.value)));
    return el('div', { class: 'action-row' }, label, picker);
  }

  if (action.type === 'text') {
    const box = el('input', { type: 'text', value: current === undefined ? '' : String(current) });
    const go = el('button', { class: 'chip', onclick: () => send_(box.value) },
                  t('common.set', 'Set'));
    return el('div', { class: 'action-row' }, label, box, go);
  }

  if (action.type === 'boolean') {
    const on = current === true || current === 1;
    return el('div', { class: 'action-row' }, label,
      el('button', {
        class: 'chip' + (on ? ' active' : ''), 'data-fan': 'x',
        onclick: () => send_(!on),
      }, on ? t('common.on', 'On') : t('common.off', 'Off')));
  }

  if (action.type === 'enum') {
    return el('div', { class: 'action-row column' }, label,
      el('div', { class: 'chip-row small' },
        ...(action.values || []).map((value) => el('button', {
          class: 'chip' + (String(current) === value ? ' active' : ''),
          'data-fan': 'x',
          onclick: () => send_(value),
        }, t(`${action.id}.${value}`, value)))));
  }

  if (action.type === 'number') {
    const step = action.step || 1;
    const min = action.min !== undefined ? action.min : 0;
    const max = action.max !== undefined ? action.max : 100;
    const value = Number.isFinite(current) ? current : min;
    return el('div', { class: 'action-row' }, label,
      el('button', { class: 'chip', onclick: () => send_(Math.max(min, value - step)) }, '−'),
      el('span', { class: 'action-value', text: `${value}${action.unit || ''}` }),
      el('button', { class: 'chip', onclick: () => send_(Math.min(max, value + step)) }, '+'));
  }

  return el('div', { class: 'action-row' }, label);
}

async function commandDevice(deviceId, command) {
  try {
    if (deviceId === 'self') { await send(command); return; }
    await api('/api/peers/command', { body: { id: deviceId, command } });
    // A peer is polled rather than confirmed synchronously; give it a moment
    // before asking what actually happened.
    setTimeout(refreshPeers, 1200);
  } catch (error) { toast(error.message, 'error'); }
}

async function refreshPeers() {
  try {
    const data = await api('/api/peers');
    state.peers = data.peers || [];
    state.peerStatus = data.status || [];
    state.candidates = data.candidates || [];
    renderDeviceList();
    renderControl();
  } catch { /* the panel is optional */ }
}

function renderDeviceList() {
  const list = $('#device-list');
  if (!list) return;

  const rows = (state.peers || []).map((peer, index) => {
    const info = typeInfo(peer.type);
    const live = (state.peerStatus || []).find((entry) => entry.id === peer.id);

    const enabled = el('input', { type: 'checkbox' });
    enabled.checked = peer.enabled !== false;
    enabled.addEventListener('change', () => { peer.enabled = enabled.checked; });

    // Some families need one string per device that the type cannot know:
    // an ESPHome entity id, a machine's MAC address. The type says what to
    // call it, so this stays one field rather than one per family.
    const family = (info && info.family) || {};
    let extra = null;
    if (family.needsEntity) {
      extra = el('input', {
        type: 'text', class: 'row-extra',
        value: peer.entity || '',
        placeholder: family.entityPlaceholder || '',
        title: family.entityLabel || '',
      });
      extra.addEventListener('input', () => { peer.entity = extra.value; });
    }

    // A thing with nothing to poll is not offline — it has no state to have.
    const stateless = live && live.stateless;

    return el('div', { class: 'row' },
      el('label', { class: 'switch' }, enabled, el('span', {}, '')),
      icon(info && info.icon, 'row-icon'),
      el('span', { class: 'grow', text: peer.name }),
      extra,
      el('span', { class: 'tag', text: info ? info.name : peer.type }),
      stateless ? null : el('span', {
        class: 'muted small',
        text: live && live.online ? t('devices.online', 'online')
                                  : t('devices.offline', 'offline'),
      }),
      el('button', {
        class: 'danger ghost',
        onclick: () => { state.peers.splice(index, 1); renderDeviceList(); },
      }, '×'));
  });

  list.replaceChildren(...(rows.length ? rows :
    [el('p', { class: 'muted', text: t('devices.empty', 'No devices paired yet.') })]));

  const select = $('#device-type');
  if (select && select.options.length === 0) {
    // `hidden` types are not hardware you pair with — the clock is one.
    select.replaceChildren(...(((state.types || {}).types) || [])
      .filter((type) => !type.hidden)
      .map((type) => el('option', { value: type.id, text: type.name })));
  }

  const found = (state.candidates || []).filter((candidate) => !candidate.known);
  $('#device-candidates').replaceChildren(...found.map((candidate) =>
    el('div', { class: 'row' },
      el('span', { class: 'grow', text: candidate.name || candidate.host }),
      el('span', { class: 'muted small', text: candidate.host }),
      el('button', {
        class: 'ghost',
        onclick: () => {
          $('#device-host').value = candidate.host;
          $('#device-name').value = candidate.name || candidate.host;
          if (candidate.type) $('#device-type').value = candidate.type;
        },
      }, t('devices.use', 'Use')))));
}

/* ═══════════════════════════════ automations ═════════════════════════════ */

const OPS = ['eq', 'ne', 'gt', 'lt', 'gte', 'lte'];

async function refreshAutomations() {
  try {
    const data = await api('/api/automations');
    state.automations = data.automations || [];
    renderAutomations();
  } catch { /* optional */ }
}

/* The clock is not a device, but it answers questions in exactly the same
 * shape — so it is offered as one, and the engine reads it through the same
 * path. It can be asked about; it cannot be commanded, so it is only ever in
 * the IF half. */
/* Everything a rule can name, whether it can be asked, told, or both. Lookups
 * go through this; the two menus filter it. */
function ruleThings() {
  // Named for both halves of what it answers: nobody looking for sunset
  // thinks to open something called "Time".
  return [...deviceList(),
          { id: 'clock', name: t('clock.device', 'Time and sun'),
            typeId: 'clock' }];
}

function declares(device, which) {
  const info = typeInfo(device.typeId);
  return ((info && info.family && info.family[which]) || []).length > 0;
}

/* Only things that can actually be asked something. A Wake-on-LAN target has
 * no state, so offering it here would be a dead end with an empty field list
 * at the end of it. */
function conditionDevices() {
  return ruleThings().filter((device) => declares(device, 'conditions'));
}

function deviceOptions(list) {
  return (list || deviceList()).map((device) =>
    el('option', { value: device.id, text: device.name }));
}

/* The type database is the only place that knows what a device can do, so
 * both halves of the editor read their choices from it. */
function fieldsFor(deviceId, which) {
  const device = ruleThings().find((entry) => entry.id === deviceId);
  const info = device ? typeInfo(device.typeId) : null;
  return (info && info.family && info.family[which]) || [];
}

function renderAutomations() {
  const rows = (state.automations || []).map((rule, index) => {
    const enabled = el('input', { type: 'checkbox' });
    enabled.checked = rule.enabled !== false;
    enabled.addEventListener('change', () => { rule.enabled = enabled.checked; });

    const name = el('input', { type: 'text', value: rule.name || '' });
    name.addEventListener('input', () => { rule.name = name.value; });

    const match = el('select', {},
      el('option', { value: 'all', text: t('automations.all', 'all of these') }),
      el('option', { value: 'any', text: t('automations.any', 'any of these') }));
    match.value = rule.match || 'all';
    match.addEventListener('change', () => { rule.match = match.value; });

    const hold = el('input', {
      type: 'number', min: '0', max: '3600', style: 'width:5rem',
    });
    hold.value = rule.for || 0;
    hold.addEventListener('input', () => { rule.for = Number(hold.value); });

    return el('div', { class: 'card rule' },
      el('div', { class: 'row' },
        el('label', { class: 'switch' }, enabled, el('span', {}, '')),
        el('span', { class: 'grow' }, name),
        el('button', { class: 'ghost', onclick: () => runRule(rule) },
           t('automations.test', 'Run now')),
        el('button', {
          class: 'danger ghost',
          onclick: () => { state.automations.splice(index, 1); renderAutomations(); },
        }, '×')),

      el('div', { class: 'rule-part' },
        el('div', { class: 'rule-heading' },
          el('span', { class: 'rule-word', text: t('automations.if', 'IF') }), match),
        ...(rule.conditions || []).map((condition, ci) =>
          renderCondition(rule, condition, ci)),
        el('button', {
          class: 'ghost small-btn',
          onclick: () => {
            rule.conditions = rule.conditions || [];
            rule.conditions.push({ device: 'self', field: 'power', op: 'eq', value: 'true' });
            renderAutomations();
          },
        }, t('automations.add_condition', '+ condition'))),

      el('div', { class: 'rule-part' },
        el('div', { class: 'rule-heading' },
          el('span', { class: 'rule-word', text: t('automations.then', 'THEN') }),
          el('span', { class: 'muted small', text: t('automations.hold', 'after') }),
          hold,
          el('span', { class: 'muted small', text: t('automations.seconds', 'seconds') })),
        ...(rule.actions || []).map((action, ai) => renderRuleAction(rule, action, ai)),
        el('button', {
          class: 'ghost small-btn',
          onclick: () => {
            rule.actions = rule.actions || [];
            rule.actions.push({ device: 'self', command: { power: true } });
            renderAutomations();
          },
        }, t('automations.add_action', '+ action'))),

      rule.fired
        ? el('p', { class: 'muted small',
                    text: `${t('automations.fired', 'Fired')} ${rule.fired}×` })
        : null);
  });

  $('#automation-list').replaceChildren(...(rows.length ? rows :
    [el('p', { class: 'muted', text: t('automations.empty', 'No rules yet.') })]));
}

function renderCondition(rule, condition, index) {
  const device = el('select', {}, ...deviceOptions(conditionDevices()));
  device.value = condition.device || 'self';
  device.addEventListener('change', () => {
    condition.device = device.value;
    // The new device may not have the field the old one did.
    condition.field = (fieldsFor(condition.device, 'conditions')[0] || {}).id || '';
    renderAutomations();
  });

  const fields = fieldsFor(condition.device || 'self', 'conditions');
  const field = el('select', {}, ...fields.map((entry) =>
    el('option', { value: entry.id, text: entry.label })));
  field.value = condition.field || (fields[0] || {}).id || '';
  field.addEventListener('change', () => {
    condition.field = field.value;
    renderAutomations();
  });

  const op = el('select', {}, ...OPS.map((name) =>
    el('option', { value: name, text: t(`op.${name}`, name) })));
  op.value = condition.op || 'eq';
  op.addEventListener('change', () => { condition.op = op.value; });

  const spec = fields.find((entry) => entry.id === condition.field) || {};
  let value;
  if (spec.type === 'enum') {
    value = el('select', {}, ...(spec.values || []).map((v) =>
      el('option', { value: v, text: t(`${spec.id}.${v}`, v) })));
  } else if (spec.type === 'boolean') {
    value = el('select', {},
      el('option', { value: 'true', text: t('common.on', 'On') }),
      el('option', { value: 'false', text: t('common.off', 'Off') }));
  } else if (spec.type === 'time') {
    value = el('input', { type: 'time' });
  } else if (spec.type === 'number') {
    value = el('input', { type: 'number', step: String(spec.step || 1) });
  } else {
    value = el('input', { type: 'text' });
  }

  // Stored as minutes past midnight, because that compares with < and >
  // where "08:00" does not. Shown as a clock face, because nobody thinks in
  // minutes past midnight.
  const toField = (stored) => {
    const total = Number(stored);
    if (!Number.isFinite(total)) return '';
    const pad = (n) => String(n).padStart(2, '0');
    return `${pad(Math.floor(total / 60) % 24)}:${pad(total % 60)}`;
  };
  const fromField = (shown) => {
    const [hh, mm] = String(shown).split(':').map(Number);
    return Number.isFinite(hh) ? String(hh * 60 + (mm || 0)) : '';
  };

  value.value = condition.value !== undefined && condition.value !== null
    ? (spec.type === 'time' ? toField(condition.value) : condition.value)
    : (spec.type === 'boolean' ? 'true' : '');
  const store = () => {
    condition.value = spec.type === 'time'
      ? fromField(value.value) : String(value.value);
  };
  value.addEventListener('change', store);
  value.addEventListener('input', store);

  return el('div', { class: 'rule-line' }, device, field, op, value,
    el('button', {
      class: 'danger ghost small-btn',
      onclick: () => { rule.conditions.splice(index, 1); renderAutomations(); },
    }, '×'));
}

function renderRuleAction(rule, action, index) {
  // And only things that can be told something.
  const targets = deviceList().filter(
    (entry) => entry.id === 'self' || declares(entry, 'actions'));
  const device = el('select', {}, ...deviceOptions(targets));
  device.value = action.device || 'self';
  device.addEventListener('change', () => {
    action.device = device.value;
    action.command = {};
    renderAutomations();
  });

  const actions = fieldsFor(action.device || 'self', 'actions');

  // A button that presses its own URL is stored as {_path: …}, so recovering
  // which action that was means matching the endpoint back to its declaration
  // rather than reading the key.
  const command = action.command || {};
  const byPath = command._path
    ? actions.find((entry) => entry.endpoint === command._path) : null;
  const currentKey = (byPath && (byPath.path || byPath.id)) ||
                     Object.keys(command).filter((key) => key !== '_path')[0] ||
                     (actions[0] || {}).path || (actions[0] || {}).id || '';

  const what = el('select', {}, ...actions.map((entry) =>
    el('option', { value: entry.path || entry.id, text: entry.label })));
  what.value = currentKey;
  what.addEventListener('change', () => {
    action.command = { [what.value]: '' };
    renderAutomations();
  });

  const spec = actions.find((entry) => (entry.path || entry.id) === currentKey) || {};

  // Seed a usable value rather than leaving the command empty. Changing the
  // device or the action clears it, and a rule saved in that state would pass
  // validation and then quietly do nothing.
  // A button has nothing to choose: pressing it *is* the command. Written the
  // moment it is selected, because a rule whose action is an empty object
  // passes validation and then does nothing.
  if (spec.type === 'button') {
    action.command = spec.endpoint ? { _path: spec.endpoint }
                                   : { [currentKey]: true };
  }

  let currentValue = (action.command || {})[currentKey];
  if (spec.type !== 'button' &&
      (currentValue === undefined || currentValue === '')) {
    if (spec.type === 'boolean') currentValue = true;
    else if (spec.type === 'enum') currentValue = (spec.values || [])[0];
    else if (spec.type === 'number') currentValue = spec.min !== undefined ? spec.min : 0;
    if (currentValue !== undefined) action.command = { [currentKey]: currentValue };
  }

  let value;
  if (spec.type === 'button') {
    value = el('span', { class: 'muted small', text: t('common.press', 'Press') });
  } else if (spec.type === 'enum') {
    value = el('select', {}, ...(spec.values || []).map((v) =>
      el('option', { value: v, text: v })));
  } else if (spec.type === 'boolean') {
    value = el('select', {},
      el('option', { value: 'true', text: t('common.on', 'On') }),
      el('option', { value: 'false', text: t('common.off', 'Off') }));
  } else {
    value = el('input', { type: 'number', step: String(spec.step || 1) });
  }

  if (spec.type !== 'button') {
    value.value = currentValue === undefined ? '' : String(currentValue);

    const store = () => {
      let parsed = value.value;
      if (spec.type === 'boolean') parsed = value.value === 'true';
      else if (spec.type === 'number') parsed = Number(value.value);
      action.command = { [currentKey]: parsed };
    };
    value.addEventListener('change', store);
    value.addEventListener('input', store);
  }

  return el('div', { class: 'rule-line' }, device, what, value,
    el('button', {
      class: 'danger ghost small-btn',
      onclick: () => { rule.actions.splice(index, 1); renderAutomations(); },
    }, '×'));
}

async function runRule(rule) {
  try {
    await api('/api/automations/run', { body: { id: rule.id } });
    toast(t('automations.ran', 'Rule run'), 'ok');
  } catch (error) { toast(error.message, 'error'); }
}

/* ══════════════════════════ wiring for both ═════════════════════════════ */

function wireDeviceEvents() {
  $('#device-discover').addEventListener('click', async () => {
    toast(t('devices.looking', 'Looking…'));
    try {
      const data = await api('/api/peers/discover', { method: 'POST', body: {} });
      state.candidates = data.candidates || [];
      renderDeviceList();
      toast(`${state.candidates.length} ${t('devices.found', 'found')}`, 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  // What the add form asks for depends on the type: a Wake-on-LAN target has
  // no address worth typing but must have a MAC, an ESPHome device needs its
  // entity id, an SLWF-12 needs neither.
  const describeType = () => {
    const info = typeInfo($('#device-type').value);
    const family = (info && info.family) || {};
    const extra = $('#device-entity');
    extra.hidden = !family.needsEntity;
    extra.placeholder = family.entityLabel || '';
    extra.title = family.entityPlaceholder
      ? `${family.entityLabel}, e.g. ${family.entityPlaceholder}` : '';
    $('#device-host').placeholder = family.defaultHost
      ? `${t('devices.address', 'Address or name')} (${family.defaultHost})`
      : t('devices.address', 'Address or name');
  };
  $('#device-type').addEventListener('change', describeType);
  describeType();

  $('#device-add').addEventListener('click', () => {
    const info = typeInfo($('#device-type').value);
    const family = (info && info.family) || {};
    const host = $('#device-host').value.trim() || family.defaultHost || '';
    const entity = $('#device-entity').value.trim();

    if (!host) { toast(t('devices.need_address', 'An address is required'), 'error'); return; }
    if (family.needsEntity && !entity) {
      toast(`${family.entityLabel || t('devices.entity', 'Extra detail')} ` +
            t('devices.is_required', 'is required'), 'error');
      return;
    }

    state.peers = state.peers || [];
    state.peers.push({
      host,
      name: $('#device-name').value.trim() || host,
      type: $('#device-type').value,
      token: $('#device-token').value,
      entity,
      port: family.defaultPort || 80,
      enabled: true,
    });
    $('#device-host').value = '';
    $('#device-name').value = '';
    $('#device-token').value = '';
    $('#device-entity').value = '';
    renderDeviceList();
  });

  $('#device-save').addEventListener('click', async () => {
    try {
      const data = await api('/api/peers', { body: { peers: state.peers || [] } });
      state.peers = data.peers || [];
      state.peerStatus = data.status || [];
      renderDeviceList();
      renderControl();
      toast(t('common.saved', 'Saved'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });

  $('#automation-add').addEventListener('click', () => {
    state.automations = state.automations || [];
    state.automations.push({
      name: t('automations.new', 'New rule'),
      enabled: true,
      match: 'all',
      for: 0,
      conditions: [{ device: 'self', field: 'power', op: 'eq', value: 'true' }],
      actions: [{ device: 'self', command: { power: true } }],
    });
    renderAutomations();
  });

  $('#automation-save').addEventListener('click', async () => {
    try {
      const data = await api('/api/automations',
                             { body: { automations: state.automations || [] } });
      state.automations = data.automations || [];
      renderAutomations();
      toast(t('common.saved', 'Saved'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  });
}

/* ═══════════════════════════ device cards ═══════════════════════════════
 *
 * One component, instantiated once per device. The local air conditioner and
 * every paired SLWF-12 get the same card from the same factory — not the same
 * DOM block re-pointed, which is what this used to be. That mattered for three
 * reasons: element ids were global so a second card would have collided, the
 * device being shown was implicit in a module variable rather than a
 * parameter, and there was no way to put two on screen at once.
 *
 * Each instance owns its own element references and closes over its own device
 * id, so `card.update(context)` is the whole interface.
 */

const MODE_ORDER = ['auto', 'cool', 'heat', 'dry', 'fan_only'];
const FAN_ORDER = ['auto', 'min', 'low', 'medium', 'medium_high', 'high', 'max'];
const SWING_ORDER = ['off', 'auto', 'highest', 'high', 'middle', 'low', 'lowest'];

function chipRow(className, values, labelFor, attribute, onPick) {
  const chips = values.map((value) => el('button', {
    class: 'chip', [attribute]: value,
    onclick: () => onPick(value),
  }, labelFor(value)));
  return { node: el('div', { class: className }, ...chips), chips };
}


/* Translating between this project's vocabulary and a device's own.
 *
 * The climate card speaks one language — power, mode, temp, fan, swingv. An
 * ESPHome air conditioner speaks another: no separate power at all, OFF is a
 * mode, and the names are shouted in capitals. The mapping lives in the type
 * database rather than in here, which is what lets the same card drive a
 * device nobody has written code for yet.
 *
 * With no mapping declared, the two vocabularies are the same — which is the
 * case for an SLWF-12 talking to an SLWF-12.
 */
function climateAdapter(device) {
  const info = typeInfo(device.typeId);
  const map = (info && info.family && info.family.climate) || null;

  if (!map) {
    return {
      toNative: (delta) => delta,
      fromNative: (native) => native || {},
      limits: null,
    };
  }

  const invert = (values) => {
    const out = {};
    for (const [ours, theirs] of Object.entries(values || {})) out[theirs] = ours;
    return out;
  };

  function toNative(delta) {
    const native = {};
    // Power is expressed as a mode, so both have to be resolved together.
    let mode = delta.hvac_mode || delta.mode;
    if (delta.power === false) mode = 'off';
    if (delta.power === true && !mode) {
      const current = fromNative(device.state);
      mode = current.power ? current.mode : (map.power.onFallback || 'cool').toLowerCase();
    }
    if (mode !== undefined) {
      native[map.mode.field] = mode === 'off'
        ? map.power.offValue
        : (map.mode.values[mode] || mode);
    }
    if (delta.temp !== undefined) native[map.temp.field] = delta.temp;
    if (delta.fan !== undefined && map.fan) {
      native[map.fan.field] = map.fan.values[delta.fan] || delta.fan;
    }
    if (delta.swingv !== undefined && map.swingv) {
      native[map.swingv.field] = map.swingv.values[delta.swingv] || delta.swingv;
    }
    return native;
  }

  function fromNative(native) {
    const source = native || {};
    const rawMode = source[map.mode.field];
    const mode = invert(map.mode.values)[rawMode];
    const off = rawMode === map.power.offValue || rawMode === undefined;

    return {
      power: !off,
      mode: off ? 'cool' : (mode || 'cool'),
      temp: Number(source[map.temp.field]),
      unit: 'C',
      fan: map.fan ? (invert(map.fan.values)[source[map.fan.field]] || 'auto') : 'auto',
      swingv: map.swingv
        ? (invert(map.swingv.values)[source[map.swingv.field]] || 'off') : 'off',
      current: map.current ? Number(source[map.current.field]) : undefined,
    };
  }

  const visual = (info && info.family && info.family.visual) || null;
  return {
    toNative,
    fromNative,
    limits: visual ? { minTemp: visual.min, maxTemp: visual.max, tempStep: visual.step }
                   : null,
  };
}

/* The thermostat. Used for anything whose type declares card: "climate". */
function createClimateCard(deviceId) {
  const send = (delta) => {
    const device = activeDeviceById(deviceId);
    commandDevice(deviceId, climateAdapter(device).toNative(delta));
  };

  const title = el('span', { class: 'climate-title' });
  const source = el('span', { class: 'pill' });

  const fill = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
  fill.setAttribute('class', 'dial-fill');
  fill.setAttribute('cx', '100'); fill.setAttribute('cy', '100');
  fill.setAttribute('r', '86');

  const track = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
  track.setAttribute('class', 'dial-track');
  track.setAttribute('cx', '100'); track.setAttribute('cy', '100');
  track.setAttribute('r', '86');

  const dial = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  dial.setAttribute('class', 'dial');
  dial.setAttribute('viewBox', '0 0 200 200');
  dial.setAttribute('aria-hidden', 'true');
  dial.append(track, fill);

  const temperature = el('div', { class: 'temp', text: '--' });
  const unit = el('div', { class: 'temp-unit', text: '°C' });
  const mode = el('div', { class: 'dial-mode' });

  const up = el('button', { class: 'dial-step up', 'aria-label': '+',
                            onclick: () => step(1) }, '+');
  const down = el('button', { class: 'dial-step down', 'aria-label': '−',
                              onclick: () => step(-1) }, '−');

  const power = el('button', { class: 'power', 'aria-label': 'power',
                               onclick: () => send({ power: !last.ac.power }) });
  power.innerHTML =
    '<svg viewBox="0 0 24 24"><path d="M12 3v9M6.5 6.8a8 8 0 1 0 11 0"/></svg>';

  const hold = el('p', { class: 'hold-notice', hidden: '' });
  // Scenes are a different kind of thing from mode, fan and swing: each one
  // sets several of those at once. Unlabelled they read as a fourth row of
  // the same, so they get a heading and a rule of their own.
  const sceneRow = el('div', { class: 'chip-row scene-row' });
  const sceneBlock = el('div', { class: 'scene-block' },
    el('span', { class: 'row-label', 'data-i18n': 'scenes.title' },
       t('scenes.title', 'Scenes')),
    sceneRow);

  const modes = chipRow('chip-row', MODE_ORDER,
    (value) => t(`mode.${value}`, value), 'data-mode',
    (value) => send({ hvac_mode: value }));
  const fans = chipRow('chip-row small', FAN_ORDER,
    (value) => t(`fan.${value}`, value), 'data-fan',
    (value) => send({ fan: value }));
  const swings = chipRow('chip-row small', SWING_ORDER,
    (value) => t(`swing.${value}`, value), 'data-swing',
    (value) => send({ swingv: value }));

  const resend = el('button', { class: 'ghost',
                                onclick: () => resendNow() },
                    t('control.resend', 'Send again'));
  const protocol = el('span', { class: 'muted' });

  const node = el('div', { class: 'card climate' },
    el('div', { class: 'climate-head' }, title, source),
    el('div', { class: 'dial-wrap' }, dial, up, down,
       el('div', { class: 'dial-centre' }, temperature, unit, mode)),
    // Standard controls first — mode, fan, swing are what the unit itself
    // has buttons for. Scenes are this device's own idea, so they sit below
    // the line rather than above it, where they read as the main control.
    power, hold, modes.node, fans.node, swings.node, sceneBlock,
    el('div', { class: 'climate-foot' }, resend, protocol));

  let last = { ac: {}, limits: { minTemp: 16, maxTemp: 30, tempStep: 1 }, isSelf: true };

  /* The configured range is Celsius, always — somebody sets it once, in the
   * unit they think in. Everything shown has to be in the unit the air
   * conditioner is actually speaking, so it is derived rather than stored
   * twice and left to disagree. */
  function limitsInUnit() {
    const limits = last.limits || {};
    if ((last.ac || {}).unit === 'F') {
      return {
        minTemp: Math.round(limits.minTemp * 9 / 5 + 32),
        maxTemp: Math.round(limits.maxTemp * 9 / 5 + 32),
        tempStep: 1,          // whole degrees; half a °F is not a thing
      };
    }
    return { minTemp: limits.minTemp, maxTemp: limits.maxTemp,
             tempStep: limits.tempStep || 1 };
  }

  function step(direction) {
    const bounds = limitsInUnit();
    send({ temp: (last.ac.temp || 24) + direction * bounds.tempStep });
  }

  async function resendNow() {
    try {
      if (deviceId === 'self') await api('/api/resend', { method: 'POST', body: {} });
      else await commandDevice(deviceId, { resend: true });
      toast(t('teach.sent', 'Sent'), 'ok');
    } catch (error) { toast(error.message, 'error'); }
  }

  function update(context) {
    last = context;
    const ac = context.ac || {};
    const limits = context.limits;

    title.textContent = context.device.name;
    temperature.textContent = Number.isFinite(ac.temp)
      ? (Math.round(ac.temp * 2) / 2).toFixed(Number.isInteger(ac.temp) ? 0 : 1)
      : '--';
    unit.textContent = `°${ac.unit || 'C'}`;
    mode.textContent = ac.power ? t(`mode.${ac.mode}`, ac.mode) : t('state.off', 'Off');

    const bounds = limitsInUnit();
    const span = Math.max(1, bounds.maxTemp - bounds.minTemp);
    const fraction = Math.min(1, Math.max(0, (ac.temp - bounds.minTemp) / span));
    fill.style.strokeDasharray = `${Math.max(6, fraction * 405)} 540`;
    fill.style.opacity = ac.power ? '1' : '0';
    fill.style.stroke = MODE_COLOUR[ac.mode] || 'var(--accent)';

    power.classList.toggle('on', !!ac.power);
    modes.chips.forEach((chip) =>
      chip.classList.toggle('active', !!ac.power && chip.dataset.mode === ac.mode));
    fans.chips.forEach((chip) =>
      chip.classList.toggle('active', chip.dataset.fan === ac.fan));
    swings.chips.forEach((chip) =>
      chip.classList.toggle('active', chip.dataset.swing === ac.swingv));

    // Scenes belong to the device that owns them, and this page has no way to
    // list another unit's — so the row appears only for the local one, and
    // only when there is something in it. A heading and a rule with nothing
    // underneath is worse than no heading.
    sceneBlock.hidden = !context.isSelf || (state.scenes || []).length === 0;
    if (!sceneBlock.hidden) renderSceneChipsInto(sceneRow);

    hold.hidden = !(context.isSelf && state.hold);
    if (!hold.hidden) {
      const pending = state.status && state.status.ac && state.status.ac.pendingStart;
      hold.textContent = pending
        ? `${t('control.hold_pending', 'Starting as soon as the compressor may restart')} · ${state.hold} s`
        : `${t('control.hold', 'Compressor protection: can restart in')} ${state.hold} s`;
    }

    const info = typeInfo(context.device.typeId);
    source.textContent = context.isSelf
      ? (state.status ? t(`source.${state.status.lastSource}`, state.status.lastSource) : '')
      : (info ? info.name : context.device.typeId);

    // The footer says which protocol the bridge speaks. A paired bridge has
    // one too, so its type declares where to find it; a device that reaches
    // its air conditioner over a wire rather than infrared has none, and the
    // footer stays empty rather than inventing something.
    const infoField = info && info.family && info.family.info;
    const infoValue = context.isSelf ? ac.protocol
                    : (infoField ? (context.device.state || {})[infoField] : '');
    protocol.textContent = !context.isSelf && !context.device.online
      ? t('devices.offline', 'offline')
      : (infoValue && infoValue !== 'UNKNOWN' ? infoValue
         : (context.isSelf ? t('control.no_protocol', 'not configured') : ''));

    const usable = context.isSelf
      ? !!(state.status && state.status.ac && state.status.ac.configured)
      : context.device.online;
    node.style.opacity = usable ? '1' : '.55';
  }

  return { node, update, deviceId, kind: 'climate' };
}

/* Everything else: controls generated from the type's declared actions. */
function createGenericCard(deviceId) {
  const title = el('span', { class: 'climate-title' });
  const badge = el('span', { class: 'pill' });
  const body = el('div');
  const node = el('div', { class: 'card' },
    el('div', { class: 'climate-head' }, title, badge), body);

  function update(context) {
    const device = context.device;
    const info = typeInfo(device.typeId);
    const family = (info && info.family) || {};

    title.replaceChildren(icon(info && info.icon, 'card-icon'),
                          document.createTextNode(device.name));
    badge.textContent = info ? info.name : device.typeId;

    if (!device.online) {
      body.replaceChildren(el('p', { class: 'muted',
        text: t('devices.no_answer', 'This device is not answering.') }));
      return;
    }
    const actions = (family.actions || [])
      .filter((action) => action.type !== 'scene');

    // Something with nothing to press is a reading, not a control. Show what
    // it last said rather than an empty card.
    if (actions.length === 0) {
      const values = Object.entries(device.state || {});
      body.replaceChildren(values.length
        ? el('dl', { class: 'kv' }, ...values.flatMap(([key, value]) => [
            el('dt', { text: key }), el('dd', { text: String(value) })]))
        : el('p', { class: 'muted',
                    text: t('devices.no_readings', 'Nothing read yet.') }));
      return;
    }

    body.replaceChildren(...actions.map(
      (action) => renderAction(device, action, device.state || {})));
  }

  return { node, update, deviceId, kind: 'generic' };
}

/* One card per device, created on demand and kept. */
function cardFor(device) {
  state.cards = state.cards || {};
  const info = typeInfo(device.typeId);
  const kind = ((info && info.family && info.family.card) || 'climate') === 'climate'
    ? 'climate' : 'generic';

  const existing = state.cards[device.id];
  if (existing && existing.kind === kind) return existing;

  const card = kind === 'climate' ? createClimateCard(device.id)
                                  : createGenericCard(device.id);
  state.cards[device.id] = card;
  return card;
}

/* ══════════════════════════════════ start ═══════════════════════════════ */

async function main() {
  initTheme();
  wireEvents();
  wireDeviceEvents();
  await loadDeviceTypes();
  try {
    await refreshConfig();
  } catch (error) {
    toast(error.message, 'error');
  }
  await initLanguages();
  await refreshStatus();
  await refreshScenes();
  await refreshPeers();
  connectSocket();

  showView((location.hash || '#control').slice(1));
  $('#boot').classList.add('gone');

  // The websocket carries live state; this catches anything that only appears
  // in the fuller status document (heap, IR counters, Wi-Fi quality).
  setInterval(() => refreshStatus(/*brief=*/true), 15000);
  setInterval(tickClock, 1000);
}

main();
