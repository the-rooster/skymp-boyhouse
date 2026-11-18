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
    this.visibilityParticipants = new Map();
  }

  connect(user) {
    this.adapter.block(user, true);
    this.adapter.beginEpoch(user);
    this.users.set(user, { session: "", sequence: 0, context: "", pending: null,
      dirty: true, deadline: this.now() + 30000 });
  }

  disconnect(user) { this.users.delete(user); }

  captureConnection(user) {
    const state = this.users.get(user);
    // The state belongs to the socket and survives ordinary epoch rotations.
    return () => this.adapter.connected(user) && this.users.get(user) === state;
  }

  send(user, state, body) {
    this.adapter.send(user, { customPacketType: TYPE, protocolVersion: 2, sessionId: state.session,
      sequence: state.sequence, ...body });
  }

  fail(user, error, expected = this.users.get(user)) {
    if (this.users.get(user) !== expected) return;
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
    if (this.users.get(user) !== state || state.publishing) return;
    state.publishing = true;
    try { this.publishNow(user, state); }
    finally { state.publishing = false; }
  }

  publishNow(user, state) {
    const actor = this.adapter.actor(user);
    if (!actor || !state.session || state.visibilityHeld) return;
    const resume = this.adapter.resume?.(user);
    if (resume) {
      if (typeof resume.attempt !== "string" || !resume.attempt ||
          !["Pending", "ProjectionReady", "Failed"].includes(resume.status))
        throw Error("Invalid native campaign resume state");
      if (resume.status === "Failed") throw Error(resume.error || "Campaign resume failed");
      state.resumeAttempt = resume.attempt;
      if (resume.status === "Pending") {
        // The native hold already blocks input. Do not rotate packet epochs
        // or reconstruct projection bytes while its exact save is outstanding.
        state.pending = null;
        state.dirty = true;
        return;
      }
    }
    this.adapter.block(user, true);
    const token = this.adapter.beginEpoch(user);
    if (!/^[a-f0-9]{32}$/.test(token)) throw Error("Invalid native packet epoch");
    const captured = resume ? this.adapter.captureResume(user, token) : null;
    if (resume && (!captured || captured.attempt !== resume.attempt ||
        typeof captured.proof !== "string" || !captured.proof))
      throw Error("Invalid native campaign resume projection");
    const manifest = captured ? captured.manifest : this.adapter.read(actor);
    if (!manifest || manifest.actor !== actor || !Array.isArray(manifest.references) ||
        manifest.references.length > 4096) throw Error("Invalid projection manifest");
    const encoded = JSON.stringify(manifest);
    if (Buffer.byteLength(encoded, "utf8") > 1024 * 1024) throw Error("Projection manifest is too large");
    const baseline = captured ? captured.baseline : this.adapter.baseline(user, token);
    if (baseline?.schemaVersion !== 1 || baseline.actor !== actor || baseline.packetEpoch !== token ||
        !Array.isArray(baseline.messages)) throw Error("Invalid world baseline");
    const baselineText = JSON.stringify(baseline);
    if (Buffer.byteLength(baselineText, "utf8") > 2 * 1024 * 1024)
      throw Error("World baseline is too large");
    const current = () => this.users.get(user) === state && this.adapter.connected(user) &&
      this.adapter.actor(user) === actor && this.adapter.epoch(user) === token;
    if (!current()) return;
    const { references, ...metadata } = manifest;
    if (state.sequence >= Number.MAX_SAFE_INTEGER) throw Error("Projection sequence exhausted");
    ++state.sequence;
    const pending = { token, actor, encoded, attempt: captured?.attempt, proof: captured?.proof };
    state.released = null;
    state.pending = pending;
    state.context = this.context(user);
    state.dirty = false;
    if (!state.deadline) state.deadline = this.now() + 30000;
    const chunks = Math.ceil(references.length / CHUNK_SIZE);
    this.send(user, state, { kind: "begin", token, metadata, count: references.length, chunks,
      baselineLength: baselineText.length, baselineChunks: Math.ceil(baselineText.length / BASELINE_CHUNK_SIZE) });
    for (let index = 0; index < chunks; ++index) {
      if (!current() || state.pending !== pending) return;
      this.send(user, state, { kind: "chunk", token, index,
        references: references.slice(index * CHUNK_SIZE, (index + 1) * CHUNK_SIZE) });
    }
    for (let offset = 0; offset < baselineText.length; offset += BASELINE_CHUNK_SIZE) {
      if (!current() || state.pending !== pending) return;
      this.send(user, state, { kind: "baseline", token, index: offset / BASELINE_CHUNK_SIZE,
        data: baselineText.slice(offset, offset + BASELINE_CHUNK_SIZE) });
    }
  }

  // A cue can reference only NPC copies in the exact manifest this socket applied.
  isReleased(user, release) {
    const state = this.users.get(user);
    return !!release && state?.released === release && !state.pending && !state.dirty &&
      !state.deadline && !state.visibilityHeld && this.adapter.connected(user) && !this.adapter.blocked(user) &&
      this.adapter.actor(user) === release.actor && this.adapter.epoch(user) === release.token &&
      this.context(user) === state.context;
  }

  releasedProjection(user) {
    const release = this.users.get(user)?.released;
    if (!this.isReleased(user, release)) return null;
    const manifest = this.adapter.read(release.actor);
    if (JSON.stringify(manifest) !== release.encoded) return null;
    return { release, manifest };
  }

  sendSceneLine(user, release, sceneId, line, slotMs) {
    const sequence = (release.dialogueSequence ?? 0) + 1;
    if (!Number.isSafeInteger(sequence)) throw Error("Dialogue sequence exhausted");
    release.dialogueSequence = sequence;
    this.adapter.send(user, { customPacketType: "boyhouse.sceneDialogue", protocolVersion: 1,
      packetEpoch: release.token, sequence, sceneId, actionIndex: line.index,
      speaker: line.speakerCopyId, topicId: line.topicId, ...(slotMs === undefined ? {} : { slotMs }) });
  }

  pauseUser(user, state) {
    const actor = this.adapter.actor(user);
    try {
      this.adapter.block(user, true);
      this.adapter.beginEpoch(user);
      if (state.sequence >= Number.MAX_SAFE_INTEGER) throw Error("Projection sequence exhausted");
      ++state.sequence;
      state.released = null;
      state.pending = null;
      state.dirty = true;
      state.deadline = this.now() + 30000;
      if (state.session) this.send(user, state, { kind: "pause" });
    } catch (error) {
      if (this.users.get(user) === state && this.adapter.actor(user) === actor)
        this.fail(user, error);
      throw error;
    }
  }

  pauseActor(actor) {
    for (const [user, state] of this.users) {
      if (this.adapter.connected(user) && this.adapter.actor(user) === actor)
        this.pauseUser(user, state);
    }
  }

  visibilityUsers(users) {
    if (!Array.isArray(users) || users.length > 1000 || new Set(users).size !== users.length ||
        users.some(user => !Number.isInteger(user) || user < 0 || user > 0xffffffff))
      throw Error("Invalid campaign visibility participants");
    return users.map(user => {
      const state = this.users.get(user);
      if (!state || !this.adapter.connected(user) || !this.adapter.actor(user))
        throw Error("Campaign visibility requires an active projection connection");
      return { user, state, actor: this.adapter.actor(user) };
    });
  }

  sameVisibilityParticipant({ user, state, actor }) {
    return this.users.get(user) === state && this.adapter.connected(user) &&
      this.adapter.actor(user) === actor;
  }

  // Only the native commit barrier supplies this complete participant list.
  // Hold publishing until publication and subscription reconciliation finish,
  // including a reentrant hello/poll during the synchronous native callback.
  prepareVisibility(users) {
    const participants = this.visibilityUsers(users);
    if (participants.some(({ user, state }) => state.visibilityHeld || this.visibilityParticipants.has(user)))
      throw Error("Campaign visibility is already changing");
    // Keep the original connection state outside users: a disconnect/reconnect
    // can replace that map entry before the paired native cleanup arrives.
    for (const participant of participants) {
      const { user, state } = participant;
      this.visibilityParticipants.set(user, participant);
      state.visibilityHeld = true;
    }
    for (const participant of participants) {
      if (!this.sameVisibilityParticipant(participant))
        throw Error("Campaign visibility participant changed during preparation");
      this.adapter.block(participant.user, true);
    }
    for (const participant of participants) {
      if (!this.sameVisibilityParticipant(participant))
        throw Error("Campaign visibility participant changed during preparation");
      this.pauseUser(participant.user, participant.state);
    }
    return true;
  }

  abortVisibility(users) {
    let failure;
    for (const user of users) {
      const participant = this.visibilityParticipants.get(user);
      if (!participant) continue;
      this.visibilityParticipants.delete(user);
      participant.state.visibilityHeld = false;
      try {
        if (this.sameVisibilityParticipant(participant))
          this.fail(user, Error("Campaign visibility transaction did not synchronize"));
      }
      catch (error) { failure = error; }
    }
    if (failure) throw failure; // Native fallback closes any remaining sockets.
    return true;
  }

  completeVisibility(users, synchronized) {
    if (!synchronized) return this.abortVisibility(users);
    try {
      this.visibilityUsers(users);
      const participants = users.map(user => this.visibilityParticipants.get(user));
      for (const participant of participants) {
        if (!participant || !participant.state.visibilityHeld || !this.sameVisibilityParticipant(participant))
          throw Error("Campaign visibility participant changed during commit");
      }
      for (const { state } of participants) state.visibilityHeld = false;
      for (const participant of participants) {
        if (!this.sameVisibilityParticipant(participant))
          throw Error("Campaign visibility participant changed during publication");
        this.publish(participant.user, participant.state);
        if (!this.sameVisibilityParticipant(participant))
          throw Error("Campaign visibility participant changed during publication");
      }
      for (const { user } of participants) this.visibilityParticipants.delete(user);
      return true;
    } catch (error) {
      this.abortVisibility(users);
      throw error;
    }
  }

  packet(user, packet) {
    const state = this.users.get(user);
    if (!state || !this.adapter.connected(user)) return;
    if (state.visibilityHeld && packet.action !== "hello") return;
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
        if (pending.attempt) {
          const session = state.session;
          const sequence = state.sequence;
          const current = () => this.users.get(user) === state && this.adapter.connected(user) &&
            this.adapter.actor(user) === pending.actor && state.pending === pending &&
            state.session === session && state.sequence === sequence &&
            this.adapter.epoch(user) === pending.token;
          if (!current()) return;
          const accepted = this.adapter.completeResume(user, pending.attempt, pending.token, pending.proof, () => {
            if (!current()) throw Error("Campaign resume connection changed before ready");
            this.send(user, state, { kind: "ready", token: pending.token });
            if (!current()) throw Error("Campaign resume connection changed during ready");
          });
          // Native owns release and checks its authority proof after send.
          // In particular, never call the ordinary unblock API for this lane.
          if (!current()) return;
          state.pending = null;
          state.dirty = !accepted;
          if (accepted) { state.deadline = 0; state.resumeAttempt = null; state.released = pending; }
          else this.publish(user, state);
          return;
        }
        if (this.adapter.resume?.(user)) {
          state.dirty = true;
          this.publish(user, state);
          return;
        }
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
        state.released = pending;
        state.deadline = 0;
      }
    } catch (error) { this.fail(user, error, state); }
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
        const resume = this.adapter.resume?.(user);
        if (context && (state.dirty || context !== state.context ||
            (resume && (resume.status === "Failed" || resume.attempt !== state.resumeAttempt))))
          this.publish(user, state);
      } catch (error) { this.fail(user, error, state); }
    }
  }
}

module.exports = { ProjectionFeed };
