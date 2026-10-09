import { EventEmitter } from 'events';
import * as fs from 'fs';
import * as path from 'path';
import { unframe, decodeEvent, decodeHello, EventMsg, HelloMsg, PROTO_VERSION } from './protocol.gen';

const MAX_LOG_BYTES = 5 * 1024 * 1024;

export class RotatingLogWriter {
  constructor(private file: string) {}
  append(line: string): void {
    try {
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      let size = 0;
      try {
        size = fs.statSync(this.file).size;
      } catch {
        // no file yet
      }
      if (size > MAX_LOG_BYTES) {
        try {
          fs.renameSync(this.file, `${this.file}.1`);
        } catch {
          // best effort
        }
      }
      fs.appendFileSync(this.file, line.endsWith('\n') ? line : `${line}\n`);
    } catch {
      // never let log-writing failures break the link
    }
  }
}

/** Common interface both the real serial link and the fake (test/simulator) link implement. */
export abstract class BaseLink extends EventEmitter {
  lastFrameSentAtMs: number | null = null;
  abstract writeLine(line: string): void;
  abstract release(seconds: number): Promise<void>;
  abstract reacquire(): Promise<void>;
  abstract close(): void;

  protected handleIncomingLine(line: string, logWriter?: RotatingLogWriter): void {
    if (line.startsWith('#CF')) {
      const decoded = unframe(line);
      if (!decoded) {
        this.emit('badFrame', line);
        return;
      }
      if (decoded.type === 'E') {
        try {
          const ev = decodeEvent(decoded.payload) as EventMsg;
          this.emit('event', ev);
        } catch {
          this.emit('badFrame', line);
        }
      } else if (decoded.type === 'H') {
        try {
          const hello = decodeHello(decoded.payload) as HelloMsg;
          if (hello.version !== PROTO_VERSION) {
            // eslint-disable-next-line no-console
            console.error(
              `[claude-bridge] watch protocol version mismatch: watch=${hello.version} bridge=${PROTO_VERSION} (watch shows UPDATE BRIDGE) — continuing to send`,
            );
          }
          this.emit('hello', hello);
        } catch {
          this.emit('badFrame', line);
        }
      } else {
        this.emit('badFrame', line);
      }
    } else {
      logWriter?.append(line);
      this.emit('watchLog', line);
    }
  }

  send(line: string): void {
    this.writeLine(line);
    this.lastFrameSentAtMs = Date.now();
    this.emit('frameSent', line);
  }
}

/** In-memory link for tests and `--fake-link`: no real serial I/O. */
export class FakeLink extends BaseLink {
  sent: string[] = [];
  private released = false;
  private onDataSink: ((line: string) => void) | null = null;

  constructor(private logWriter?: RotatingLogWriter, private sink?: (line: string) => void) {
    super();
  }

  connectNow(): void {
    this.emit('connect');
  }

  disconnectNow(): void {
    this.emit('disconnect');
  }

  writeLine(line: string): void {
    if (this.released) return;
    this.sent.push(line);
    this.sink?.(line);
  }

  /** Test/simulator helper: feed a line as if it arrived from the watch. */
  feed(line: string): void {
    this.handleIncomingLine(line, this.logWriter);
  }

  async release(_seconds: number): Promise<void> {
    this.released = true;
  }

  async reacquire(): Promise<void> {
    this.released = false;
  }

  close(): void {
    this.released = true;
  }
}

/** Real USB-serial link to the watch. Probes /dev/cu.usbmodem* candidates, or an
 *  explicit configured path, for a device that answers '#CFQ\n' with a HELLO frame. */
export class SerialLink extends BaseLink {
  private port: import('serialport').SerialPort | null = null;
  private closed = false;
  private backoffMs = 1000;
  private reconnectTimer: NodeJS.Timeout | null = null;
  private explicitPath: string | null;
  private logWriter: RotatingLogWriter;
  private releasedUntil = 0;
  private lineBuf = '';

  constructor(explicitPath: string | null, watchLogFile: string) {
    super();
    this.explicitPath = explicitPath;
    this.logWriter = new RotatingLogWriter(watchLogFile);
  }

  async start(): Promise<void> {
    this.closed = false;
    await this.connectLoop();
  }

