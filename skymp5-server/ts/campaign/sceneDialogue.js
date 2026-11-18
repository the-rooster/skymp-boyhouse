"use strict";

class CampaignSceneDialogue {
  constructor(projection, authority, adapter) {
    this.projection = projection;
    this.authority = authority;
    this.adapter = adapter;
    this.active = new WeakMap();
  }

  view(actor, owner) {
    if (!this.authority.canObserveScene(actor, owner)) return null;
    const result = this.projection.releasedProjection(this.adapter.user(actor));
    return result?.manifest.actor === actor && result.manifest.campaignOwner === owner ? result : null;
  }

  // The trusted quest/scene driver owns pacing. An incoming client packet can
  // neither create this handle nor advance it. A fresh projection invalidates it.
  begin(owner, questId, sceneId, holdLastLine = false) {
    const hostUser = this.adapter.user(owner);
    const host = this.view(owner, owner);
    if (!host) throw Error("Scene owner has no acknowledged private projection");
    if (this.active.has(host.release)) throw Error("A dialogue scene is already active");
    const read = () => {
      const { scenePlan, blocker } = this.adapter.plan(owner, questId, sceneId);
      if (!scenePlan) throw Error(blocker ? blocker.code + ": " + blocker.detail : "Missing dialogue plan");
      if (scenePlan.questId !== questId || scenePlan.sceneId !== sceneId || !scenePlan.lines.length)
        throw Error("Invalid dialogue plan");
      return scenePlan;
    };
    const plan = read();
    const original = JSON.stringify(plan);
    const run = {};
    this.active.set(host.release, run);
    let next = 0;
    let publishing = false;
    const cancel = () => {
      if (this.active.get(host.release) === run) this.active.delete(host.release);
    };
    const assertCurrent = () => {
      if (this.active.get(host.release) !== run || this.view(owner, owner)?.release !== host.release ||
          JSON.stringify(read()) !== original) throw Error("Dialogue campaign or quest bindings changed");
    };
    return {
      assertCurrent,
      get topicIds() { return plan.lines.map(line => line.topicId); },
      get nextTopicId() { return plan.lines[next]?.topicId ?? null; },
      get nextActionIndex() { return plan.lines[next]?.index ?? null; },
      cancel,
      advance: (expectedIndex, slotMs) => {
        if (slotMs !== undefined && (!Number.isSafeInteger(slotMs) || slotMs <= 0))
          throw Error("Invalid dialogue slot duration");
        const line = plan.lines[next];
        if (!line || this.active.get(host.release) !== run || publishing || expectedIndex !== line.index)
          throw Error("Dialogue action is no longer pending");
        publishing = true;
        try {
          assertCurrent();
          const actors = new Set([owner, ...this.authority.snapshot(owner).members]);
          let delivered = 0;
          for (const actor of actors) {
            if (this.active.get(host.release) !== run || !this.projection.isReleased(hostUser, host.release))
              throw Error("Dialogue owner changed while publishing");
            const target = this.view(actor, owner);
            const speaker = target?.manifest.references.find(ref => ref.original === line.speakerFormId);
            if (!target || target.manifest.identity !== host.manifest.identity ||
                speaker?.kind !== "actor" || speaker.disabled || speaker.deleted || speaker.copy !== line.speakerCopyId) {
              if (actor === owner) throw Error("Scene speaker is absent from the owner's projection");
              continue;
            }
            this.projection.sendSceneLine(this.adapter.user(actor), target.release, sceneId, line, slotMs);
            ++delivered;
          }
          ++next;
          if (next === plan.lines.length && !holdLastLine) cancel();
          return delivered;
        } catch (error) {
          // A transport may have delivered to a subset already.
          cancel();
          throw error;
        } finally {
          publishing = false;
        }
      },
    };
  }
}

module.exports = { CampaignSceneDialogue };
