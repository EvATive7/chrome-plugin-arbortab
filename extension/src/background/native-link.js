// WebSocket client for the ArborTab native module.

const PORTS = [47821, 47822, 47823, 47824, 47825];
const PROTOCOL = 1;

export class NativeLink {
  constructor({ onMessage, onStatus, onHello } = {}) {
    this.onMessage = onMessage || (() => {});
    this.onStatus = onStatus || (() => {});
    this.onHello = onHello || (() => {});
    this.socket = null;
    this.port = null;
    this.features = { layout: false, twisty: false, paint: false, drag: false };
    this.connected = false;
    this.portIndex = 0;
    this.retryDelay = 500;
    this.heartbeat = null;
    this.queue = [];
    this.stopped = false;
  }

  get isConnected() {
    return this.connected;
  }

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    if (this.heartbeat) {
      clearInterval(this.heartbeat);
      this.heartbeat = null;
    }
    if (this.socket) {
      try {
        this.socket.close();
      } catch {
        // ignore
      }
      this.socket = null;
    }
    this.connected = false;
    this.onStatus({ connected: false, features: this.features });
  }

  connect() {
    if (this.stopped) {
      return;
    }
    const port = PORTS[this.portIndex % PORTS.length];
    let socket;
    try {
      socket = new WebSocket(`ws://127.0.0.1:${port}/arbortab`);
    } catch {
      this.scheduleReconnect();
      return;
    }
    this.socket = socket;

    socket.addEventListener('open', () => {
      this.connected = true;
      this.port = port;
      this.retryDelay = 500;
      this.portIndex = 0;
      this.send({ type: 'hello', protocol: PROTOCOL, extVersion: chrome.runtime.getManifest().version });
      for (const message of this.queue.splice(0)) {
        this.send(message);
      }
      this.heartbeat = setInterval(() => this.send({ type: 'ping' }), 20000);
      this.onStatus({ connected: true, features: this.features });
    });

    socket.addEventListener('message', (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      if (message.type === 'hello') {
        this.features = message.features || this.features;
        this.onHello(message);
        this.onStatus({ connected: true, features: this.features, nativeVersion: message.nativeVersion });
        return;
      }
      this.onMessage(message);
    });

    const onDown = () => {
      if (!this.connected && this.socket !== socket) {
        return;
      }
      this.connected = false;
      this.socket = null;
      if (this.heartbeat) {
        clearInterval(this.heartbeat);
        this.heartbeat = null;
      }
      // Try the next port on reconnect: the browser may have moved to another one.
      this.portIndex += 1;
      this.onStatus({ connected: false, features: this.features });
      this.scheduleReconnect();
    };
    socket.addEventListener('close', onDown);
    socket.addEventListener('error', onDown);
  }

  scheduleReconnect() {
    if (this.stopped) {
      return;
    }
    const delay = this.retryDelay;
    this.retryDelay = Math.min(this.retryDelay * 2, 30000);
    setTimeout(() => this.connect(), delay);
  }

  send(message) {
    const payload = typeof message === 'string' ? message : JSON.stringify(message);
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      if (message.type === 'state') {
        // Keep only the newest state so a reconnect does not replay stale snapshots.
        this.queue = this.queue.filter((item) => item.type !== 'state');
      }
      this.queue.push(typeof message === 'string' ? JSON.parse(message) : message);
      return false;
    }
    this.socket.send(payload);
    return true;
  }
}
