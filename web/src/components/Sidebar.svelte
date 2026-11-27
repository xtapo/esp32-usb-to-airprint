<script lang="ts">
  import Printer from '@lucide/svelte/icons/printer';
  import ScanLine from '@lucide/svelte/icons/scan-line';
  import FolderOpen from '@lucide/svelte/icons/folder-open';
  import Layers from '@lucide/svelte/icons/layers';
  import Wifi from '@lucide/svelte/icons/wifi';
  import ChevronRight from '@lucide/svelte/icons/chevron-right';
  import { device } from '../lib/device.svelte';
  import { files } from '../lib/files.svelte';
  import { work } from '../lib/process.svelte';
  import { scanProgress } from '../lib/scan.svelte';
  import { i18n } from '../lib/i18n.svelte';

  let { current, home }: { current: string; home: boolean } = $props();

  const printerLine = $derived.by(() => {
    const t = i18n.t.nav;
    const p = device.printer;
    if (!device.loaded) return t.connecting;
    if (!p) return t.unavailable;
    if (!p.connected) return t.noPrinter;
    const job = p.jobs.find((j) => j.id === p.active);
    if (job && job.state === 'processing') return t.printingPage(job.pages + 1);
    if (device.scan?.state === 'scanning') return t.busyScanning;
    const low = p.supplies.find((s) => s.level >= 0 && s.level <= 10);
    if (p.reasons.some((r) => r.endsWith('-error'))) return p.message;
    return low ? t.readyLowInk(low.name) : t.ready;
  });

  const scanLine = $derived.by(() => {
    const t = i18n.t.nav;
    const s = device.scan;
    if (!device.loaded) return t.connecting;
    if (!s || !s.scanner) return t.noScanner;
    if (s.state === 'scanning') return s.lines ? t.scanningPct(Math.round(scanProgress(s) * 100)) : t.warmingUp;
    return t.ready;
  });

  const items = $derived([
    { id: 'printer', label: i18n.t.routes.printer, icon: Printer, line: printerLine, desc: i18n.t.nav.printerDesc },
    { id: 'scanner', label: i18n.t.routes.scanner, icon: ScanLine, line: scanLine, desc: i18n.t.nav.scannerDesc },
    {
      id: 'files',
      label: i18n.t.routes.files,
      icon: FolderOpen,
      line: files.loaded ? i18n.t.nav.fileCount(files.list.length) : '…',
      desc: i18n.t.nav.filesDesc,
    },
    {
      id: 'process',
      label: i18n.t.routes.process,
      icon: Layers,
      line: work.items.length ? i18n.t.nav.pageCount(work.items.length) : i18n.t.nav.empty,
      desc: i18n.t.nav.processDesc,
    },
    {
      id: 'wifi',
      label: i18n.t.routes.wifi,
      icon: Wifi,
      line: device.online ? i18n.t.nav.wifiLineOnline : i18n.t.nav.wifiLineSetup,
      desc: i18n.t.nav.wifiDesc,
    },
  ]);

  const tone = $derived.by(() => {
    const p = device.printer;
    if (!device.online) return 'err';
    if (!p?.connected) return 'warn';
    if (p.state === 'processing' || device.scan?.state === 'scanning') return 'busy';
    return 'ok';
  });
</script>

<nav class="sidebar" class:home>
  <div class="brand">
    <div class="logo"><Printer size={22} /></div>
    <div class="grow">
      <div class="name ellipsis">{device.printer?.name || i18n.t.routes.printer}</div>
      <div class="model ellipsis">{device.printer?.model || ''}</div>
    </div>
  </div>

  {#if home}
    <p class="hello">{i18n.t.nav.hello}</p>
  {/if}

  <ul>
    {#each items as it (it.id)}
      {@const Icon = it.icon}
      <li>
        <a href={'#/' + it.id} class:on={current === it.id} aria-current={current === it.id ? 'page' : undefined}>
          <span class="ic"><Icon size={home ? 24 : 19} /></span>
          <span class="txt">
            <span class="label">{it.label}</span>
            <span class="line ellipsis">{home ? it.desc : it.line}</span>
            {#if home}<span class="status ellipsis">{it.line}</span>{/if}
          </span>
          {#if home}<ChevronRight size={18} class="chev" />{/if}
        </a>
      </li>
    {/each}
  </ul>

  <div class="foot">
    <span class="pill {tone}">
      <span class="dot"></span>
      {device.online
        ? tone === 'busy'
          ? i18n.t.nav.working
          : tone === 'warn'
            ? i18n.t.nav.noPrinterShort
            : i18n.t.nav.online
        : i18n.t.nav.reconnecting}
    </span>
    <button
      type="button"
      class="btn lang-btn"
      onclick={() => i18n.toggle()}
      title="Switch language (VI / EN)"
    >
      {i18n.lang.toUpperCase()}
    </button>
  </div>
</nav>

<style>
  .sidebar {
    display: flex;
    flex-direction: column;
    gap: 6px;
    height: 100%;
    padding: 18px 14px;
    padding-top: max(18px, env(safe-area-inset-top));
    background: var(--surface);
    border-right: 1px solid var(--border);
    overflow: auto;
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 11px;
    padding: 4px 6px 16px;
  }
  .logo {
    display: grid;
    place-items: center;
    width: 40px;
    height: 40px;
    border-radius: 12px;
    background: linear-gradient(145deg, var(--accent), color-mix(in srgb, var(--accent) 60%, #a855f7));
    color: #fff;
    flex: none;
  }
  .name {
    font-weight: 650;
  }
  .model {
    font-size: 0.8rem;
    color: var(--muted);
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 3px;
  }
  a {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 9px 10px;
    border-radius: 11px;
    color: var(--text);
    text-decoration: none;
  }
  a:hover {
    background: var(--surface-2);
  }
  a.on {
    background: var(--accent-soft);
  }
  a.on .ic {
    color: var(--accent);
  }
  .ic {
    display: grid;
    place-items: center;
    color: var(--muted);
    flex: none;
  }
  .txt {
    display: grid;
    min-width: 0;
    flex: 1;
  }
  .label {
    font-weight: 600;
  }
  .line {
    font-size: 0.8rem;
    color: var(--muted);
  }
  .foot {
    margin-top: auto;
    padding: 12px 6px 0;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }
  .lang-btn {
    padding: 4px 10px;
    min-height: 28px;
    font-size: 0.75rem;
    font-weight: 700;
  }

  /* phones: the menu is the first screen */
  .home {
    border-right: 0;
    background: var(--bg);
    padding: 22px 16px;
    padding-top: max(22px, env(safe-area-inset-top));
  }
  .home .brand {
    padding-bottom: 8px;
  }
  .home .name {
    font-size: 1.15rem;
  }
  .hello {
    font-size: 1.5rem;
    font-weight: 700;
    letter-spacing: -0.02em;
    margin: 14px 4px 14px;
  }
  .home ul {
    gap: 10px;
  }
  .home a {
    background: var(--surface);
    border: 1px solid var(--border);
    box-shadow: var(--shadow);
    border-radius: var(--radius);
    padding: 16px;
    gap: 14px;
  }
  .home .ic {
    width: 46px;
    height: 46px;
    border-radius: 13px;
    background: var(--accent-soft);
    color: var(--accent);
  }
  .home .label {
    font-size: 1.05rem;
  }
  .home .status {
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--accent);
    margin-top: 2px;
  }
  .home :global(.chev) {
    color: var(--muted);
    flex: none;
  }
  @media (min-width: 761px) {
    .hello {
      display: none;
    }
  }
</style>
