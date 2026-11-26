<script lang="ts">
  import { onMount } from 'svelte';
  import Wifi from '@lucide/svelte/icons/wifi';
  import RefreshCw from '@lucide/svelte/icons/refresh-cw';
  import Lock from '@lucide/svelte/icons/lock';
  import Eye from '@lucide/svelte/icons/eye';
  import EyeOff from '@lucide/svelte/icons/eye-off';
  import RotateCcw from '@lucide/svelte/icons/rotate-ccw';
  import Check from '@lucide/svelte/icons/check';
  import { toast } from '../lib/ui.svelte';

  interface WifiInfo {
    connected: boolean;
    ap_active: boolean;
    rssi: number;
    ssid: string;
    ip: string;
    ap_ssid: string;
    ap_ip: string;
    hostname: string;
    printer_name: string;
    driver: string;
    model: string;
    driver_count: number;
  }

  interface ScannedNet {
    ssid: string;
    rssi: number;
    secure: boolean;
  }

  let info = $state<WifiInfo | null>(null);
  let networks = $state<ScannedNet[]>([]);
  let scanning = $state(false);
  let saving = $state(false);
  let showPass = $state(false);

  let ssid = $state('');
  let password = $state('');
  let hostname = $state('');
  let printerName = $state('');

  async function loadInfo() {
    try {
      const r = await fetch('/api/wifi', { cache: 'no-store' });
      if (!r.ok) return;
      const d = (await r.json()) as WifiInfo;
      info = d;
      if (!ssid) ssid = d.ssid || '';
      if (!hostname) hostname = d.hostname || 'airprint';
      if (!printerName) printerName = d.printer_name || '';
    } catch {
      /* offline */
    }
  }

  async function scan() {
    scanning = true;
    try {
      const r = await fetch('/api/wifi/scan', { cache: 'no-store' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const d = (await r.json()) as { networks?: ScannedNet[] };
      networks = (d.networks || []).sort((a, b) => b.rssi - a.rssi);
    } catch (e) {
      toast(`Scan failed: ${(e as Error).message}`, 'error');
    } finally {
      scanning = false;
    }
  }

  async function save(e: Event) {
    e.preventDefault();
    if (!ssid.trim()) {
      toast('Enter a WiFi network name (SSID)', 'error');
      return;
    }
    saving = true;
    try {
      const r = await fetch('/api/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ssid: ssid.trim(),
          password,
          hostname: hostname.trim(),
          printer_name: printerName.trim(),
        }),
      });
      const d = (await r.json()) as { ok?: boolean; rebooting?: boolean; error?: string };
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
      toast(
        d.rebooting
          ? `Saved! Rebooting and connecting to "${ssid.trim()}"…`
          : 'WiFi settings saved.',
        'ok',
        [],
        9000,
      );
    } catch (err) {
      toast(`Save failed: ${(err as Error).message}`, 'error');
    } finally {
      saving = false;
    }
  }

  async function resetWifi() {
    saving = true;
    try {
      const r = await fetch('/api/wifi/reset', { method: 'POST' });
      const d = (await r.json()) as { ok?: boolean; rebooting?: boolean };
      if (!r.ok || !d.ok) throw new Error(`HTTP ${r.status}`);
      toast('Cleared saved WiFi credentials. Rebooting into setup AP…', 'ok', [], 9000);
    } catch (err) {
      toast(`Reset failed: ${(err as Error).message}`, 'error');
    } finally {
      saving = false;
    }
  }

  onMount(() => {
    loadInfo();
  });
</script>

