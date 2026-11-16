export interface QuestProgress {
    stage: number;
    stages: number[];
    running: boolean;
    started: boolean;
    completed: boolean;
    active: boolean;
    objectives: Record<string, { displayed: boolean; completed: boolean; failed: boolean }>;
}

export interface CampaignJournal {
    schemaVersion: 1;
    revision: number;
    actor: number;
    campaignOwner: number;
    instance: string;
    contextEpoch: string;
    quests: Record<string, QuestProgress>;
}

const object = (value: any): boolean => value !== null && typeof value === "object" && !Array.isArray(value);
const integer = (value: any, max = Number.MAX_SAFE_INTEGER): boolean => Number.isSafeInteger(value) && value >= 0 && value <= max;

function validJournal(value: any): value is CampaignJournal {
    if (!object(value) || value.schemaVersion !== 1 || !integer(value.revision) ||
        !integer(value.actor, 0xffffffff) || !value.actor || !integer(value.campaignOwner, 0xffffffff) || !value.campaignOwner ||
        typeof value.instance !== "string" || value.instance.length > 128 ||
        typeof value.contextEpoch !== "string" || !/^\d{1,20}:\d{1,20}$/.test(value.contextEpoch) ||
        !object(value.quests) || Object.keys(value.quests).length > 10000) return false;
    for (const [key, q] of Object.entries(value.quests) as [string, any][]) {
        if (!key.length || key.length > 300 || !object(q) || !integer(q.stage, 65535) ||
            !Array.isArray(q.stages) || q.stages.length > 65536 || !object(q.objectives) ||
            ![q.running, q.started, q.completed, q.active].every(v => typeof v === "boolean")) return false;
        let highest = 0;
        const visited = new Set<number>();
        for (const stage of q.stages) {
            if (!integer(stage, 65535) || visited.has(stage)) return false;
            visited.add(stage);
            highest = Math.max(highest, stage);
        }
        if (q.stage !== highest) return false;
        for (const [index, objective] of Object.entries(q.objectives) as [string, any][]) {
            if (!integer(Number(index), 65535) || String(Number(index)) !== index || !object(objective) ||
                ![objective.displayed, objective.completed, objective.failed].every(v => typeof v === "boolean") ||
                (objective.completed && objective.failed)) return false;
        }
    }
    return true;
}

// A projection for our UI, never a call to Skyrim SetStage: that would execute
// client fragments and duplicate effects outside server authority.
export class QuestJournalReplica {
    private subscription = "";
    private sequence = 0;
    private journal: CampaignJournal | undefined;
    error = "Not connected";

    begin(subscriptionId: string): void {
        this.subscription = subscriptionId;
        this.sequence = 0;
        this.journal = undefined;
        this.error = "Waiting for campaign journal";
    }

    clear(): void {
        this.begin("");
        this.error = "Not connected";
    }

    snapshot(): CampaignJournal | undefined {
        return this.journal ? JSON.parse(JSON.stringify(this.journal)) : undefined;
    }

    accept(packet: any): boolean {
        if (!this.subscription || !object(packet) || packet.customPacketType !== "boyhouse.questJournal" ||
            packet.subscriptionId !== this.subscription || !integer(packet.sequence) || packet.sequence <= this.sequence) return false;
        if (packet.ok === false && typeof packet.error === "string") {
            this.journal = undefined;
            this.error = packet.error;
        } else if (packet.ok === true && validJournal(packet.journal)) {
            const previous = this.journal, next = packet.journal;
            if (previous && previous.actor === next.actor && previous.campaignOwner === next.campaignOwner &&
                previous.instance === next.instance && previous.contextEpoch === next.contextEpoch && next.revision < previous.revision) return false;
            this.journal = JSON.parse(JSON.stringify(next));
            this.error = "";
        } else return false;
        this.sequence = packet.sequence;
        return true;
    }
}
