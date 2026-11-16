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
      setInstance: (id: number, key: string) => api.set(id, "worldInstance", key),
      disable: (id: number) => api.setEnabled(id, false),
      isInside: (id: number, area: { worldOrCell: number; center: number[]; radius: number }) => {
        const pos = ctx.svr.getActorPos(id);
        return ctx.svr.getActorCellOrWorld(id) === area.worldOrCell &&
          pos.reduce((sum: number, n: number, i: number) => sum + (n - area.center[i]) ** 2, 0) <= area.radius ** 2;
      },
    }, config.areas ?? []);
    // Spawn emits this before enabling or subscribing a saved character.
    ctx.gm.on("beforeSpawn", (actor: number) => this.authority?.recover(actor));
  }

  customPacket(userId: number, type: string, content: Content, ctx: SystemContext): void {
    if (type !== "boyhouse.instance" || !this.authority) return;
    const actor = ctx.svr.getUserActor(userId);
    if (!actor) return;
    const now = Date.now();
    if (now - (this.lastAction.get(userId) ?? 0) < 200) return;
    this.lastAction.set(userId, now);
    const reply = (body: Record<string, unknown>) =>
      ctx.svr.sendCustomPacket(userId, JSON.stringify({ customPacketType: "boyhouse.instance", ...body }));
    const previousMembers = this.authority.snapshot(actor).members;
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
      reply({ ok: true, state: this.authority.snapshot(actor) });
      // Notify party members and the invited character without trusting any
      // client-supplied sender ID.
      const recipients = new Set<number>([...previousMembers, ...this.authority.snapshot(actor).members]);
      if (content.action === "invite" || content.action === "kick") recipients.add(content.target);
      for (const recipient of recipients) {
        if (recipient === actor) continue;
        const user = ctx.svr.getUserByActor(recipient);
        if (ctx.svr.isConnected(user)) ctx.svr.sendCustomPacket(user, JSON.stringify({
          customPacketType: "boyhouse.instance", state: this.authority.snapshot(recipient),
        }));
      }
    } catch (error) {
      reply({ ok: false, error: error instanceof Error ? error.message : "Instance action failed" });
    }
  }

  disconnect(userId: number, ctx: SystemContext): void {
    this.lastAction.delete(userId);
    const actor = ctx.svr.getUserActor(userId);
    if (actor) this.authority?.leave(actor);
  }
}
