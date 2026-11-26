export type LineEnding = 'crlf' | 'lf' | 'none';

export class SerialMonitor {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private reading = false;

  get connected(): boolean {
    return this.port !== null;
  }

  async connect(
    baudRate: number,
    onChunk: (text: string) => void,
    onDisconnect: () => void,
  ): Promise<void> {
    if (!navigator.serial) {
      throw new Error('Web Serial API is not supported in this browser.');
    }
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate });
    this.port = port;
    this.reading = true;
    if (port.writable) {
      this.writer = port.writable.getWriter();
    }

    const decoder = new TextDecoder();
    if (port.readable) {
      this.reader = port.readable.getReader();
      void (async () => {
        try {
          while (this.reading && this.reader) {
            const { value, done } = await this.reader.read();
            if (done) break;
            if (value) onChunk(decoder.decode(value, { stream: true }));
          }
        } catch {
          /* port closed or detached */
        } finally {
          const remaining = decoder.decode();
          if (remaining) onChunk(remaining);
          await this.disconnect();
          onDisconnect();
        }
      })();
    }
  }

  async send(text: string, lineEnding: LineEnding): Promise<void> {
    if (!this.writer) throw new Error('Serial port is not writable');
    const suffix = lineEnding === 'crlf' ? '\r\n' : lineEnding === 'lf' ? '\n' : '';
    const bytes = new TextEncoder().encode(text + suffix);
    await this.writer.write(bytes);
  }

  async hardReset(): Promise<void> {
    if (!this.port) throw new Error('Not connected');
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: true });
    await new Promise((r) => setTimeout(r, 120));
    await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
  }

  async disconnect(): Promise<void> {
    this.reading = false;
    try {
      await this.reader?.cancel();
    } catch {
      /* ignore */
    }
    try {
      this.reader?.releaseLock();
    } catch {
      /* ignore */
    }
    try {
      this.writer?.releaseLock();
    } catch {
      /* ignore */
    }
    try {
      await this.port?.close();
    } catch {
      /* ignore */
    }
    this.reader = null;
    this.writer = null;
    this.port = null;
  }
}
