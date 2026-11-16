import { Settings } from "../settings";
import { Content, System, SystemContext } from "./system";
import { QuestJournalFeed } from "../campaign/questJournalFeed";

export class CampaignQuests implements System {
  systemName = "CampaignQuests";
  private feed: QuestJournalFeed | undefined;
  private lastRequest = new Map<number, number>();

  async initAsync(ctx: SystemContext): Promise<void> {
    const settings = await Settings.get();
    if ((settings.allSettings?.boyhouseInstances as { experimental?: boolean })?.experimental !== true) return;
    const api = ctx.svr as any;
    this.feed = new QuestJournalFeed({
      connected: (user: number) => ctx.svr.isConnected(user),
      actor: (user: number) => ctx.svr.getUserActor(user),
      read: (actor: number) => api.get(actor, "questJournal"),
      send: (user: number, packet: unknown) => ctx.svr.sendCustomPacket(user, JSON.stringify(packet)),
    });
  }

  async updateAsync(): Promise<void> {
    await new Promise(resolve => setTimeout(resolve, 250));
    this.feed?.poll();
  }

  customPacket(userId: number, type: string, content: Content): void {
    if (type !== "boyhouse.questJournal" || !this.feed) return;
    const now = Date.now();
    if (now - (this.lastRequest.get(userId) ?? 0) < 200) return;
    this.lastRequest.set(userId, now);
    this.feed.subscribe(userId, content);
  }

  disconnect(userId: number): void {
    this.lastRequest.delete(userId);
    this.feed?.disconnect(userId);
  }
}
