<script lang="ts">
  import { onMount } from 'svelte';
  import Wifi from '@lucide/svelte/icons/wifi';
  import RefreshCw from '@lucide/svelte/icons/refresh-cw';
  import Lock from '@lucide/svelte/icons/lock';
  import Eye from '@lucide/svelte/icons/eye';
  import EyeOff from '@lucide/svelte/icons/eye-off';
  import RotateCcw from '@lucide/svelte/icons/rotate-ccw';
  import Check from '@lucide/svelte/icons/check';
  import Shield from '@lucide/svelte/icons/shield';
  import Upload from '@lucide/svelte/icons/upload';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import { toast } from '../lib/ui.svelte';
  import { i18n } from '../lib/i18n.svelte';

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
    use_static_ip?: boolean;
    static_ip?: string;
    netmask?: string;
    gateway?: string;
    dns?: string;
    has_password?: boolean;
    admin_protected?: boolean;
    ap_has_password?: boolean;
    ap_custom_ssid?: string;
    fw_version?: string;
    fw_date?: string;
    fw_partition?: string;
    ota_state?: string;
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
  let initialized = false;

  let ssid = $state('');
  let password = $state('');
  let clearWifiPass = $state(false);
  let hostname = $state('');
  let printerName = $state('');
  let useStaticIp = $state(false);
  let staticIp = $state('');
  let netmask = $state('255.255.255.0');
  let gateway = $state('');
  let dns = $state('');

  const keepingSavedPass = $derived(Boolean(info?.has_password && ssid.trim() === info.ssid));

  // Admin password for protected endpoints
  let adminPass = $state('');
  let showAdminPass = $state(false);

  // Security & Setup AP settings
  let apSsid = $state('');
  let apPass = $state('');
  let showApPass = $state(false);
  let clearApPass = $state(false);
  let newAdminPass = $state('');
  let showNewAdmin = $state(false);
  let clearAdminPass = $state(false);
  let savingSec = $state(false);

  // Web OTA
  let otaFile = $state<File | null>(null);
  let otaInput: HTMLInputElement;
  let otaUploading = $state(false);
  let otaPhase = $state<'idle' | 'uploading' | 'verifying'>('idle');
  let otaPct = $state(0);

  function adminHeaders(): Record<string, string> {
    return adminPass ? { 'X-Admin-Password': adminPass } : {};
  }

  function requireAdminInput(): boolean {
    if (info?.admin_protected && !adminPass) {
      toast(i18n.t.wifi.errAdminRequired, 'error');
      return false;
    }
    return true;
  }

  function isValidIpv4(v: string): boolean {
    const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(v.trim());
    if (!m) return false;
    const octets = m.slice(1).map((o) => (o.length > 1 && o.startsWith('0') ? -1 : Number(o)));
    if (octets.some((n) => n < 0 || n > 255)) return false;
    return v.trim() !== '0.0.0.0';
  }

  async function loadInfo() {
    try {
      const r = await fetch('/api/wifi', { cache: 'no-store' });
      if (!r.ok) return;
      const d = (await r.json()) as WifiInfo;
      info = d;
      if (!initialized) {
        initialized = true;
        if (!ssid) ssid = d.ssid || '';
        if (!hostname) hostname = d.hostname || 'airprint';
        if (!printerName) printerName = d.printer_name || '';
        useStaticIp = Boolean(d.use_static_ip);
        staticIp = d.static_ip || '';
        netmask = d.netmask || '255.255.255.0';
        gateway = d.gateway || '';
        dns = d.dns || '';
        apSsid = d.ap_custom_ssid || '';
      }
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
      toast(i18n.t.wifi.scanFailed((e as Error).message), 'error');
    } finally {
      scanning = false;
    }
  }

  async function save(e: Event) {
    e.preventDefault();
    if (!requireAdminInput()) return;
    if (!ssid.trim()) {
      toast(i18n.t.wifi.errSsid, 'error');
      return;
    }
    if (useStaticIp) {
      if (!isValidIpv4(staticIp)) {
        toast(i18n.t.wifi.errStaticIp, 'error');
        return;
      }
      if (!isValidIpv4(netmask)) {
        toast(i18n.t.wifi.errMask, 'error');
        return;
      }
      if (!isValidIpv4(gateway)) {
        toast(i18n.t.wifi.errGw, 'error');
        return;
      }
      if (dns.trim() && !isValidIpv4(dns)) {
        toast(i18n.t.wifi.errDns, 'error');
        return;
      }
    }
    saving = true;
    try {
      const r = await fetch('/api/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...adminHeaders() },
        body: JSON.stringify({
          ssid: ssid.trim(),
          password: clearWifiPass ? '' : password,
          clear_password: clearWifiPass,
          hostname: hostname.trim(),
          printer_name: printerName.trim(),
          use_static_ip: useStaticIp,
          static_ip: useStaticIp ? staticIp.trim() : '',
          netmask: useStaticIp ? netmask.trim() : '',
          gateway: useStaticIp ? gateway.trim() : '',
          dns: useStaticIp ? dns.trim() : '',
        }),
      });
      const d = (await r.json()) as { ok?: boolean; rebooting?: boolean; error?: string };
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
      const ipNote = useStaticIp ? ` (http://${staticIp.trim()}/)` : '';
      toast(
        d.rebooting ? i18n.t.wifi.savedRebootToast(ssid.trim(), ipNote) : i18n.t.wifi.savedToast,
        'ok',
        [],
        9000,
      );
    } catch (err) {
      toast(i18n.t.wifi.saveFailed((err as Error).message), 'error');
    } finally {
      saving = false;
    }
  }

  async function resetWifi() {
    if (!requireAdminInput()) return;
    saving = true;
    try {
      const r = await fetch('/api/wifi/reset', {
        method: 'POST',
        headers: adminHeaders(),
      });
      const d = (await r.json()) as { ok?: boolean; rebooting?: boolean; error?: string };
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
      toast(i18n.t.wifi.resetToast, 'ok', [], 9000);
    } catch (err) {
      toast(i18n.t.wifi.resetFailed((err as Error).message), 'error');
    } finally {
      saving = false;
    }
  }

  async function saveSecurity(e: Event) {
    e.preventDefault();
    if (!requireAdminInput()) return;
    if (!clearApPass && apPass && (apPass.length < 8 || apPass.length > 63)) {
      toast(i18n.t.wifi.errApPassLen, 'error');
      return;
    }
    savingSec = true;
    try {
      const r = await fetch('/api/security', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...adminHeaders() },
        body: JSON.stringify({
          ap_ssid: apSsid.trim(),
          ap_password: clearApPass ? '' : apPass,
          clear_ap_password: clearApPass,
          new_admin_password: clearAdminPass ? '' : newAdminPass,
          clear_admin_password: clearAdminPass,
        }),
      });
      const d = (await r.json()) as { ok?: boolean; rebooting?: boolean; error?: string };
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
      if (clearAdminPass) adminPass = '';
      else if (newAdminPass) adminPass = newAdminPass;
      apPass = '';
      newAdminPass = '';
      clearApPass = false;
      clearAdminPass = false;
      toast(i18n.t.wifi.secSavedToast(Boolean(d.rebooting)), 'ok', [], 7000);
      await loadInfo();
    } catch (err) {
      toast(i18n.t.wifi.saveFailed((err as Error).message), 'error');
    } finally {
      savingSec = false;
    }
  }

  function onPickOta(e: Event) {
    const el = e.target as HTMLInputElement;
    otaFile = el.files?.[0] ?? null;
    el.value = '';
  }

  function uploadOta() {
    if (!requireAdminInput()) return;
    if (!otaFile) {
      toast(i18n.t.wifi.otaPickErr, 'error');
      return;
    }
    const file = otaFile;
    otaUploading = true;
    otaPhase = 'uploading';
    otaPct = 0;

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    if (adminPass) xhr.setRequestHeader('X-Admin-Password', adminPass);

    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable && ev.total > 0) {
        otaPct = Math.min(100, Math.round((ev.loaded / ev.total) * 100));
        if (otaPct >= 100) otaPhase = 'verifying';
      }
    };
    xhr.upload.onload = () => {
      otaPct = 100;
      otaPhase = 'verifying';
    };
    xhr.onload = () => {
      otaUploading = false;
      otaPhase = 'idle';
      let msg = `HTTP ${xhr.status}`;
      try {
        const d = JSON.parse(xhr.responseText || '{}') as { ok?: boolean; error?: string };
        if (xhr.status >= 200 && xhr.status < 300 && d.ok) {
          otaFile = null;
          toast(i18n.t.wifi.otaSuccess, 'ok', [], 10000);
          return;
        }
        if (d.error) msg = d.error;
      } catch {
        /* ignore */
      }
      toast(i18n.t.wifi.otaFailed(msg), 'error', [], 9000);
    };
    xhr.onerror = () => {
      otaUploading = false;
      otaPhase = 'idle';
      toast(i18n.t.wifi.otaFailed('network error'), 'error');
    };
    xhr.send(file);
  }

  const looksLikeFactory = $derived(
    otaFile ? /factory|universal/i.test(otaFile.name) : false,
  );

  onMount(() => {
    loadInfo();
  });
