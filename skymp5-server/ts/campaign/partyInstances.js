"use strict";

// Experimental control plane. Native worldInstance enforces replication and
// interaction boundaries; vanilla reference overlays are a separate milestone.
class PartyInstances {
  constructor(adapter, areas, now = () => Date.now()) {
    this.adapter = adapter;
    this.now = now;
    this.areas = new Map();
    this.parties = new Map();
    this.membership = new Map();
    this.invites = new Map();
    this.visits = new Map();
    for (const area of areas) {
      if (!area || !/^[a-zA-Z0-9_-]{1,48}$/.test(area.id) || this.areas.has(area.id)) {
        throw new Error("Invalid or duplicate instance area");
      }
      if (!Number.isInteger(area.worldOrCell) || area.worldOrCell < 0 ||
          area.worldOrCell > 0xffffffff || !Array.isArray(area.center) ||
          area.center.length !== 3 || !area.center.every(Number.isFinite) ||
          !Number.isFinite(area.radius) || area.radius <= 0) {
        throw new Error("Invalid instance area boundary");
      }
      this.areas.set(area.id, { ...area, center: [...area.center] });
    }
  }

  actor(id) {
    if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff || !this.adapter.exists(id)) {
      throw new Error("Unknown character");
    }
    return id;
  }

  sameParty(a, b) {
    return this.membership.has(a) && this.membership.get(a) === this.membership.get(b);
  }

  key(owner, area) { return `campaign:${owner}:${area}`; }

  invite(actor, target) {
    this.actor(actor);
    this.actor(target);
    if (actor === target || this.membership.has(target)) throw new Error("Target already in a party");
    let leader = this.membership.get(actor);
    if (leader === undefined) {
      leader = actor;
      this.membership.set(actor, actor);
      this.parties.set(actor, new Set([actor]));
    }
    if (leader !== actor) throw new Error("Only the party leader can invite");
    if (this.parties.get(leader).size >= 32) throw new Error("Party is full");
    this.invites.set(target, { leader, expires: this.now() + 60000 });
  }

  accept(actor, leader) {
    this.actor(actor);
    const invite = this.invites.get(actor);
    if (!invite || invite.leader !== leader || invite.expires <= this.now()) {
      throw new Error("Invitation missing or expired");
    }
    const party = this.parties.get(leader);
    if (!party || party.size >= 32 || this.membership.has(actor)) throw new Error("Cannot join party");
    party.add(actor);
    this.membership.set(actor, leader);
    this.invites.delete(actor);
  }

  leave(actor) {
    this.actor(actor);
    const leader = this.membership.get(actor);
    if (leader === undefined) return;
    if (leader === actor) {
      for (const member of this.parties.get(leader)) this.membership.delete(member);
      this.parties.delete(leader);
      for (const [target, invite] of this.invites) {
        if (invite.leader === leader) this.invites.delete(target);
      }
    } else {
      this.parties.get(leader).delete(actor);
      this.membership.delete(actor);
    }
    this.reconcile();
  }

  kick(actor, target) {
    if (this.membership.get(actor) !== actor || !this.sameParty(actor, target) || actor === target) {
      throw new Error("Only the leader can remove another party member");
    }
    this.leave(target);
  }

  enter(actor, owner, areaId) {
    this.actor(actor);
    this.actor(owner);
    const area = this.areas.get(areaId);
    if (!area) throw new Error("Area is not enabled for instancing");
    if (actor !== owner && !this.sameParty(actor, owner)) throw new Error("Party membership required");
    if (!this.adapter.isInside(actor, area)) throw new Error("Character is outside the instance boundary");
    if (actor !== owner) {
      const hostVisit = this.visits.get(owner);
      if (!hostVisit || hostVisit.owner !== owner || hostVisit.area !== areaId ||
          !this.adapter.isInside(owner, area)) throw new Error("Campaign owner is not in that instance");
    }
    this.adapter.setInstance(actor, this.key(owner, areaId));
    this.visits.set(actor, { owner, area: areaId });
  }

  exit(actor) {
    this.actor(actor);
    const visit = this.visits.get(actor);
    if (visit && this.adapter.isInside(actor, this.areas.get(visit.area))) {
      throw new Error("Leave the altered area before returning to shared space");
    }
    this.adapter.setInstance(actor, "");
    this.visits.delete(actor);
  }

  reconcile() {
    for (const [actor, visit] of this.visits) {
      if (actor === visit.owner || this.sameParty(actor, visit.owner)) continue;
      try {
        this.adapter.setInstance(actor, this.key(actor, visit.area));
        this.visits.set(actor, { owner: actor, area: visit.area });
      } catch (error) {
        // A failed native transition must not leave a revoked guest active.
        this.adapter.disable(actor);
        this.visits.delete(actor);
      }
    }
  }

  recover(actor) {
    this.actor(actor);
    const saved = this.adapter.getInstance(actor);
    if (!saved) { this.visits.delete(actor); return; }
    const match = /^campaign:([0-9]+):([a-zA-Z0-9_-]{1,48})$/.exec(saved);
    if (!match || !this.areas.has(match[2])) {
      this.adapter.disable(actor);
      throw new Error("Saved instance needs administrator recovery");
    }
    const owner = Number(match[1]);
    const authorizedOwner = owner === actor || this.sameParty(actor, owner) ? owner : actor;
    this.adapter.setInstance(actor, this.key(authorizedOwner, match[2]));
    this.visits.set(actor, { owner: authorizedOwner, area: match[2] });
  }

  snapshot(actor) {
    const leader = this.membership.get(actor);
    return {
      character: actor,
      leader: leader ?? null,
      members: leader === undefined ? [] : [...this.parties.get(leader)],
      visit: this.visits.has(actor) ? { ...this.visits.get(actor) } : null,
      invitation: this.invites.has(actor) ? { ...this.invites.get(actor) } : null,
    };
  }
}

module.exports = { PartyInstances };
