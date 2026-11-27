<script lang="ts">
  import { onDestroy } from 'svelte';
  import Printer from '@lucide/svelte/icons/printer';
  import ChevronLeft from '@lucide/svelte/icons/chevron-left';
  import ChevronRight from '@lucide/svelte/icons/chevron-right';
  import Minus from '@lucide/svelte/icons/minus';
  import Plus from '@lucide/svelte/icons/plus';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import Modal from './Modal.svelte';
  import { device, inkSummary } from '../lib/device.svelte';
  import { fileSource, type StoredFile } from '../lib/files.svelte';
  import {
    defaultPaper,
    MARGINS,
    PAPERS,
    parseRanges,
    place,
    type Fit,
    type Orientation,
    type PaperId,
  } from '../lib/layout';
  import { renderJob, sendJob, type PrintOptions } from '../lib/print';
  import type { Sides } from '../lib/urf';
  import { whiteCanvas, type Source } from '../lib/source';
  import { toast } from '../lib/ui.svelte';
  import { i18n } from '../lib/i18n.svelte';

  let { file, onclose }: { file: StoredFile; onclose: () => void } = $props();

  const saved = (() => {
    try {
      return JSON.parse(localStorage.getItem('printSettings.v2') || '{}');
    } catch {
      return {};
    }
  })();

  let src = $state.raw<Source | null>(null);
  let loadError = $state('');
  let copies = $state(1);
  let range = $state('');
  let paper = $state<PaperId>(saved.paper || defaultPaper());
  let orientation = $state<Orientation>('auto');
  // a scan prints at its real size (a copy); anything else fits the page
  // svelte-ignore state_referenced_locally
  let fit = $state<Fit>(file.source === 'scan' && file.dpi ? 'actual' : 'fit'); // a scan: a 1:1 copy
  let gray = $state<boolean>(saved.gray ?? false);
  let dpi = $state<300 | 600>(saved.dpi || 600);
  let sides = $state<Sides>('one-sided');
  let pageIdx = $state(0);
  let previewEl: HTMLDivElement;

  type Phase = 'setup' | 'render' | 'send' | 'finish';
  let phase = $state<Phase>('setup');
  let frac = $state(0);
  let detail = $state('');
  let abort: AbortController | null = null;
  let closed = false;

  // svelte-ignore state_referenced_locally
  fileSource(file).then(
    (s) => (src = s),
    (e) => (loadError = (e as Error).message),
  );

  const count = $derived(src?.pages.length ?? file.pages ?? 1);
  const selected = $derived(parseRanges(range, count));
  const p = $derived(device.printer);
  const supportsColor = $derived(p?.color !== false);
  const supportsDuplex = $derived(p?.duplex === true);
  const supports300 = $derived(!p?.resolutions?.length || p.resolutions.includes(300));
  const supports600 = $derived(!p?.resolutions?.length || p.resolutions.includes(600));
  const effectiveGray = $derived(!supportsColor || gray);
  const effectiveSides = $derived<Sides>(supportsDuplex ? sides : 'one-sided');
  const effectiveDpi = $derived<300 | 600>(
    dpi === 300 && !supports300 && supports600 ? 600 : dpi === 600 && !supports600 && supports300 ? 300 : dpi,
  );
  const blocker = $derived.by(() => {
    const t = i18n.t.printDialog;
    if (!device.online) return t.blockerOffline;
    if (!p?.connected) return t.blockerNoPrinter;
    if (p.state === 'processing') return t.blockerBusy;
    if (device.scan?.state === 'scanning') return t.blockerScan;
    return '';
  });

  $effect(() => {
    try {
      localStorage.setItem('printSettings.v2', JSON.stringify({ paper, gray, dpi }));
    } catch {
      /* private mode */
    }
  });

  $effect(() => {
    if (pageIdx >= count) pageIdx = Math.max(0, count - 1);
  });

  // preview of the sheet
  let previewToken = 0;
  $effect(() => {
    const s = src;
    const opts = { paper, fit, orientation, pageIdx, gray: effectiveGray };
    if (!s || !previewEl) return;
    const token = ++previewToken;
    const P = PAPERS[opts.paper];
    const box = previewEl.getBoundingClientRect();
    const scale = Math.min((box.width || 300) / P.w, (box.height || 420) / P.h) * Math.min(2, devicePixelRatio || 1);
    const ps = s.pages[opts.pageIdx];
    const pl = place(ps.w, ps.h, opts.paper, opts.fit, opts.orientation);
    const c = whiteCanvas(P.w * scale, P.h * scale);
    s.draw(c, opts.pageIdx, pl.rot, pl.scale * scale, pl.x * scale, pl.y * scale).then(() => {
      if (token !== previewToken) return;
      if (opts.gray) {
        const ctx = c.getContext('2d')!;
        ctx.globalCompositeOperation = 'saturation';
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, c.width, c.height);
      }
      c.className = 'sheet';
      previewEl.querySelector('canvas')?.remove();
      previewEl.prepend(c);
    });
  });

  async function print() {
    if (!src || !selected?.length) return;
    const o: PrintOptions = {
      paper,
      fit,
      orientation,
      pages: selected,
      copies,
      gray: effectiveGray,
      dpi: effectiveDpi,
      sides: effectiveSides,
    };
    abort = new AbortController();
    const signal = abort.signal;
    const sheets = selected.length * copies;
    const name = file.name;
    try {
      phase = 'render';
      const doc = await renderJob(
        src,
        o,
        (done, total) => {
          frac = done / total;
          detail = i18n.t.printDialog.preparingPage(Math.min(total, Math.floor(done) + 1), total);
        },
        signal,
      );
      phase = 'send';
      frac = 0;
      detail = i18n.t.printDialog.sending;
      await sendJob(
        doc,
        name,
        (f) => {
          frac = f;
          if (f >= 1) {
            phase = 'finish';
            detail = i18n.t.printDialog.finishing;
          }
        },
        signal,
        effectiveSides,
      );
      toast(i18n.t.printDialog.printedToast(sheets, name), 'ok');
      if (!closed) onclose();
    } catch (e) {
      if ((e as Error).name === 'AbortError') {
        toast(i18n.t.printDialog.canceledToast);
      } else {
        toast(i18n.t.printDialog.failedToast((e as Error).message), 'error', [], 9000);
      }
      phase = 'setup';
    } finally {
      abort = null;
    }
  }

  function close() {
    // a job being sent goes on in the background
    closed = true;
    if (phase === 'render') abort?.abort();
    onclose();
  }

  onDestroy(() => {
    closed = true;
  });

  const marginStyle = $derived.by(() => {
    const P = PAPERS[paper];
    return `left:${(MARGINS.left / P.w) * 100}%;right:${(MARGINS.right / P.w) * 100}%;top:${(MARGINS.top / P.h) * 100}%;bottom:${(MARGINS.bottom / P.h) * 100}%`;
  });
