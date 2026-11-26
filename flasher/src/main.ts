import './style.css';
import { EspWebFlasher, type ChipInfo, type FlashPart } from './lib/flasher';
import { SerialMonitor, type LineEnding } from './lib/serial';

interface CatalogPartition {
  offset: string;
  url: string;
  name: string;
}

interface CatalogEntry {
  id: string;
  name: string;
  version: string;
  chipFamily: string;
  badge: string;
  description: string;
  descriptionVi: string;
  models: string[];
  partitions: CatalogPartition[];
}

interface LocalPart {
  id: number;
  offsetHex: string;
  file: File;
}

type Lang = 'vi' | 'en';
type Tab = 'flasher' | 'serial';
type SourceMode = 'catalog' | 'local';

const STRINGS = {
  vi: {
    subtitle: 'Nạp Firmware & Serial Monitor qua Web Serial API cho ESP32-S3 AirPrint',
    tabFlash: 'Nạp Firmware',
    tabSerial: 'Serial Monitor',
    noSerial: 'Trình duyệt này chưa hỗ trợ Web Serial API. Hãy dùng Google Chrome hoặc Microsoft Edge bản Desktop.',
    connTitle: 'Kết nối mạch ESP32',
    baud: 'Tốc độ nạp (Baudrate)',
    cleanFlash: 'Xóa toàn bộ Flash trước khi nạp (Clean Flash)',
    connectEsp: 'Kết Nối ESP',
    disconnect: 'Ngắt kết nối',
    eraseOnly: 'Xóa Flash',
    hardReset: 'Reset cứng',
    statusConnected: 'Đã kết nối',
    statusDisconnected: 'Chưa kết nối chip',
    chipHint: 'Cắm cáp USB vào cổng COM/UART (hoặc giữ BOOT khi cắm) rồi bấm "Kết Nối ESP".',
    fwSourceCatalog: 'Kho Firmware có sẵn',
    fwSourceLocal: 'Tải file .bin riêng',
    searchPlaceholder: 'Tìm firmware (Universal, Canon LBP, HP LaserJet, Brother...)',
    flashBtn: 'Bắt đầu nạp Firmware',
    flashing: 'Đang nạp...',
    logsTitle: 'Nhật ký nạp (Flash Logs)',
    clear: 'Xóa log',
    copy: 'Sao chép',
    dropHint: 'Kéo thả hoặc chọn file .bin (merged factory.bin ở 0x0, hoặc từng phần 0x0 / 0x8000 / 0x10000)',
    addPart: 'Chọn file .bin',
    serialTitle: 'Serial Monitor',
    serialConnect: 'Kết nối Serial',
    autoScroll: 'Tự cuộn',
    timestamps: 'Hiện thời gian',
    download: 'Tải .txt',
    send: 'Gửi',
    cmdPlaceholder: 'Nhập lệnh hoặc chuỗi gửi xuống ESP32...',
    missingBin: 'Chưa có file nhị phân được build sẵn tại URL này. Hãy chạy "pio run -e esp32s3" + "python tools/make_factory_bin.py --copy-to-flasher" hoặc chuyển sang tab "Tải file .bin riêng".',
  },
  en: {
    subtitle: 'Web Serial API Firmware Flasher & Serial Monitor for ESP32-S3 AirPrint',
    tabFlash: 'Web Flasher',
    tabSerial: 'Serial Monitor',
    noSerial: 'Web Serial API is unavailable in this browser. Please use desktop Google Chrome or Microsoft Edge.',
    connTitle: 'ESP32 Connection',
    baud: 'Baudrate',
    cleanFlash: 'Erase entire flash before writing (Clean Flash)',
    connectEsp: 'Connect ESP32',
    disconnect: 'Disconnect',
    eraseOnly: 'Erase Flash',
    hardReset: 'Hard Reset',
    statusConnected: 'Connected',
    statusDisconnected: 'Not connected',
    chipHint: 'Plug USB into the COM/UART port (or hold BOOT while plugging in) and click "Connect ESP32".',
    fwSourceCatalog: 'Firmware Catalog',
    fwSourceLocal: 'Upload Local .bin',
    searchPlaceholder: 'Search firmware (Universal, Canon LBP, HP LaserJet, Brother...)',
    flashBtn: 'Flash Firmware',
    flashing: 'Flashing...',
    logsTitle: 'Flash Logs',
    clear: 'Clear',
    copy: 'Copy',
    dropHint: 'Drop or pick .bin files (single factory.bin at 0x0, or split 0x0 / 0x8000 / 0x10000)',
    addPart: 'Pick .bin file',
    serialTitle: 'Serial Monitor',
    serialConnect: 'Connect Serial',
    autoScroll: 'Auto-scroll',
    timestamps: 'Timestamps',
    download: 'Download .txt',
    send: 'Send',
    cmdPlaceholder: 'Type command to send over UART...',
    missingBin: 'Prebuilt binary not found at this URL yet. Run "pio run -e esp32s3" + "python tools/make_factory_bin.py --copy-to-flasher", or switch to "Upload Local .bin".',
  },
} as const;

