"use strict";

const TYPE = "boyhouse.projection";
const CHUNK_SIZE = 128;
const BASELINE_CHUNK_SIZE = 16384;

// The connection owns the acknowledgement. Neither a supplied actor ID nor an
// old connection's nonce can release another character's gameplay gate.
class ProjectionFeed {
  constructor(adapter, now = () => Date.now()) {
    this.adapter = adapter;
    this.now = now;
    this.users = new Map();
  }

  connect(user) {
    this.adapter.block(user, true);
    this.adapter.beginEpoch(user);
    this.users.set(user, { session: "", sequence: 0, context: "", pending: null,
      dirty: true, deadline: this.now() + 30000 });
  }

  disconnect(user) { this.users.delete(user); }

  send(user, state, body) {
    this.adapter.send(user, { customPacketType: TYPE, protocolVersion: 2, sessionId: state.session,
      sequence: state.sequence, ...body });
  }

  fail(user, error) {
    this.users.delete(user);
    // Keep the native gate closed even if transport cleanup itself fails.
    if (this.adapter.connected(user)) {
      this.adapter.block(user, true);
      this.adapter.close(user, error);
    }
  }

  context(user) {
    const actor = this.adapter.actor(user);
    return actor ? `${actor}:${this.adapter.instance(actor)}` : "";
  }

  publish(user, state) {
    const actor = this.adapter.actor(user);
    if (!actor || !state.session) return;
    this.adapter.block(user, true);
    const token = this.adapter.beginEpoch(user);
    if (!/^[a-f0-9]{32}$/.test(token)) throw Error("Invalid native packet epoch");
    const manifest = this.adapter.read(actor);
    if (!manifest || manifest.actor !== actor || !Array.isArray(manifest.references) ||
        manifest.references.length > 4096) throw Error("Invalid projection manifest");
    const encoded = JSON.stringify(manifest);
    if (Buffer.byteLength(encoded, "utf8") > 1024 * 1024) throw Error("Projection manifest is too large");
    const baseline = this.adapter.baseline(user, token);
    if (baseline?.schemaVersion !== 1 || baseline.actor !== actor || baseline.packetEpoch !== token ||
        !Array.isArray(baseline.messages)) throw Error("Invalid world baseline");
    const baselineText = JSON.stringify(baseline);
    if (Buffer.byteLength(baselineText, "utf8") > 2 * 1024 * 1024)
      throw Error("World baseline is too large");
    const { references, ...metadata } = manifest;
    if (state.sequence >= Number.MAX_SAFE_INTEGER) throw Error("Projection sequence exhausted");
    ++state.sequence;
    state.pending = { token, actor, encoded };
    state.context = this.context(user);
    state.dirty = false;
    if (!state.deadline) state.deadline = this.now() + 30000;
    const chunks = Math.ceil(references.length / CHUNK_SIZE);
    this.send(user, state, { kind: "begin", token, metadata, count: references.length, chunks,
      baselineLength: baselineText.length, baselineChunks: Math.ceil(baselineText.length / BASELINE_CHUNK_SIZE) });
    for (let index = 0; index < chunks; ++index) {
      this.send(user, state, { kind: "chunk", token, index,
        references: references.slice(index * CHUNK_SIZE, (index + 1) * CHUNK_SIZE) });
    }
    for (let offset = 0; offset < baselineText.length; offset += BASELINE_CHUNK_SIZE) {
      this.send(user, state, { kind: "baseline", token, index: offset / BASELINE_CHUNK_SIZE,
        data: baselineText.slice(offset, offset + BASELINE_CHUNK_SIZE) });
    }
  }

  pauseActor(actor) {
    for (const [user, state] of this.users) {
      if (!this.adapter.connected(user) || this.adapter.actor(user) !== actor) continue;
      try {
        this.adapter.block(user, true);
        this.adapter.beginEpoch(user);
        if (state.sequence >= Number.MAX_SAFE_INTEGER) throw Error("Projection sequence exhausted");
        ++state.sequence;
        state.pending = null;
        state.dirty = true;
        state.deadline = this.now() + 30000;
        if (state.session) this.send(user, state, { kind: "pause" });
      } catch (error) { this.fail(user, error); throw error; }
    }
  }

  packet(user, packet) {
    const state = this.users.get(user);
    if (!state || !this.adapter.connected(user)) return;
    try {
      if (packet.action === "hello") {
        if (packet.protocolVersion !== 2) throw Error("Campaign projection requires protocol version 2");
        if (typeof packet.sessionId !== "string" || !/^[a-zA-Z0-9_-]{1,64}$/.test(packet.sessionId)) return;
        if (state.session && state.session !== packet.sessionId) return;
        if (state.session) return; // Reliable transport; repeats cannot extend the deadline.
        state.session = packet.sessionId;
        this.publish(user, state);
        return;
      }
      const pending = state.pending;
      if (!pending || packet.protocolVersion !== 2 || packet.sessionId !== state.session || packet.sequence !== state.sequence ||
          packet.token !== pending.token || this.adapter.actor(user) !== pending.actor) return;
      if (packet.action === "failed") {
        this.fail(user, Error("Client could not apply the campaign projection"));
      } else if (packet.action === "ack") {
        // State may change while the engine applies the manifest. Never release
        // against an obsolete mapping, phase, owner identity, or disabled state.
        if (this.adapter.epoch(user) !== pending.token || this.context(user) !== state.context ||
            JSON.stringify(this.adapter.read(pending.actor)) !== pending.encoded) {
          this.publish(user, state);
          return;
        }
        this.send(user, state, { kind: "ready", token: pending.token });
        this.adapter.block(user, false);
        state.pending = null;
        state.deadline = 0;
      }
    } catch (error) { this.fail(user, error); }
  }

  poll() {
    for (const [user, state] of this.users) {
      try {
        if (!this.adapter.connected(user)) { this.disconnect(user); continue; }
        if (state.deadline && state.deadline <= this.now()) {
          this.fail(user, Error("Campaign projection acknowledgement timed out"));
          continue;
        }
        // Active sessions only read their actor and phase, not every reference
        // ledger four times per second. State is checked again on each ack.
        const context = this.context(user);
        if (context && (state.dirty || context !== state.context)) this.publish(user, state);
      } catch (error) { this.fail(user, error); }
    }
  }
}

module.exports = { ProjectionFeed };