<div class="grid">
  <section class="card card-pad status-card">
    <div class="row wrap">
      <div class="art"><Wifi size={28} /></div>
      <div class="grow">
        <h1>WiFi &amp; Device Setup</h1>
        <p class="muted small">
          {#if info?.connected}
            Connected to <b>{info.ssid}</b> · IP <code>{info.ip}</code> ({info.rssi} dBm)
          {:else if info?.ap_active}
            Setup AP active: <b>{info.ap_ssid}</b> at <code>{info.ap_ip}</code>
          {:else}
            Loading network status…
          {/if}
        </p>
      </div>
      {#if info}
        <span class="pill {info.connected ? 'ok' : info.ap_active ? 'warn' : ''}">
          <span class="dot"></span>
          {info.connected ? 'Connected' : info.ap_active ? 'Setup AP' : 'Connecting'}
        </span>
      {/if}
    </div>
    {#if info}
      <div class="meta">
        <div><span class="muted small">Hostname</span><br /><code>http://{info.hostname}.local/</code></div>
        <div><span class="muted small">Active Driver</span><br /><span>{info.model || info.driver}</span></div>
        <div><span class="muted small">Supported Models</span><br /><span>{info.driver_count} built-in drivers</span></div>
      </div>
    {/if}
  </section>

  <section class="card card-pad">
    <div class="row head">
      <h2 class="grow">Nearby Networks</h2>
      <button class="btn" onclick={scan} disabled={scanning}>
        <RefreshCw size={16} class={scanning ? 'spin' : ''} /> {scanning ? 'Scanning…' : 'Scan WiFi'}
      </button>
    </div>
    {#if networks.length}
      <ul class="nets">
        {#each networks as net (net.ssid)}
          <li>
            <button type="button" class:selected={ssid === net.ssid} onclick={() => (ssid = net.ssid)}>
              <span class="grow ellipsis">{net.ssid}</span>
              {#if net.secure}<Lock size={14} class="muted" />{/if}
              <span class="small muted">{net.rssi} dBm</span>
            </button>
          </li>
        {/each}
      </ul>
    {:else}
      <p class="muted small">Click "Scan WiFi" to pick a 2.4 GHz network, or type the SSID manually.</p>
    {/if}
  </section>

  <section class="card card-pad">
    <h2>Configure WiFi &amp; AirPrint</h2>
    <form class="form" onsubmit={save}>
      <label class="field">
        <span class="label">WiFi Network (2.4 GHz SSID)</span>
        <input class="input" bind:value={ssid} placeholder="Your WiFi SSID" maxlength="32" required />
      </label>
      <label class="field">
        <span class="label">WiFi Password</span>
        <div class="row">
          <input
            class="input grow"
            type={showPass ? 'text' : 'password'}
            bind:value={password}
            placeholder="Leave empty for open network"
            maxlength="64"
          />
          <button type="button" class="icon-btn" onclick={() => (showPass = !showPass)} aria-label="Toggle password">
            {#if showPass}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
          </button>
        </div>
      </label>
      <label class="field">
        <span class="label">Hostname (.local)</span>
        <input class="input" bind:value={hostname} placeholder="canon-mg2500" maxlength="32" />
      </label>
      <label class="field">
        <span class="label">AirPrint Display Name</span>
        <input class="input" bind:value={printerName} placeholder="AirPrint Server (ESP32)" maxlength="64" />
      </label>
      <div class="row wrap actions">
        <button class="btn primary" type="submit" disabled={saving}>
          <Check size={17} /> {saving ? 'Saving…' : 'Save & Reboot'}
        </button>
        <button class="btn ghost" type="button" onclick={resetWifi} disabled={saving}>
          <RotateCcw size={16} /> Reset WiFi to Setup AP
        </button>
      </div>
    </form>
  </section>
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 18px;
  }
  .status-card {
    grid-column: 1 / -1;
    display: grid;
    gap: 16px;
  }
  .art {
    display: grid;
    place-items: center;
    width: 56px;
    height: 56px;
    border-radius: 16px;
    background: var(--accent-soft);
    color: var(--accent);
    flex: none;
  }
  .meta {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
    gap: 12px;
    padding-top: 12px;
    border-top: 1px solid var(--border);
  }
  .head {
    margin-bottom: 12px;
  }
  h2 {
    margin-bottom: 12px;
  }
  .head h2 {
    margin-bottom: 0;
  }
  .nets {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 6px;
    max-height: 280px;
    overflow: auto;
  }
  .nets button {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--surface-2);
    color: var(--text);
    cursor: pointer;
    text-align: left;
  }
  .nets button:hover {
    border-color: var(--accent);
  }
  .nets button.selected {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .form {
    display: grid;
    gap: 14px;
  }
  .actions {
    margin-top: 4px;
  }
  @media (max-width: 900px) {
    .grid {
      grid-template-columns: 1fr;
    }
  }
</style>