const flasher = new EspWebFlasher();
const monitor = new SerialMonitor();

let lang: Lang = 'vi';
let tab: Tab = 'flasher';
let sourceMode: SourceMode = 'catalog';
let baudrate = 460800;
let eraseAll = false;
let chip: ChipInfo | null = null;
let catalog: CatalogEntry[] = [];
let selectedId = '';
let searchQuery = '';
let localParts: LocalPart[] = [];
let nextPartId = 1;
let busy = false;
let progressPct = 0;
let progressLabel = '';
let flashLogs = '';

let monBaud = 115200;
let monConnected = false;
let monAutoScroll = true;
let monTimestamps = false;
let monEnding: LineEnding = 'crlf';
let monBuffer = '';

function t() {
  return STRINGS[lang];
}

function appendFlashLog(msg: string) {
  flashLogs += msg;
  const el = document.getElementById('flash-terminal');
  if (el) {
    el.textContent = flashLogs;
    el.scrollTop = el.scrollHeight;
  }
}

function appendMonitor(text: string) {
  let out = text;
  if (monTimestamps) {
    const ts = new Date().toLocaleTimeString('en-GB', { hour12: false });
    out = out.replace(/(^|\n)(?=.)/g, `$1[${ts}] `);
  }
  monBuffer += out;
  if (monBuffer.length > 200_000) {
    monBuffer = monBuffer.slice(-150_000);
  }
  const el = document.getElementById('serial-terminal');
  if (el) {
    el.textContent = monBuffer;
    if (monAutoScroll) el.scrollTop = el.scrollHeight;
  }
}

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}

function parseHexOffset(s: string): number {
  const clean = s.trim().toLowerCase();
  const val = clean.startsWith('0x') ? parseInt(clean, 16) : parseInt(clean, 10);
  if (!Number.isFinite(val) || val < 0) {
    throw new Error(`Invalid flash offset: ${s}`);
  }
  return val;
}

function inferOffset(filename: string): string {
  const lower = filename.toLowerCase();
  if (lower.includes('partition')) return '0x8000';
  if (lower.includes('boot_app0') || lower.includes('ota_data')) return '0xe000';
  if (lower.includes('bootloader') || lower.includes('factory')) return '0x0';
  if (lower.includes('firmware') || lower.includes('app')) return localParts.length === 0 ? '0x10000' : '0x10000';
  return '0x0';
}

function addLocalFiles(files: FileList | null) {
  if (!files) return;
  for (const file of Array.from(files)) {
    localParts.push({ id: nextPartId++, offsetHex: inferOffset(file.name), file });
  }
  render();
}

async function loadCatalog() {
  try {
    const r = await fetch('/firmwares/catalog.json', { cache: 'no-store' });
    if (!r.ok) return;
    catalog = (await r.json()) as CatalogEntry[];
    if (catalog.length && !selectedId) selectedId = catalog[0].id;
  } catch {
    catalog = [];
  }
  render();
}

