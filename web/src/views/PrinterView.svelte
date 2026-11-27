<script lang="ts">
  import Printer from '@lucide/svelte/icons/printer';
  import Upload from '@lucide/svelte/icons/upload';
  import FolderOpen from '@lucide/svelte/icons/folder-open';
  import Lightbulb from '@lucide/svelte/icons/lightbulb';
  import CircleStop from '@lucide/svelte/icons/circle-stop';
  import CircleAlert from '@lucide/svelte/icons/circle-alert';
  import { device, type Job, type Supply } from '../lib/device.svelte';
  import { cancelJob, identifyPrinter, statusOk, statusText } from '../lib/ipp';
  import { addFile, formatSize, isSupported } from '../lib/files.svelte';
  import { openPrint, pickFiles, toast } from '../lib/ui.svelte';
  import { i18n } from '../lib/i18n.svelte';

  const p = $derived(device.printer);
  const active = $derived(p?.jobs.find((j) => j.id === p.active && (j.state === 'processing' || j.state === 'pending')));
  let input: HTMLInputElement;

  const status = $derived.by(() => {
    const t = i18n.t.printer;
    if (!device.loaded) return { text: t.connecting, tone: '' };
    if (!p) return { text: t.unavailable, tone: 'err' };
    if (!p.connected) return { text: t.notConnected, tone: 'warn' };
    if (p.reasons.some((r) => r.endsWith('-error'))) return { text: p.message, tone: 'err' };
    if (p.state === 'processing') return { text: t.printing, tone: 'busy' };
    if (device.scan?.state === 'scanning') return { text: t.scanning, tone: 'busy' };
    return { text: t.ready, tone: 'ok' };
  });

  function reasonText(r: string): string {
    return i18n.t.printer.reasons[r] || r.replace(/-(error|warning|report)$/, '').replace(/-/g, ' ');
  }

  function inkColor(s: Supply): string {
    const cs = s.color.match(/#[0-9a-f]{6}/gi) || ['#888'];
    return cs.length > 1 ? `linear-gradient(90deg, ${cs.join(', ')})` : cs[0];
  }

  const STATE_TONE: Record<Job['state'], string> = {
    pending: 'busy',
    held: 'warn',
    processing: 'busy',
    completed: 'ok',
    canceled: '',
    aborted: 'err',
  };

  // the device's clock is set over the network; without it, no time
  function ago(t: number): string {
    if (t < 1.6e9) return '';
    const s = Math.max(0, Date.now() / 1000 - t);
    if (s < 60) return i18n.t.printer.justNow;
    if (s < 3600) return i18n.t.printer.minAgo(Math.round(s / 60));
    if (s < 86400) return i18n.t.printer.hAgo(Math.round(s / 3600));
    return i18n.t.printer.dAgo(Math.round(s / 86400));
  }

  async function chooseFile() {
    const [f] = await pickFiles(i18n.t.printer.chooseToPrint, false);
    if (f) openPrint(f);
  }

  async function upload(e: Event) {
    const list = (e.target as HTMLInputElement).files;
    const file = list?.[0];
    (e.target as HTMLInputElement).value = '';
    if (!file) return;
    if (!isSupported(file.type, file.name)) return toast(i18n.t.printer.pdfOrImageOnly, 'error');
    try {
      const f = await addFile({ name: file.name, blob: file, source: 'upload' });
      openPrint(f);
    } catch (err) {
      toast(i18n.t.printer.cannotOpen(file.name, (err as Error).message), 'error');
    }
  }

  async function identify() {
    try {
      await identifyPrinter();
      toast(i18n.t.printer.identifyToast, 'ok');
    } catch (e) {
      toast(i18n.t.printer.failed((e as Error).message), 'error');
    }
  }

  async function cancel(id: number) {
    try {
      const r = await cancelJob(id);
      if (!statusOk(r.status)) throw new Error(statusText(r));
      toast(i18n.t.printer.cancelingToast);
    } catch (e) {
      toast(i18n.t.printer.cannotCancel((e as Error).message), 'error');
    }
  }
</script>

<div class="grid">
  <section class="card hero">
    <div class="art"><Printer size={40} strokeWidth={1.5} /></div>
    <div class="grow">
      <div class="row wrap">
        <h1 class="ellipsis">{p?.name || i18n.t.routes.printer}</h1>
        <span class="pill {status.tone}"><span class="dot"></span>{status.text}</span>
      </div>
      <p class="muted">{p?.model || ''}{p?.connected && p.message && p.message !== status.text ? ` · ${p.message}` : ''}</p>
      <div class="actions row wrap">
        <button class="btn primary" onclick={() => input.click()} disabled={!p?.connected}>
          <Upload size={17} /> {i18n.t.printer.uploadPrint}
        </button>
        <button class="btn" onclick={chooseFile} disabled={!p?.connected}><FolderOpen size={17} /> {i18n.t.printer.printFromFiles}</button>
        <button class="btn ghost" onclick={identify} disabled={!device.online}><Lightbulb size={17} /> {i18n.t.printer.identify}</button>
      </div>
      <input bind:this={input} type="file" accept="application/pdf,image/*" hidden onchange={upload} />
    </div>
  </section>

  {#if p && p.reasons.length}
    <section class="card card-pad alerts">
      {#each p.reasons as r}
        <div class="row">
          <CircleAlert size={18} class={r.endsWith('-error') ? 'err' : 'warn'} />
          <span>{reasonText(r)}</span>
        </div>
      {/each}
    </section>
  {/if}

  {#if active}
    <section class="card card-pad job">
      <div class="row">
        <div class="grow">
          <h2 class="ellipsis">{active.name}</h2>
          <p class="muted small">
            {active.state === 'pending' ? i18n.t.printer.waitingDoc : i18n.t.printer.pageNum(active.pages + 1)} · {formatSize(active.bytes)}
            {i18n.t.printer.received}{active.user ? ` · ${i18n.t.printer.fromUser(active.user)}` : ''}
          </p>
        </div>
        <button class="btn danger" onclick={() => cancel(active.id)}><CircleStop size={17} /> {i18n.t.printer.cancel}</button>
      </div>
      <div class="bar indeterminate"><div></div></div>
    </section>
  {/if}

  <section class="card card-pad">
    <h2>{i18n.t.printer.ink}</h2>
    {#if p?.supplies.length}
      <div class="inks">
        {#each p.supplies as s}
          <div class="ink">
            <div class="row"><span class="grow">{s.name}</span><b>{s.level >= 0 ? s.level + '%' : i18n.t.printer.unknown}</b></div>
            <div class="bar">
              <div style:width={(s.level >= 0 ? s.level : 0) + '%'} style:background={inkColor(s)}></div>
            </div>
          </div>
        {/each}
      </div>
      <p class="muted small">{i18n.t.printer.refilledHint}</p>
    {:else}
      <p class="muted">{p?.connected ? i18n.t.printer.noInkReported : i18n.t.printer.noPrinterConnected}</p>
    {/if}
  </section>

  <section class="card card-pad">
    <h2>{i18n.t.printer.recentJobs}</h2>
    {#if p?.jobs.length}
      <ul class="jobs">
        {#each p.jobs as j (j.id)}
          {@const label = i18n.t.printer.jobStates[j.state] || j.state}
          {@const tone = STATE_TONE[j.state] || ''}
          <li class="row">
            <div class="grow">
              <div class="ellipsis">{j.name}</div>
              <div class="muted small">
                {[
                  i18n.t.printer.pagesCount(j.pages),
                  j.state === 'aborted' && j.reason && j.reason !== 'aborted-by-system' ? reasonText(j.reason) : '',
                  ago(j.time),
                  j.user,
                ]
                  .filter(Boolean)
                  .join(' · ')}
              </div>
            </div>
            <span class="pill {tone}">{label}</span>
          </li>
        {/each}
      </ul>
    {:else}
      <p class="muted">{i18n.t.printer.noJobs}</p>
    {/if}
  </section>

  <section class="card card-pad tips">
    <h2>{i18n.t.printer.otherWaysTitle}</h2>
    <p class="muted small">
      {i18n.t.printer.otherWaysDesc}
    </p>
  </section>
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 18px;
  }
  .hero,
  .alerts,
  .job {
    grid-column: 1 / -1;
  }
  .hero {
    display: flex;
    gap: 22px;
    padding: 24px;
    align-items: center;
  }
  .art {
    display: grid;
    place-items: center;
    width: 92px;
    height: 92px;
    border-radius: 24px;
    background: var(--accent-soft);
    color: var(--accent);
    flex: none;
  }
  .actions {
    margin-top: 16px;
  }
  h2 {
    margin-bottom: 12px;
  }
  .alerts {
    display: grid;
    gap: 6px;
  }
  .alerts :global(.warn) {
    color: var(--warn);
  }
  .alerts :global(.err) {
    color: var(--err);
  }
  .job h2 {
    margin: 0;
  }
  .job .bar {
    margin-top: 12px;
  }
  .indeterminate > div {
    width: 35%;
    animation: slide 1.4s ease-in-out infinite;
  }
  @keyframes slide {
    from {
      transform: translateX(-100%);
    }
    to {
      transform: translateX(300%);
    }
  }
  .inks {
    display: grid;
    gap: 14px;
    margin-bottom: 12px;
  }
  .ink .bar {
    height: 10px;
    margin-top: 6px;
    border-radius: 5px;
  }
  .ink .bar > div {
    border-radius: 5px;
  }
  .jobs {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
  }
  .jobs li {
    padding: 9px 0;
    border-top: 1px solid var(--border);
  }
  .jobs li:first-child {
    border-top: 0;
    padding-top: 0;
  }
  .tips {
    grid-column: 1 / -1;
  }
  .tips h2 {
    margin-bottom: 6px;
  }
  @media (max-width: 900px) {
    .grid {
      grid-template-columns: 1fr;
    }
    .hero {
      flex-direction: column;
      align-items: flex-start;
      gap: 14px;
      padding: 18px;
    }
    .art {
      width: 60px;
      height: 60px;
      border-radius: 16px;
    }
  }
</style>
