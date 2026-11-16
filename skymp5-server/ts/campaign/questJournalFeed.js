"use strict";

// Read-only subscription. Actor identity always comes from the connection.
// No packet accepted here can set stages, objectives, rewards, or ownership.
class QuestJournalFeed {
  constructor(api) {
    this.api = api;
    this.subscriptions = new Map();
  }

  subscribe(user, content) {
    if (content.action !== "subscribe" || typeof content.subscriptionId !== "string" ||
        !/^[a-zA-Z0-9_-]{1,96}$/.test(content.subscriptionId)) return false;
    const previous = this.subscriptions.get(user);
    if (previous?.id === content.subscriptionId) return true;
    this.subscriptions.set(user, { id: content.subscriptionId, sequence: 0, last: "" });
    this.publish(user);
    return true;
  }

  disconnect(user) { this.subscriptions.delete(user); }

  poll() {
    for (const user of this.subscriptions.keys()) {
      try { this.publish(user); }
      catch { /* A failed send retries next poll without starving other players. */ }
    }
  }

  publish(user) {
    const subscription = this.subscriptions.get(user);
    if (!subscription) return;
    if (!this.api.connected(user)) return this.disconnect(user);
    let body;
    try {
      const actor = this.api.actor(user);
      if (!actor) body = { ok: false, error: "Character is not ready" };
      else {
        const journal = this.api.read(actor);
        body = { ok: true, journal };
        // Full snapshots are diagnostic for now. Fail explicitly rather than
        // truncate a large journal and accidentally erase client progress.
        if (Buffer.byteLength(JSON.stringify(body), "utf8") > 1024 * 1024)
          throw new Error("Journal exceeds diagnostic transport limit");
      }
    } catch {
      body = { ok: false, error: "Campaign journal is unavailable" };
    }
    const serialized = JSON.stringify(body);
    if (serialized === subscription.last) return;
    const packet = { customPacketType: "boyhouse.questJournal", subscriptionId: subscription.id,
      sequence: subscription.sequence + 1, ...body };
    this.api.send(user, packet);
    subscription.sequence = packet.sequence;
    subscription.last = serialized;
  }
}

module.exports = { QuestJournalFeed };