async function handleConnectFlasher() {
  if (flasher.connected) {
    await flasher.disconnect();
    chip = null;
    appendFlashLog('\n[disconnected]\n');
    render();
    return;
  }
  if (monConnected) {
    await monitor.disconnect();
    monConnected = false;
  }
  busy = true;
  render();
  try {
    const terminal = {
      clean: () => {
        flashLogs = '';
        render();
      },
      writeLine: (data: string) => appendFlashLog(data + '\n'),
      write: (data: string) => appendFlashLog(data),
    };
    chip = await flasher.connect(baudrate, terminal);
    appendFlashLog(`\nConnected: ${chip.chipName} (MAC ${chip.mac}, Flash ${chip.flashSizeKb} KB)\n`);
  } catch (e) {
    appendFlashLog(`\nError: ${(e as Error).message}\n`);
  } finally {
    busy = false;
    render();
  }
}

async function handleErase() {
  if (!flasher.connected || busy) return;
  busy = true;
  render();
  try {
    appendFlashLog('\nErasing entire flash...\n');
    await flasher.eraseFlash();
    appendFlashLog('Flash erased.\n');
  } catch (e) {
    appendFlashLog(`Erase failed: ${(e as Error).message}\n`);
  } finally {
    busy = false;
    render();
  }
}

async function handleFlash() {
  if (!flasher.connected || busy) return;
  busy = true;
  progressPct = 0;
  progressLabel = t().flashing;
  render();

  try {
    const parts: FlashPart[] = [];
    if (sourceMode === 'catalog') {
      const entry = catalog.find((c) => c.id === selectedId);
      if (!entry) throw new Error('Select a firmware from the catalog');
      for (const p of entry.partitions) {
        appendFlashLog(`Fetching ${p.url}...\n`);
        const res = await fetch(p.url, { cache: 'no-store' });
        if (!res.ok) throw new Error(`${res.status} ${p.url} — ${t().missingBin}`);
        const buf = new Uint8Array(await res.arrayBuffer());
        // Guard against SPA HTML fallback being returned for missing .bin
        if (buf.length < 32 || (buf[0] === 0x3c && buf[1] === 0x21)) {
          throw new Error(`${p.url} — ${t().missingBin}`);
        }
        parts.push({ address: parseHexOffset(p.offset), data: buf, name: p.name });
      }
    } else {
      if (!localParts.length) throw new Error('Add at least one .bin file');
      for (const lp of localParts) {
        const buf = new Uint8Array(await lp.file.arrayBuffer());
        parts.push({ address: parseHexOffset(lp.offsetHex), data: buf, name: lp.file.name });
      }
    }

    await flasher.flash(parts, eraseAll, (idx, written, total) => {
      const pct = total > 0 ? Math.round((written / total) * 100) : 0;
      progressPct = pct;
      progressLabel = `${parts[idx]?.name || 'image'}: ${pct}% (${written}/${total})`;
      const bar = document.getElementById('flash-progress-bar');
      const lbl = document.getElementById('flash-progress-label');
      if (bar) bar.style.width = `${pct}%`;
      if (lbl) lbl.textContent = progressLabel;
    });
    progressPct = 100;
    progressLabel = 'Done (100%)';
    appendFlashLog('\nFlash completed and board reset!\n');
  } catch (e) {
    appendFlashLog(`\nFlash failed: ${(e as Error).message}\n`);
  } finally {
    busy = false;
    render();
  }
}

async function handleConnectSerial() {
  if (monConnected) {
    await monitor.disconnect();
    monConnected = false;
    render();
    return;
  }
  if (flasher.connected) {
    await flasher.disconnect();
    chip = null;
  }
  try {
    await monitor.connect(
      monBaud,
      (txt) => appendMonitor(txt),
      () => {
        monConnected = false;
        render();
      },
    );
    monConnected = true;
  } catch (e) {
    appendMonitor(`\n[error: ${(e as Error).message}]\n`);
  }
  render();
}

