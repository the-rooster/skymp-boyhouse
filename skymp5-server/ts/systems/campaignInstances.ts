import { Settings } from "../settings";
import { System, SystemContext, Content } from "./system";
import { PartyInstances } from "../campaign/partyInstances";
import { ProjectionFeed } from "../campaign/projectionFeed";

export class CampaignInstances implements System {
  systemName = "CampaignInstances";
  private authority: PartyInstances | undefined;
  private projection: ProjectionFeed | undefined;
  private lastAction = new Map<number, number>();

  async initAsync(ctx: SystemContext): Promise<void> {
    const settings = await Settings.get();
    const config = settings.allSettings?.boyhouseInstances as
      { experimental?: boolean; projection?: boolean; areas?: unknown[] } | undefined;
    if (config?.experimental !== true) return;
    const api = ctx.svr as any;
    this.authority = new PartyInstances({
      exists: (id: number) => {
        try { return api.get(id, "type") === "MpActor"; } catch { return false; }
      },
      getInstance: (id: number) => api.get(id, "worldInstance"),
      prepareInstance: (id: number, area: { id: string; worldOrCell: number; center: number[]; radius: number; encounterLevel: number }) =>
        api.set(id, "campaignReferences", {
          instanceArea: area.id, encounterLevel: area.encounterLevel,
          area: { worldOrCell: area.worldOrCell, center: area.center, radius: area.radius },
        }),
      resumeOwner: config.projection === true ? (actor: number, area: { id: string; worldOrCell: number; center: number[]; radius: number; encounterLevel: number }, user: number) => {
        return ctx.svr.resumeCampaignActor(user, actor, api.get(actor, "worldInstance"), {
          id: area.id, worldOrCell: area.worldOrCell, center: [...area.center], radius: area.radius, encounterLevel: area.encounterLevel,
        });
      } : undefined,
      assignedElsewhere: (actor: number, requester: number) => {
        const assigned = ctx.svr.getUserByActor(actor);
        return assigned !== requester && ctx.svr.isConnected(assigned);
      },
      setInstance: (id: number, key: string) => {
        this.projection?.pauseActor(id);
        api.set(id, "worldInstance", key);
      },
      isReady: (id: number) => {
        if (!this.projection) return true;
        const user = ctx.svr.getUserByActor(id);
        return ctx.svr.isConnected(user) && !ctx.svr.getGameplayBlocked(user);
      },
      disable: (id: number) => {
        // Disabled objects are hidden, but native packet handlers still resolve
        // an assigned actor. Remove that authority before closing the socket.
        const user = ctx.svr.getUserByActor(id);
        const connected = ctx.svr.isConnected(user);
        const current = this.projection?.captureConnection(user);
        try {
          if (!connected || !ctx.svr.detachCampaignActor(user)) {
            if (connected) ctx.svr.setUserActor(user, 0);
            api.setEnabled(id, false);
          }
        }
        finally { if (connected && (!current || current())) ctx.svr.kick(user); }
      },
      isInside: (id: number, area: { worldOrCell: number; center: number[]; radius: number }) => {
        const pos = ctx.svr.getActorPos(id);
        return ctx.svr.getActorCellOrWorld(id) === area.worldOrCell &&
          pos.reduce((sum: number, n: number, i: number) => sum + (n - area.center[i]) ** 2, 0) <= area.radius ** 2;
      },
    }, config.areas ?? []);
    if (config.projection === true) this.projection = new ProjectionFeed({
      connected: (user: number) => ctx.svr.isConnected(user),
      actor: (user: number) => ctx.svr.getUserActor(user),
      instance: (actor: number) => api.get(actor, "worldInstance"),
      read: (actor: number) => api.get(actor, "campaignReferences"),
      beginEpoch: (user: number) => ctx.svr.beginPacketEpoch(user),
      epoch: (user: number) => ctx.svr.getPacketEpoch(user),
      baseline: (user: number, epoch: string) => ctx.svr.getWorldBaseline(user, epoch),
      resume: (user: number) => ctx.svr.getCampaignResume(user),
      captureResume: (user: number, epoch: string) => ctx.svr.captureCampaignResumeProjection(user, epoch),
      completeResume: (user: number, attempt: string, epoch: string, proof: string, publishReady: () => void) => {
        const resume = ctx.svr.getCampaignResume(user);
        if (!resume || resume.attempt !== attempt) return false;
        const accepted = ctx.svr.completeCampaignResume(user, attempt, epoch, proof, publishReady);
        if (accepted) this.authority!.confirmPersonalResume(resume.actor, resume.phase);
        return accepted;
      },
      block: (user: number, blocked: boolean) => ctx.svr.setGameplayBlocked(user, blocked),
      send: (user: number, packet: unknown) => ctx.svr.sendCustomPacket(user, JSON.stringify(packet)),
      close: (user: number, error: unknown) => {
        console.error("Campaign projection failed", user, error);
        const actor = ctx.svr.getUserActor(user);
        if (actor) {
          this.authority?.quarantine(actor);
          this.authority?.reconcile();
        } else ctx.svr.kick(user);
      },
    });
    if (this.projection) {
      const projection = this.projection;
      ctx.svr.setCampaignVisibilityCallbacks(
        (_owner, users) => projection.prepareVisibility(users),
        (_owner, users, synchronized) => projection.completeVisibility(users, synchronized)
      );
    }
    // Spawn emits this before enabling or subscribing a saved character.
    ctx.gm.on("beforeSpawn", (actor: number, request?: { userId: number; handled: boolean }) => {
      const current = request && this.projection?.captureConnection(request.userId);
      try { this.authority?.recover(actor, request, current); }
      catch (error) {
        // Resume validation can fail before native actor assignment. Close the
        // requesting connection even when quarantine cannot find it by actor.
        if (request && ctx.svr.isConnected(request.userId) && (!current || current())) ctx.svr.kick(request.userId);
        throw error;
      }
    });
  }