</script>

<div class="grid">
  <section class="card card-pad status-card">
    <div class="row wrap">
      <div class="art"><Wifi size={28} /></div>
      <div class="grow">
        <h1>{i18n.t.wifi.title}</h1>
        <p class="muted small">
          {#if info?.connected}
            {i18n.t.wifi.connectedLine(info.ssid, info.ip, info.use_static_ip ? 'Static IP' : 'DHCP', info.rssi)}
          {:else if info?.ap_active}
            {i18n.t.wifi.apLine(info.ap_ssid, info.ap_ip)}
          {:else}
            {i18n.t.wifi.loading}
          {/if}
        </p>
      </div>
      {#if info}
        <span class="pill {info.connected ? 'ok' : info.ap_active ? 'warn' : ''}">
          <span class="dot"></span>
          {info.connected ? i18n.t.wifi.pillConnected : info.ap_active ? i18n.t.wifi.pillAp : i18n.t.wifi.pillConnecting}
        </span>
      {/if}
    </div>
    {#if info}
      <div class="meta">
        <div><span class="muted small">{i18n.t.wifi.hostnameLabel}</span><br /><code>http://{info.hostname}.local/</code></div>
        <div><span class="muted small">{i18n.t.wifi.driverLabel}</span><br /><span>{info.model || info.driver}</span></div>
        <div><span class="muted small">{i18n.t.wifi.modelsLabel}</span><br /><span>{i18n.t.wifi.builtInDrivers(info.driver_count)}</span></div>
      </div>
    {/if}
  </section>

  {#if info?.admin_protected}
    <section class="card card-pad full admin-bar">
      <div class="row wrap">
        <Lock size={18} class="accent-ic" />
        <div class="grow">
          <strong>{i18n.t.wifi.adminBannerTitle}</strong>
          <div class="muted small">{i18n.t.wifi.adminBannerHint}</div>
        </div>
        <div class="row admin-inp-wrap">
          <input
            class="input"
            type={showAdminPass ? 'text' : 'password'}
            bind:value={adminPass}
            placeholder={i18n.t.wifi.adminCurrentPlaceholder}
            maxlength="64"
          />
          <button type="button" class="icon-btn" onclick={() => (showAdminPass = !showAdminPass)} aria-label="Toggle admin password">
            {#if showAdminPass}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
          </button>
        </div>
      </div>
    </section>
  {/if}

  <section class="card card-pad">
    <div class="row head">
      <h2 class="grow">{i18n.t.wifi.nearbyTitle}</h2>
      <button class="btn" onclick={scan} disabled={scanning}>
        <RefreshCw size={16} class={scanning ? 'spin' : ''} /> {scanning ? i18n.t.wifi.scanning : i18n.t.wifi.scanBtn}
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
      <p class="muted small">{i18n.t.wifi.scanHint}</p>
    {/if}
  </section>

  <section class="card card-pad">
    <h2>{i18n.t.wifi.configTitle}</h2>
    <form class="form" onsubmit={save}>
      <label class="field">
        <span class="label">{i18n.t.wifi.ssidLabel}</span>
        <input class="input" bind:value={ssid} placeholder={i18n.t.wifi.ssidPlaceholder} maxlength="32" required />
      </label>
      <label class="field">
        <span class="label">{i18n.t.wifi.passLabel}</span>
        <div class="row">
          <input
            class="input grow"
            type={showPass ? 'text' : 'password'}
            bind:value={password}
            placeholder={keepingSavedPass ? i18n.t.wifi.passKeepPlaceholder : i18n.t.wifi.passPlaceholder}
            maxlength="64"
            disabled={clearWifiPass}
          />
          <button type="button" class="icon-btn" onclick={() => (showPass = !showPass)} aria-label="Toggle password">
            {#if showPass}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
          </button>
        </div>
      </label>
      {#if keepingSavedPass}
        <label class="checkbox">
          <input type="checkbox" bind:checked={clearWifiPass} />
          <span>{i18n.t.wifi.clearWifiPass}</span>
        </label>
      {/if}
      <label class="field">
        <span class="label">{i18n.t.wifi.hostField}</span>
        <input class="input" bind:value={hostname} placeholder="canon-mg2500" maxlength="32" />
      </label>
      <label class="field">
        <span class="label">{i18n.t.wifi.printerNameField}</span>
        <input class="input" bind:value={printerName} placeholder="AirPrint Server (ESP32)" maxlength="64" />
      </label>
      <label class="checkbox">
        <input
          type="checkbox"
          bind:checked={useStaticIp}
          onchange={() => {
            if (useStaticIp && !gateway && info?.connected && isValidIpv4(info.ip)) {
              const parts = info.ip.trim().split('.');
              gateway = `${parts[0]}.${parts[1]}.${parts[2]}.1`;
            }
          }}
        />
        <span>{i18n.t.wifi.useStaticIp}</span>
      </label>
      {#if useStaticIp}
        <div class="ip-grid">
          {#if info?.connected && isValidIpv4(info.ip)}
            <p class="small muted ip-hint">{i18n.t.wifi.currentSubnetHint(info.ip)}</p>
          {/if}
          <label class="field">
            <span class="label">{i18n.t.wifi.staticIp}</span>
            <input class="input mono" bind:value={staticIp} placeholder="192.168.1.200" maxlength="15" required />
          </label>
          <label class="field">
            <span class="label">{i18n.t.wifi.netmask}</span>
            <input class="input mono" bind:value={netmask} placeholder="255.255.255.0" maxlength="15" required />
          </label>
          <label class="field">
            <span class="label">{i18n.t.wifi.gateway}</span>
            <input class="input mono" bind:value={gateway} placeholder="192.168.1.1" maxlength="15" required />
          </label>
          <label class="field">
            <span class="label">{i18n.t.wifi.dns}</span>
            <input class="input mono" bind:value={dns} placeholder="192.168.1.1" maxlength="15" />
          </label>
        </div>
      {/if}
      <div class="row wrap actions">
        <button class="btn primary" type="submit" disabled={saving}>
          <Check size={17} /> {saving ? i18n.t.wifi.saving : i18n.t.wifi.saveReboot}
        </button>
        <button class="btn ghost" type="button" onclick={resetWifi} disabled={saving}>
          <RotateCcw size={16} /> {i18n.t.wifi.resetWifi}
        </button>
      </div>
    </form>
  </section>

  <section class="card card-pad">
    <div class="row head">
      <Shield size={18} class="accent-ic" />
      <h2 class="grow">{i18n.t.wifi.secTitle}</h2>
    </div>
    <p class="muted small sec-desc">{i18n.t.wifi.secDesc}</p>
    <form class="form" onsubmit={saveSecurity}>
      <label class="field">
        <span class="label">{i18n.t.wifi.apSsidLabel}</span>
        <input class="input" bind:value={apSsid} placeholder={i18n.t.wifi.apSsidPlaceholder} maxlength="32" />
      </label>
      <label class="field">
        <span class="label">{i18n.t.wifi.apPassLabel}</span>
        <div class="row">
          <input
            class="input grow"
            type={showApPass ? 'text' : 'password'}
            bind:value={apPass}
            placeholder={info?.ap_has_password ? i18n.t.wifi.apPassSet : i18n.t.wifi.apPassOpen}
            maxlength="63"
            disabled={clearApPass}
          />
          <button type="button" class="icon-btn" onclick={() => (showApPass = !showApPass)} aria-label="Toggle AP password">
            {#if showApPass}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
          </button>
        </div>
      </label>
      {#if info?.ap_has_password}
        <label class="checkbox">
          <input type="checkbox" bind:checked={clearApPass} />
          <span>{i18n.t.wifi.clearApPass}</span>
        </label>
      {/if}
      <label class="field">
        <span class="label">{i18n.t.wifi.newAdminLabel}</span>
        <div class="row">
          <input
            class="input grow"
            type={showNewAdmin ? 'text' : 'password'}
            bind:value={newAdminPass}
            placeholder={info?.admin_protected ? i18n.t.wifi.adminOn : i18n.t.wifi.adminOff}
            maxlength="64"
            disabled={clearAdminPass}
          />
          <button type="button" class="icon-btn" onclick={() => (showNewAdmin = !showNewAdmin)} aria-label="Toggle new admin password">
            {#if showNewAdmin}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
          </button>
        </div>
      </label>
      {#if info?.admin_protected}
        <label class="checkbox">
          <input type="checkbox" bind:checked={clearAdminPass} />
          <span>{i18n.t.wifi.clearAdminPass}</span>
        </label>
      {/if}
      <div class="row wrap actions">
        <button class="btn primary" type="submit" disabled={savingSec}>
          <Check size={17} /> {savingSec ? i18n.t.wifi.saving : i18n.t.wifi.saveSecBtn}
        </button>
      </div>
      <p class="muted small">{i18n.t.wifi.resetPinNote}</p>
    </form>
  </section>

  <section class="card card-pad">
    <div class="row head">
      <Upload size={18} class="accent-ic" />
      <h2 class="grow">{i18n.t.wifi.otaTitle}</h2>
    </div>
    <div class="form">
      {#if info?.fw_version}
        <div class="small muted">
          {i18n.t.wifi.otaCurrent(info.fw_version, info.fw_partition || '', info.ota_state || '')}
          {#if info.fw_date} · {info.fw_date}{/if}
        </div>
      {/if}
      <p class="muted small" style="margin:0">{i18n.t.wifi.otaDesc}</p>
      <p class="small warn-note" style="margin:0">{i18n.t.wifi.otaWarn}</p>
      <div class="row wrap">
        <button type="button" class="btn" onclick={() => otaInput.click()} disabled={otaUploading}>
          <Upload size={16} /> {i18n.t.wifi.otaPick}
        </button>
        {#if otaFile}
          <span class="small mono grow ellipsis">{otaFile.name} ({(otaFile.size / 1024).toFixed(0)} KB)</span>
        {/if}
        <input bind:this={otaInput} type="file" accept=".bin" hidden onchange={onPickOta} />
      </div>
      {#if looksLikeFactory}
        <p class="err small" style="margin:0">{i18n.t.wifi.otaFactoryHint}</p>
      {/if}
      {#if otaUploading}
        <div class="ota-prog">
          <div class="row small">
            <LoaderCircle size={15} class="spin" />
            <span>{otaPhase === 'verifying' ? i18n.t.wifi.otaVerifying : i18n.t.wifi.otaUploading(otaPct)}</span>
          </div>
          <div class="bar"><div style:width={`${Math.max(3, otaPct)}%`}></div></div>
        </div>
      {/if}
      <div class="row wrap actions">
        <button type="button" class="btn primary" onclick={uploadOta} disabled={!otaFile || otaUploading}>
          {#if otaUploading}<LoaderCircle size={17} class="spin" />{:else}<Upload size={17} />{/if}
          {i18n.t.wifi.otaStart}
        </button>
      </div>
    </div>
  </section>
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 18px;
  }
  .status-card,
  .full {
    grid-column: 1 / -1;
  }
  .status-card {
    display: grid;
    gap: 16px;
  }
  .admin-bar {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .admin-inp-wrap {
    min-width: 240px;
  }
  :global(.accent-ic) {
    color: var(--accent);
    flex: none;
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
  .sec-desc {
    margin: -4px 0 12px;
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
  .checkbox {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 13px;
    cursor: pointer;
  }
  .ip-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
    padding: 12px;
    border: 1px solid var(--border);
    border-radius: 12px;
    background: var(--surface-2);
  }
  .ip-hint {
    grid-column: 1 / -1;
    margin: 0;
  }
  .mono {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .warn-note {
    color: var(--warn);
  }
  .ota-prog {
    display: grid;
    gap: 6px;
  }
  .actions {
    margin-top: 4px;
  }
  @media (max-width: 900px) {
    .grid,
    .ip-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