function render() {
  const root = document.getElementById('app');
  if (!root) return;
  const s = t();
  const hasSerial = typeof navigator !== 'undefined' && 'serial' in navigator;

  const filteredCatalog = catalog.filter((c) => {
    const q = searchQuery.trim().toLowerCase();
    if (!q) return true;
    return (
      c.name.toLowerCase().includes(q) ||
      c.description.toLowerCase().includes(q) ||
      c.descriptionVi.toLowerCase().includes(q) ||
      c.models.some((m) => m.toLowerCase().includes(q))
    );
  });

  root.innerHTML = `
    <div class="glow-bg"></div>
    <header class="topbar">
      <div class="container topbar-inner">
        <div class="brand">
          <div class="brand-icon">AP</div>
          <div>
            <h1>ESP32 AirPrint Studio</h1>
            <p>${esc(s.subtitle)}</p>
          </div>
        </div>
        <div class="nav-actions">
          <button class="btn tab ${tab === 'flasher' ? 'active' : ''}" id="tab-flash">${esc(s.tabFlash)}</button>
          <button class="btn tab ${tab === 'serial' ? 'active' : ''}" id="tab-serial">${esc(s.tabSerial)}</button>
          <button class="btn" id="btn-lang">${lang.toUpperCase()}</button>
        </div>
      </div>
    </header>

    <main class="container">
      ${!hasSerial ? `<div class="alert">${esc(s.noSerial)}</div>` : ''}
      ${
        tab === 'flasher'
          ? `
        <div class="grid-2">
          <section class="card stack">
            <div class="row">
              <h2 class="grow" style="margin:0">${esc(s.connTitle)}</h2>
              <span class="pill ${chip ? 'ok' : 'warn'}">${esc(chip ? s.statusConnected : s.statusDisconnected)}</span>
            </div>
            <p class="small muted" style="margin:0">
              ${
                chip
                  ? `Chip: <b>${esc(chip.chipName)}</b> · MAC: <code>${esc(chip.mac)}</code> · Flash: ${chip.flashSizeKb} KB`
                  : esc(s.chipHint)
              }
            </p>
            <label class="field">
              <span>${esc(s.baud)}</span>
              <select id="sel-baud" ${chip || busy ? 'disabled' : ''}>
                ${[921600, 460800, 230400, 115200]
                  .map((b) => `<option value="${b}" ${baudrate === b ? 'selected' : ''}>${b.toLocaleString()} bps</option>`)
                  .join('')}
              </select>
            </label>
            <label class="checkbox">
              <input type="checkbox" id="chk-erase" ${eraseAll ? 'checked' : ''} ${busy ? 'disabled' : ''} />
              <span>${esc(s.cleanFlash)}</span>
            </label>
            <div class="row wrap">
              <button class="btn primary grow" id="btn-connect" ${!hasSerial || busy ? 'disabled' : ''}>
                ${esc(chip ? s.disconnect : s.connectEsp)}
              </button>
              ${
                chip
                  ? `<button class="btn danger" id="btn-erase" ${busy ? 'disabled' : ''}>${esc(s.eraseOnly)}</button>
                     <button class="btn" id="btn-reset" ${busy ? 'disabled' : ''}>${esc(s.hardReset)}</button>`
                  : ''
              }
            </div>
          </section>

          <section class="card stack">
            <div class="row wrap">
              <button class="btn tab ${sourceMode === 'catalog' ? 'active' : ''}" id="src-catalog">${esc(s.fwSourceCatalog)}</button>
              <button class="btn tab ${sourceMode === 'local' ? 'active' : ''}" id="src-local">${esc(s.fwSourceLocal)}</button>
              <button class="btn primary" style="margin-left:auto" id="btn-flash" ${!chip || busy ? 'disabled' : ''}>
                ${esc(busy ? s.flashing : s.flashBtn)}
              </button>
            </div>

            ${
              sourceMode === 'catalog'
                ? `
              <input type="search" id="inp-search" value="${esc(searchQuery)}" placeholder="${esc(s.searchPlaceholder)}" />
              <div class="catalog-grid">
                ${filteredCatalog
                  .map((c) => {
                    const desc = lang === 'vi' ? c.descriptionVi : c.description;
                    return `
                    <button class="fw-card ${selectedId === c.id ? 'selected' : ''}" data-fw="${esc(c.id)}">
                      <div class="row">
                        <strong class="grow">${esc(c.name)}</strong>
                        <span class="pill">${esc(c.version)}</span>
                      </div>
                      <div class="small muted">${esc(c.chipFamily)} · ${esc(c.badge)}</div>
                      <div class="small">${esc(desc)}</div>
                      <ul>${c.models.slice(0, 4).map((m) => `<li>${esc(m)}</li>`).join('')}</ul>
                    </button>`;
                  })
                  .join('')}
              </div>`
                : `
              <div class="dropzone stack" id="dropzone">
                <div class="small muted">${esc(s.dropHint)}</div>
                <div>
                  <button class="btn" id="btn-pick-bin" type="button">${esc(s.addPart)}</button>
                  <input type="file" id="file-bin" accept=".bin" multiple hidden />
                </div>
              </div>
              <div class="stack">
                ${localParts
                  .map(
                    (p) => `
                  <div class="part-row">
                    <input type="text" class="mono part-off" data-id="${p.id}" value="${esc(p.offsetHex)}" />
                    <div class="small mono" style="overflow:hidden;text-overflow:ellipsis">${esc(p.file.name)} (${(p.file.size / 1024).toFixed(1)} KB)</div>
                    <button class="btn danger part-del" data-id="${p.id}" type="button">×</button>
                  </div>`,
                  )
                  .join('')}
              </div>`
            }

            <div class="stack" style="gap:6px">
              <div class="row small muted">
                <span class="grow" id="flash-progress-label">${esc(progressLabel || 'Idle')}</span>
                <span>${progressPct}%</span>
              </div>
              <div class="progress-wrap"><div class="progress-bar" id="flash-progress-bar" style="width:${progressPct}%"></div></div>
            </div>

            <div class="row">
              <h3 class="grow" style="margin:0">${esc(s.logsTitle)}</h3>
              <button class="btn" id="btn-copy-log">${esc(s.copy)}</button>
              <button class="btn" id="btn-clear-log">${esc(s.clear)}</button>
            </div>
            <pre class="terminal" id="flash-terminal">${esc(flashLogs)}</pre>
          </section>
        </div>`
          : `
        <section class="card stack">
          <div class="row wrap">
            <h2 class="grow" style="margin:0">${esc(s.serialTitle)}</h2>
            <select id="sel-mon-baud" style="width:150px" ${monConnected ? 'disabled' : ''}>
              ${[115200, 921600, 460800, 230400, 74880, 9600]
                .map((b) => `<option value="${b}" ${monBaud === b ? 'selected' : ''}>${b} baud</option>`)
                .join('')}
            </select>
            <button class="btn primary" id="btn-mon-connect" ${!hasSerial ? 'disabled' : ''}>
              ${esc(monConnected ? s.disconnect : s.serialConnect)}
            </button>
            <button class="btn" id="btn-mon-reset" ${!monConnected ? 'disabled' : ''}>${esc(s.hardReset)}</button>
            <label class="checkbox"><input type="checkbox" id="chk-scroll" ${monAutoScroll ? 'checked' : ''}/><span>${esc(s.autoScroll)}</span></label>
            <label class="checkbox"><input type="checkbox" id="chk-ts" ${monTimestamps ? 'checked' : ''}/><span>${esc(s.timestamps)}</span></label>
            <button class="btn" id="btn-mon-dl">${esc(s.download)}</button>
            <button class="btn" id="btn-mon-clear">${esc(s.clear)}</button>
          </div>
          <pre class="terminal tall" id="serial-terminal">${esc(monBuffer)}</pre>
          <form class="row" id="mon-form">
            <input type="text" id="mon-input" class="mono grow" placeholder="${esc(s.cmdPlaceholder)}" ${!monConnected ? 'disabled' : ''} />
            <select id="sel-ending" style="width:105px">
              <option value="crlf" ${monEnding === 'crlf' ? 'selected' : ''}>CRLF</option>
              <option value="lf" ${monEnding === 'lf' ? 'selected' : ''}>LF</option>
              <option value="none" ${monEnding === 'none' ? 'selected' : ''}>None</option>
            </select>
            <button class="btn primary" type="submit" ${!monConnected ? 'disabled' : ''}>${esc(s.send)}</button>
          </form>
        </section>`
      }
    </main>
  `;

  bindEvents();
}