  private async connectLoop(): Promise<void> {
    if (this.closed) return;
    try {
      const found = await this.findAndOpen();
      if (!found) throw new Error('no watch found on any /dev/cu.usbmodem*');
      this.backoffMs = 1000;
    } catch (err) {
      this.emit('connectError', err);
      this.reconnectTimer = setTimeout(() => this.connectLoop(), this.backoffMs);
      this.backoffMs = Math.min(3000, this.backoffMs * 2); // re-plugged watch is picked up within ~3 s
    }
  }

  private async findAndOpen(): Promise<boolean> {
    const { SerialPort } = await import('serialport');
    const candidates: string[] = [];
    if (this.explicitPath) {
      candidates.push(this.explicitPath);
    } else {
      const ports = await SerialPort.list();
      // SerialPort.list() reports the callin device (/dev/tty.usbmodem*) on
      // macOS; the callout device (/dev/cu.*) is the one to open (tty.* blocks
      // on carrier detect). Espressif USB-Serial-JTAG = VID 0x303a, so try
      // those first, then any other usbmodem.
      const toCu = (p: string) => p.replace('/dev/tty.', '/dev/cu.');
      const modems = ports.filter((p) => p.path && p.path.includes('usbmodem'));
      modems.sort((a, b) => Number(b.vendorId === '303a') - Number(a.vendorId === '303a'));
      for (const p of modems) {
        const cu = toCu(p.path);
        if (!candidates.includes(cu)) candidates.push(cu);
      }
    }
    for (const candidate of candidates) {
      const ok = await this.probeAndOpen(candidate, SerialPort);
      if (ok) return true;
    }
    return false;
  }

  private probeAndOpen(
    devPath: string,
    SerialPortCtor: typeof import('serialport').SerialPort,
  ): Promise<boolean> {
    return new Promise((resolve) => {
      let settled = false;
      let port: import('serialport').SerialPort;
      try {
        port = new SerialPortCtor({
          path: devPath,
          baudRate: 115200,
          autoOpen: false,
          hupcl: false, // never toggle DTR/RTS: ESP32-S3 USB-JTAG resets on certain transitions
        });
      } catch {
        resolve(false);
        return;
      }
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        try {
          port.close();
        } catch {
          // ignore
        }
        resolve(false);
      }, 1500);

      port.open((err) => {
        if (err) {
          clearTimeout(timer);
          if (!settled) {
            settled = true;
            resolve(false);
          }
          return;
        }
        let buf = '';
        const onData = (chunk: Buffer) => {
          buf += chunk.toString('utf8');
          let idx: number;
          while ((idx = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, idx).trim();
            buf = buf.slice(idx + 1);
            if (line.startsWith('#CFH')) {
              const decoded = unframe(line);
              if (decoded && decoded.type === 'H' && !settled) {
                settled = true;
                clearTimeout(timer);
                port.removeListener('data', onData);
                this.attachOpenPort(port, devPath);
                resolve(true);
                return;
              }
            }
          }
        };
        port.on('data', onData);
        port.write('#CFQ\n');
      });
    });
  }

  private attachOpenPort(port: import('serialport').SerialPort, devPath: string): void {
    this.port = port;
    this.lineBuf = '';
    port.on('data', (chunk: Buffer) => {
      this.lineBuf += chunk.toString('utf8');
      let idx: number;
      while ((idx = this.lineBuf.indexOf('\n')) >= 0) {
        const line = this.lineBuf.slice(0, idx).trim();
        this.lineBuf = this.lineBuf.slice(idx + 1);
        if (line) this.handleIncomingLine(line, this.logWriter);
      }
    });
    port.on('close', () => {
      this.port = null;
      this.emit('disconnect');
      if (!this.closed) this.reconnectTimer = setTimeout(() => this.connectLoop(), this.backoffMs);
    });
    port.on('error', (err) => {
      this.emit('linkError', err);
    });
    this.emit('connect', devPath);
  }

  writeLine(line: string): void {
    if (Date.now() < this.releasedUntil) return;
    if (!this.port || !this.port.isOpen) return;
    this.port.write(line);
  }

  async release(seconds: number): Promise<void> {
    this.releasedUntil = Date.now() + seconds * 1000;
    if (this.port) {
      await new Promise<void>((resolve) => {
        this.port!.close(() => resolve());
      });
      this.port = null;
    }
  }

  async reacquire(): Promise<void> {
    this.releasedUntil = 0;
    if (!this.port) await this.connectLoop();
  }

  close(): void {
    this.closed = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    if (this.port) {
      try {
        this.port.close();
      } catch {
        // ignore
      }
      this.port = null;
    }
  }
}
