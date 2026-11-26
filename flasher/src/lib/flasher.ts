import { ESPLoader, Transport, type FlashOptions, type IEspLoaderTerminal } from 'esptool-js';

export interface ChipInfo {
  chipName: string;
  mac: string;
  flashSizeKb: number;
}

export interface FlashPart {
  address: number;
  data: Uint8Array;
  name: string;
}

export function bytesToBinaryString(bytes: Uint8Array): string {
  const chunks: string[] = [];
  const step = 0x8000;
  for (let i = 0; i < bytes.length; i += step) {
    const sub = bytes.subarray(i, Math.min(i + step, bytes.length));
    chunks.push(String.fromCharCode(...sub));
  }
  return chunks.join('');
}

export class EspWebFlasher {
  private port: SerialPort | null = null;
  private transport: Transport | null = null;
  private loader: ESPLoader | null = null;

  get connected(): boolean {
    return this.loader !== null;
  }

  async connect(baudrate: number, terminal: IEspLoaderTerminal): Promise<ChipInfo> {
    if (!navigator.serial) {
      throw new Error('Web Serial API is not supported in this browser.');
    }
    const port = await navigator.serial.requestPort();
    const transport = new Transport(port, true);
    const loader = new ESPLoader({
      transport,
      baudrate,
      romBaudrate: 115200,
      terminal,
    });

    try {
      const chipName = await loader.main();
      let mac = '';
      try {
        mac = await loader.chip.readMac(loader);
      } catch {
        mac = 'unknown';
      }
      let flashSizeKb = 0;
      try {
        flashSizeKb = await loader.getFlashSize();
      } catch {
        flashSizeKb = 0;
      }
      this.port = port;
      this.transport = transport;
      this.loader = loader;
      return { chipName, mac, flashSizeKb };
    } catch (err) {
      try {
        await transport.disconnect();
      } catch {
        /* ignore cleanup error */
      }
      throw err;
    }
  }

  async eraseFlash(): Promise<void> {
    if (!this.loader) throw new Error('Not connected');
    await this.loader.eraseFlash();
  }

  async flash(
    parts: FlashPart[],
    eraseAll: boolean,
    onProgress: (fileIndex: number, written: number, total: number) => void,
  ): Promise<void> {
    if (!this.loader) throw new Error('Not connected');
    const fileArray = parts.map((p) => ({
      address: p.address,
      data: bytesToBinaryString(p.data),
    }));
    const opts: FlashOptions = {
      fileArray,
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll,
      compress: true,
      reportProgress: onProgress,
    };
    await this.loader.writeFlash(opts);
    await this.loader.after('hard_reset');
  }

  async hardReset(): Promise<void> {
    if (!this.loader) throw new Error('Not connected');
    await this.loader.after('hard_reset');
  }

  async disconnect(): Promise<void> {
    try {
      await this.transport?.disconnect();
    } catch {
      /* ignore */
    }
    try {
      await this.port?.close();
    } catch {
      /* ignore */
    }
    this.loader = null;
    this.transport = null;
    this.port = null;
  }
}