function bindEvents() {
  document.getElementById('tab-flash')?.addEventListener('click', () => {
    tab = 'flasher';
    render();
  });
  document.getElementById('tab-serial')?.addEventListener('click', () => {
    tab = 'serial';
    render();
  });
  document.getElementById('btn-lang')?.addEventListener('click', () => {
    lang = lang === 'vi' ? 'en' : 'vi';
    render();
  });

  document.getElementById('sel-baud')?.addEventListener('change', (e) => {
    baudrate = Number((e.target as HTMLSelectElement).value);
  });
  document.getElementById('chk-erase')?.addEventListener('change', (e) => {
    eraseAll = (e.target as HTMLInputElement).checked;
  });
  document.getElementById('btn-connect')?.addEventListener('click', () => void handleConnectFlasher());
  document.getElementById('btn-erase')?.addEventListener('click', () => void handleErase());
  document.getElementById('btn-reset')?.addEventListener('click', () => void flasher.hardReset());
  document.getElementById('btn-flash')?.addEventListener('click', () => void handleFlash());

  document.getElementById('src-catalog')?.addEventListener('click', () => {
    sourceMode = 'catalog';
    render();
  });
  document.getElementById('src-local')?.addEventListener('click', () => {
    sourceMode = 'local';
    render();
  });
  document.getElementById('inp-search')?.addEventListener('input', (e) => {
    searchQuery = (e.target as HTMLInputElement).value;
    render();
    const inp = document.getElementById('inp-search') as HTMLInputElement | null;
    if (inp) {
      inp.focus();
      inp.setSelectionRange(searchQuery.length, searchQuery.length);
    }
  });
  document.querySelectorAll<HTMLElement>('[data-fw]').forEach((el) => {
    el.addEventListener('click', () => {
      selectedId = el.dataset.fw || '';
      render();
    });
  });

  const fileInput = document.getElementById('file-bin') as HTMLInputElement | null;
  document.getElementById('btn-pick-bin')?.addEventListener('click', () => fileInput?.click());
  fileInput?.addEventListener('change', () => addLocalFiles(fileInput.files));
  const dz = document.getElementById('dropzone');
  dz?.addEventListener('dragover', (e) => {
    e.preventDefault();
    dz.classList.add('drag');
  });
  dz?.addEventListener('dragleave', () => dz.classList.remove('drag'));
  dz?.addEventListener('drop', (e) => {
    e.preventDefault();
    dz.classList.remove('drag');
    addLocalFiles((e as DragEvent).dataTransfer?.files || null);
  });
  document.querySelectorAll<HTMLInputElement>('.part-off').forEach((el) => {
    el.addEventListener('change', () => {
      const id = Number(el.dataset.id);
      const part = localParts.find((p) => p.id === id);
      if (part) part.offsetHex = el.value;
    });
  });
  document.querySelectorAll<HTMLElement>('.part-del').forEach((el) => {
    el.addEventListener('click', () => {
      const id = Number(el.dataset.id);
      localParts = localParts.filter((p) => p.id !== id);
      render();
    });
  });

  document.getElementById('btn-clear-log')?.addEventListener('click', () => {
    flashLogs = '';
    render();
  });
  document.getElementById('btn-copy-log')?.addEventListener('click', () => {
    void navigator.clipboard?.writeText(flashLogs);
  });

  document.getElementById('sel-mon-baud')?.addEventListener('change', (e) => {
    monBaud = Number((e.target as HTMLSelectElement).value);
  });
  document.getElementById('btn-mon-connect')?.addEventListener('click', () => void handleConnectSerial());
  document.getElementById('btn-mon-reset')?.addEventListener('click', () => void monitor.hardReset());
  document.getElementById('chk-scroll')?.addEventListener('change', (e) => {
    monAutoScroll = (e.target as HTMLInputElement).checked;
  });
  document.getElementById('chk-ts')?.addEventListener('change', (e) => {
    monTimestamps = (e.target as HTMLInputElement).checked;
  });
  document.getElementById('btn-mon-clear')?.addEventListener('click', () => {
    monBuffer = '';
    render();
  });
  document.getElementById('btn-mon-dl')?.addEventListener('click', () => {
    const blob = new Blob([monBuffer], { type: 'text/plain;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `esp32-serial-${Date.now()}.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  });
  document.getElementById('sel-ending')?.addEventListener('change', (e) => {
    monEnding = (e.target as HTMLSelectElement).value as LineEnding;
  });
  document.getElementById('mon-form')?.addEventListener('submit', (e) => {
    e.preventDefault();
    const inp = document.getElementById('mon-input') as HTMLInputElement | null;
    if (!inp || !monConnected) return;
    const text = inp.value;
    inp.value = '';
    void monitor.send(text, monEnding);
  });
}

render();
void loadCatalog();