</script>

<Modal title={i18n.t.printDialog.title(file.name)} onclose={close} wide>
  <div class="layout">
    <div class="left">
      <div class="preview" bind:this={previewEl} style:aspect-ratio={`${PAPERS[paper].w} / ${PAPERS[paper].h}`}>
        <div class="margins" style={marginStyle}></div>
        {#if !src && !loadError}<div class="loading"><LoaderCircle class="spin" /></div>{/if}
        {#if loadError}<div class="loading err">{loadError}</div>{/if}
      </div>
      {#if count > 1}
        <div class="pager row">
          <button class="icon-btn" onclick={() => pageIdx--} disabled={pageIdx === 0} aria-label={i18n.t.printDialog.previousPage}><ChevronLeft /></button>
          <span class="small muted">{i18n.t.printDialog.pageOf(pageIdx + 1, count)}</span>
          <button class="icon-btn" onclick={() => pageIdx++} disabled={pageIdx >= count - 1} aria-label={i18n.t.printDialog.nextPage}><ChevronRight /></button>
        </div>
      {/if}
      <p class="small muted center">{i18n.t.printDialog.marginsHint}</p>
    </div>

    <div class="opts">
      {#if phase !== 'setup'}
        <div class="progress card card-pad">
          <div class="row"><LoaderCircle size={18} class="spin" /><b>{detail}</b></div>
          <div class="bar"><div style:width={`${Math.max(2, frac * 100)}%`}></div></div>
          <p class="small muted">
            {#if phase === 'render'}
              {i18n.t.printDialog.renderHint}
            {:else}
              {i18n.t.printDialog.sendHint}
            {/if}
          </p>
          <button class="btn danger" onclick={() => abort?.abort()}>{i18n.t.printDialog.cancel}</button>
        </div>
      {/if}
      <fieldset disabled={phase !== 'setup'}>
        <div class="field">
          <span class="label">{i18n.t.printDialog.copies}</span>
          <div class="stepper">
            <button class="icon-btn" onclick={() => (copies = Math.max(1, copies - 1))} aria-label="Fewer"><Minus size={16} /></button>
            <input class="input" type="number" min="1" max="99" bind:value={copies} aria-label={i18n.t.printDialog.copies} />
            <button class="icon-btn" onclick={() => (copies = Math.min(99, copies + 1))} aria-label="More"><Plus size={16} /></button>
          </div>
        </div>
        {#if count > 1}
          <div class="field">
            <span class="label">{i18n.t.printDialog.pages}</span>
            <input class="input" placeholder={i18n.t.printDialog.allPagesPlaceholder(count)} bind:value={range} />
            {#if !selected}<span class="err small">{i18n.t.printDialog.pagesRangeErr(count)}</span>{/if}
          </div>
        {/if}
        <div class="field">
          <span class="label">{i18n.t.printDialog.paper}</span>
          <div class="seg">
            <button class:on={paper === 'a4'} onclick={() => (paper = 'a4')}>A4</button>
            <button class:on={paper === 'letter'} onclick={() => (paper = 'letter')}>Letter</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{i18n.t.printDialog.size}</span>
          <div class="seg">
            <button class:on={fit === 'fit'} onclick={() => (fit = 'fit')}>{i18n.t.printDialog.fit}</button>
            <button class:on={fit === 'fill'} onclick={() => (fit = 'fill')}>{i18n.t.printDialog.fill}</button>
            <button class:on={fit === 'actual'} onclick={() => (fit = 'actual')}>{i18n.t.printDialog.actual}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{i18n.t.printDialog.orientation}</span>
          <div class="seg">
            <button class:on={orientation === 'auto'} onclick={() => (orientation = 'auto')}>{i18n.t.printDialog.auto}</button>
            <button class:on={orientation === 'portrait'} onclick={() => (orientation = 'portrait')}>{i18n.t.printDialog.portrait}</button>
            <button class:on={orientation === 'landscape'} onclick={() => (orientation = 'landscape')}>{i18n.t.printDialog.landscape}</button>
          </div>
        </div>
        <div class="field">
          <span class="label">{i18n.t.printDialog.color}</span>
          {#if supportsColor}
            <div class="seg">
              <button class:on={!gray} onclick={() => (gray = false)}>{i18n.t.printDialog.colorBtn}</button>
              <button class:on={gray} onclick={() => (gray = true)}>{i18n.t.printDialog.bwBtn}</button>
            </div>
          {:else}
            <span class="small muted">{i18n.t.printDialog.monoOnly}</span>
          {/if}
        </div>
        <div class="field">
          <span class="label">{i18n.t.printDialog.quality}</span>
          <div class="seg">
            {#if supports600}
              <button class:on={effectiveDpi === 600} onclick={() => (dpi = 600)}>{i18n.t.printDialog.dpi600}</button>
            {/if}
            {#if supports300}
              <button class:on={effectiveDpi === 300} onclick={() => (dpi = 300)}>{i18n.t.printDialog.dpi300}</button>
            {/if}
          </div>
          {#if supports300 && supports600}
            <span class="small muted">{i18n.t.printDialog.dpiHint}</span>
          {/if}
        </div>
        {#if supportsDuplex}
          <div class="field">
            <span class="label">{i18n.t.printDialog.sides}</span>
            <div class="seg">
              <button class:on={sides === 'one-sided'} onclick={() => (sides = 'one-sided')}>{i18n.t.printDialog.oneSided}</button>
              <button class:on={sides === 'two-sided-long-edge'} onclick={() => (sides = 'two-sided-long-edge')}>{i18n.t.printDialog.twoLong}</button>
              <button class:on={sides === 'two-sided-short-edge'} onclick={() => (sides = 'two-sided-short-edge')}>{i18n.t.printDialog.twoShort}</button>
            </div>
          </div>
        {/if}
      </fieldset>
    </div>
  </div>

  {#snippet footer()}
    <div class="grow small">
      {#if blocker}
        <span class="err">{blocker}</span>
      {:else}
        <span class="muted">{p?.name} · {inkSummary(p)}</span>
      {/if}
    </div>
    <button class="btn" onclick={close}>{phase === 'setup' ? i18n.t.printDialog.cancel : i18n.t.printDialog.hide}</button>
    <button class="btn primary" onclick={print} disabled={!src || !selected?.length || !!blocker || phase !== 'setup'}>
      <Printer size={17} /> {i18n.t.printDialog.printBtn} {selected && selected.length * copies > 1 ? i18n.t.printDialog.pagesSuffix(selected.length * copies) : ''}
    </button>
  {/snippet}
</Modal>

<style>
  .layout {
    display: grid;
    grid-template-columns: 1.1fr 1fr;
    gap: 24px;
    padding: 20px;
  }
  .left {
    display: grid;
    gap: 10px;
    align-content: start;
    justify-items: center;
    background: var(--surface-3);
    border-radius: var(--radius);
    padding: 22px;
  }
  .preview {
    position: relative;
    width: 100%;
    max-width: 380px;
    max-height: 62vh;
    background: var(--paper);
    box-shadow: var(--shadow);
  }
  .preview :global(canvas.sheet) {
    display: block;
    width: 100%;
    height: 100%;
  }
  .margins {
    position: absolute;
    border: 1px dashed rgba(63, 86, 224, 0.55);
    pointer-events: none;
  }
  .loading {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    color: #888;
    padding: 20px;
    text-align: center;
  }
  .pager {
    justify-content: center;
  }
  .center {
    text-align: center;
  }
  .opts {
    display: grid;
    gap: 16px;
    align-content: start;
  }
  fieldset {
    border: 0;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 16px;
    min-width: 0;
  }
  fieldset:disabled {
    opacity: 0.55;
  }
  .stepper {
    display: flex;
    align-items: center;
    gap: 4px;
  }
  .stepper .input {
    width: 70px;
    text-align: center;
  }
  .progress {
    display: grid;
    gap: 10px;
    border-color: var(--accent);
  }
  .progress .btn {
    justify-self: start;
  }
  @media (max-width: 760px) {
    .layout {
      grid-template-columns: 1fr;
      padding: 14px;
      gap: 16px;
    }
    .left {
      padding: 14px;
    }
    .preview {
      max-width: 240px;
    }
  }
</style>
