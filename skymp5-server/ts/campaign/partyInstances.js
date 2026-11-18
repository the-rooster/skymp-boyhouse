"use strict";

// Experimental control plane. Native worldInstance enforces replication and
// interaction boundaries; vanilla reference overlays are a separate milestone.
class PartyInstances {
  constructor(adapter, areas, now = () => Date.now()) {
    if (typeof adapter.prepareInstance !== "function") {
      throw new Error("Instance preparation is required");
    }
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
      if (!Number.isInteger(area.worldOrCell) || area.worldOrCell <= 0 ||
          area.worldOrCell >= 0xff000000 || !Array.isArray(area.center) ||
          area.center.length !== 3 || !area.center.every(Number.isFinite) ||
          !Number.isFinite(area.radius) || area.radius <= 0 || area.radius > 65536 ||
          area.center.slice(0, 2).some(n => Math.abs(n) + area.radius > 60000000) ||
          !Number.isInteger(area.encounterLevel) || area.encounterLevel < 1 || area.encounterLevel > 65535) {
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

  quarantine(actor) {
    this.visits.delete(actor);
    this.removeMembership(actor);
    this.adapter.disable(actor);
  }

  hostAvailable(owner, areaId, requireReady = false) {
    const visit = this.visits.get(owner);
    return this.adapter.exists(owner) && visit?.owner === owner && visit.area === areaId &&
      this.adapter.getInstance(owner) === this.key(owner, areaId) &&
      this.adapter.isInside(owner, this.areas.get(areaId)) &&
      (!requireReady || !this.adapter.isReady || this.adapter.isReady(owner));
  }

  canObserveScene(actor, owner) {
    const visit = this.visits.get(actor);
    return !!visit && visit.owner === owner && this.hostAvailable(owner, visit.area, true) &&
      (actor === owner || this.sameParty(actor, owner)) &&
      this.adapter.getInstance(actor) === this.key(owner, visit.area) &&
      this.adapter.isInside(actor, this.areas.get(visit.area));
  }

  transition(actor, owner, areaId) {
    // Preparation is synchronous and never changes a player's current phase.
    // Do not publish a visit until every destination reference is available.
    if (owner !== null) this.adapter.prepareInstance(owner, this.areas.get(areaId));
    try {
      this.adapter.setInstance(actor, owner === null ? "" : this.key(owner, areaId));
    } catch (error) {
      // Native subscription or script loading can throw after changing the
      // phase. Quarantine this character instead of claiming the old visit.
      this.quarantine(actor);
      throw error;
    }
    if (owner === null) this.visits.delete(actor);
    else this.visits.set(actor, { owner, area: areaId });
  }

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
    this.removeMembership(actor);
    this.reconcile();
  }

  removeMembership(actor) {
    this.invites.delete(actor);
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
    if (actor !== owner && !this.hostAvailable(owner, areaId, true)) {
      throw new Error("Campaign owner is not in that instance");
    }
    try { this.transition(actor, owner, areaId); }
    finally { this.reconcile(); }
  }

  exit(actor) {
    this.actor(actor);
    const visit = this.visits.get(actor);
    if (visit && this.adapter.isInside(actor, this.areas.get(visit.area))) {
      throw new Error("Leave the altered area before returning to shared space");
    }
    try { this.transition(actor, null); }
    finally { this.reconcile(); }
  }

  reconcile() {
    const changed = [];
    for (const [actor, visit] of this.visits) {
      if (!this.adapter.exists(actor)) {
        this.visits.delete(actor);
        changed.push(actor);
        continue;
      }
      try {
        if (this.adapter.getInstance(actor) !== this.key(visit.owner, visit.area)) {
          throw new Error("Instance visit disagrees with native phase");
        }
        if (!this.adapter.isInside(actor, this.areas.get(visit.area))) {
          this.transition(actor, null);
        } else if (actor !== visit.owner &&
                   (!this.sameParty(actor, visit.owner) || !this.hostAvailable(visit.owner, visit.area))) {
          this.transition(actor, actor, visit.area);
        } else continue;
      } catch (error) {
        // In contrast to voluntary entry, a failed revocation cannot retain
        // the old access even when preparation failed before switching.
        this.quarantine(actor);
      }
      changed.push(actor);
    }
    return changed;
  }

  recover(actor, request, current = () => true) {
    this.actor(actor);
    const saved = this.adapter.getInstance(actor);
    if (!saved) { this.visits.delete(actor); return; }
    const match = /^campaign:([0-9]+):([a-zA-Z0-9_-]{1,48})$/.exec(saved);
    const owner = match ? Number(match[1]) : 0;
    if (!match || !this.areas.has(match[2]) || !Number.isInteger(owner) ||
        owner <= 0 || owner > 0xffffffff || String(owner) !== match[1]) {
      if (current() && (!request || !this.adapter.assignedElsewhere?.(actor, request.userId))) {
        this.quarantine(actor);
        this.reconcile();
      }
      throw new Error("Saved instance needs administrator recovery");
    }
    const areaId = match[2];
    const authorizedOwner = owner === actor ||
      (this.sameParty(actor, owner) && this.hostAvailable(owner, areaId)) ? owner : actor;
    try {
      if (request && typeof this.adapter.resumeOwner === "function") {
        if (!Number.isInteger(request.userId) || request.userId < 0 ||
            request.userId > 0xffffffff || request.handled !== false)
          throw new Error("Invalid campaign spawn request");
        // Only native admission can distinguish an unenrolled character from
        // an invalid enrolled context. The latter must never be rebound here.
        const resumed = this.adapter.resumeOwner(actor, this.areas.get(areaId), request.userId);
        if (!current()) throw new Error("Campaign spawn connection was replaced");
        if (resumed !== null) {
          const personal = this.key(actor, areaId);
          if (!resumed || resumed.actor !== actor || resumed.phase !== personal ||
              this.adapter.getInstance(actor) !== personal || typeof resumed.attempt !== "string" ||
              !resumed.attempt || !["Pending", "ProjectionReady"].includes(resumed.status))
            throw new Error("Campaign resume was not accepted");
          if (owner === actor) {
            this.visits.set(actor, { owner, area: areaId });
          } else {
            // Cold visitors return to their own campaign. Their visit becomes
            // visible to party commands after the native projection completes.
            this.visits.delete(actor);
            this.removeMembership(actor);
          }
          request.handled = true;
          return;
        }
      }
      if (this.adapter.isInside(actor, this.areas.get(areaId))) {
        this.transition(actor, authorizedOwner, areaId);
      } else this.transition(actor, null);
    } catch (error) {
      // The requesting socket can lose a race to the character's existing
      // connection. Only the requester should be closed in that case.
      if (current() && (!request || !this.adapter.assignedElsewhere?.(actor, request.userId)))
        this.quarantine(actor);
      throw error;
    } finally { if (current()) this.reconcile(); }
  }

  confirmPersonalResume(actor, phase) {
    const area = phase.slice(phase.lastIndexOf(":") + 1);
    if (!this.areas.has(area) || phase !== this.key(actor, area) ||
        this.adapter.getInstance(actor) !== phase)
      throw new Error("Completed recovery disagrees with the personal campaign");
    this.visits.set(actor, { owner: actor, area });
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
