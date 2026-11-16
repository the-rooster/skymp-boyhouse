import { Settings } from "../settings";
import { System, SystemContext, Content } from "./system";
import { PartyInstances } from "../campaign/partyInstances";

export class CampaignInstances implements System {
  systemName = "CampaignInstances";
  private authority: PartyInstances | undefined;
  private lastAction = new Map<number, number>();

  async initAsync(ctx: SystemContext): Promise<void> {
    const settings = await Settings.get();
    const config = settings.allSettings?.boyhouseInstances as
      { experimental?: boolean; areas?: unknown[] } | undefined;
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
      setInstance: (id: number, key: string) => api.set(id, "worldInstance", key),
      disable: (id: number) => {
        // Disabled objects are hidden, but native packet handlers still resolve
        // an assigned actor. Remove that authority before closing the socket.
        const user = ctx.svr.getUserByActor(id);
        const connected = ctx.svr.isConnected(user);
        if (connected) ctx.svr.setUserActor(user, 0);
        try { api.setEnabled(id, false); }
        finally { if (connected) ctx.svr.kick(user); }
      },
      isInside: (id: number, area: { worldOrCell: number; center: number[]; radius: number }) => {
        const pos = ctx.svr.getActorPos(id);
        return ctx.svr.getActorCellOrWorld(id) === area.worldOrCell &&
          pos.reduce((sum: number, n: number, i: number) => sum + (n - area.center[i]) ** 2, 0) <= area.radius ** 2;
      },
    }, config.areas ?? []);
    // Spawn emits this before enabling or subscribing a saved character.
    ctx.gm.on("beforeSpawn", (actor: number) => this.authority?.recover(actor));
  }

  async updateAsync(ctx: SystemContext): Promise<void> {
    await new Promise(resolve => setTimeout(resolve, 250));
    for (const actor of this.authority?.reconcile() ?? []) this.sendState(actor, ctx);
  }

  private sendState(actor: number, ctx: SystemContext): void {
    const user = ctx.svr.getUserByActor(actor);
    if (ctx.svr.isConnected(user)) ctx.svr.sendCustomPacket(user, JSON.stringify({
      customPacketType: "boyhouse.instance", state: this.authority?.snapshot(actor),
    }));
  }

  customPacket(userId: number, type: string, content: Content, ctx: SystemContext): void {
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
    this.lastAction.delete(userId);
    const actor = ctx.svr.getUserActor(userId);
    if (actor && this.authority) {
      const members = this.authority.snapshot(actor).members;
      this.authority.leave(actor);
      for (const member of members) if (member !== actor) this.sendState(member, ctx);
    }
  }
}
