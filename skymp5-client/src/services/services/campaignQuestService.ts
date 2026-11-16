import { QuestJournalReplica } from "../../campaign/questJournalReplica";
import { MsgType } from "../../messages";
import { ClientListener, CombinedController, Sp } from "./clientListener";

export class CampaignQuestService extends ClientListener {
    readonly replica = new QuestJournalReplica();

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.emitter.on("connectionAccepted", () => {
            const subscriptionId = `${Date.now()}_${Math.random().toString(36).slice(2)}`;
            this.replica.begin(subscriptionId);
            controller.emitter.emit("sendMessage", {
                message: { t: MsgType.CustomPacket, contentJsonDump: JSON.stringify({
                    customPacketType: "boyhouse.questJournal", action: "subscribe", subscriptionId,
                }) }, reliability: "reliable",
            });
        });
        for (const event of ["connectionDisconnect", "connectionDenied", "connectionFailed"] as const)
            controller.emitter.on(event, () => this.replica.clear());
        controller.emitter.on("customPacketMessage", event => {
            try { this.replica.accept(JSON.parse(event.message.contentJsonDump)); }
            catch { /* Ignore invalid JSON; do not replace the current projection. */ }
        });
    }

    printJournal(): void {
        const journal = this.replica.snapshot();
        if (!journal) return this.sp.printConsole(`[Boyhouse] ${this.replica.error}`);
        this.sp.printConsole(`[Boyhouse] Campaign ${journal.campaignOwner}, revision ${journal.revision}, ${Object.keys(journal.quests).length} journal entries`);
        for (const [id, quest] of Object.entries(journal.quests))
            this.sp.printConsole(`${id}: ${JSON.stringify(quest)}`);
    }
}