  connect(userId: number): void { this.projection?.connect(userId); }

  async updateAsync(ctx: SystemContext): Promise<void> {
    await new Promise(resolve => setTimeout(resolve, 250));
    for (const actor of this.authority?.reconcile() ?? []) this.sendState(actor, ctx);
    this.projection?.poll();
  }

  private sendState(actor: number, ctx: SystemContext): void {
    const user = ctx.svr.getUserByActor(actor);
    if (ctx.svr.isConnected(user)) ctx.svr.sendCustomPacket(user, JSON.stringify({
      customPacketType: "boyhouse.instance", state: this.authority?.snapshot(actor),
    }));
  }

  customPacket(userId: number, type: string, content: Content, ctx: SystemContext): void {
    if (type === "boyhouse.projection") { this.projection?.packet(userId, content); return; }
    if (type !== "boyhouse.instance" || !this.authority) return;
    const actor = ctx.svr.getUserActor(userId);
    if (!actor) return;
    const now = Date.now();
    if (now - (this.lastAction.get(userId) ?? 0) < 200) return;
    this.lastAction.set(userId, now);
    const reply = (body: Record<string, unknown>) => {
      if (ctx.svr.isConnected(userId)) ctx.svr.sendCustomPacket(userId,
        JSON.stringify({ customPacketType: "boyhouse.instance", ...body }));
    };
    const previousMembers = this.authority.snapshot(actor).members;
    let succeeded = false;
    try {
      switch (content.action) {
        case "invite": this.authority.invite(actor, content.target); break;
        case "accept": this.authority.accept(actor, content.leader); break;
        case "leave": this.authority.leave(actor); break;
        case "kick": this.authority.kick(actor, content.target); break;
        case "enter": this.authority.enter(actor, content.owner ?? actor, content.area); break;
        case "exit": this.authority.exit(actor); break;
        case "status": break;
        default: throw new Error("Unknown instance action");
      }
      succeeded = true;
      reply({ ok: true, state: this.authority.snapshot(actor) });
    } catch (error) {
      reply({ ok: false, error: error instanceof Error ? error.message : "Instance action failed" });
    }
    // Failure after native admission can revoke this character's visitors too.
    // Notify both previous and current party members after either outcome.
    const recipients = new Set<number>([...previousMembers, ...this.authority.snapshot(actor).members]);
    if (succeeded && (content.action === "invite" || content.action === "kick")) recipients.add(content.target);
    for (const recipient of recipients) if (recipient !== actor) this.sendState(recipient, ctx);
  }

  disconnect(userId: number, ctx: SystemContext): void {
    this.projection?.disconnect(userId);
    this.lastAction.delete(userId);
    const actor = ctx.svr.getUserActor(userId);
    if (actor && this.authority) {
      const members = this.authority.snapshot(actor).members;
      this.authority.leave(actor);
      for (const member of members) if (member !== actor) this.sendState(member, ctx);
    }
  }
}
