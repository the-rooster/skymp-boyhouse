// Wire values match MsgType::CampaignFrame and MsgType::CustomPacket. This
// module stays independent of Skyrim Platform so transition races can be tested.
const FRAME = 34, CUSTOM = 1;
const validEpoch = (value: unknown): value is string => typeof value === "string" && /^[a-f0-9]{32}$/.test(value);
const object = (value: any): boolean => value !== null && typeof value === "object" && !Array.isArray(value);
const validMessage = (value: any): boolean => object(value) && Number.isInteger(value.t) && value.t >= 1 && value.t < FRAME;

export class CampaignPacketChannel {
    private active = false;
    private epoch = "";
    private released = false;
    private releasing = false;
    private generation = 0;
    private queued: any[] = [];
    private queuedBytes = 0;

    get enabled(): boolean { return this.active; }
    get ready(): boolean { return !this.active || this.released; }
    get currentEpoch(): string { return this.epoch; }

    // Called before login by a projection-enabled connection. Unknown epochs
    // cannot supply its initial world; the coordinator must request a baseline.
    requireEpoch(): void {
        this.reset(true);
    }

    clear(): void { this.reset(false); }

    private reset(active: boolean): void {
        ++this.generation;
        this.active = active;
        this.epoch = "";
        this.released = false;
        this.releasing = false;
        this.queued = [];
        this.queuedBytes = 0;
    }

    select(epoch: string): void {
        if (!validEpoch(epoch)) throw Error("Invalid campaign packet epoch");
        // Only the validated projection control path may select an epoch.
        this.reset(true);
        this.epoch = epoch;
    }

    private control(message: any): boolean {
        if (!object(message) || message.t !== CUSTOM || typeof message.contentJsonDump !== "string") return false;
        try {
            const content = JSON.parse(message.contentJsonDump);
            return object(content) && (content.customPacketType === "boyhouse.projection" ||
                content.customPacketType === "loginWithSkympIo");
        } catch { return false; }
    }

    outgoing(message: any): any | undefined {
        if (!this.active || this.control(message)) return message;
        if (!this.released) return undefined; // Never replay input after a pause.
        if (!validMessage(message)) throw Error("Invalid outgoing campaign message");
        return { t: FRAME, epoch: this.epoch, message };
    }

    outgoingRaw(raw: ArrayBuffer): any | undefined {
        if (!this.active || !this.released) return undefined;
        if (raw.byteLength < 2 || raw.byteLength > 1024 * 1024) throw Error("Invalid campaign packet size");
        const bytes = new Uint8Array(raw);
        if (bytes[0] !== 134 || bytes[1] === FRAME) throw Error("Invalid campaign raw packet");
        return { t: FRAME, epoch: this.epoch, raw: Array.from(bytes) };
    }

    incoming(message: any, deliver: (message: any) => void): void {
        if (!object(message)) return;
        if (message.t !== FRAME) {
            // Login is client-to-server only. Only projection control bypasses
            // the receive gate after an epoch-aware session begins.
            let projection = false;
            if (message.t === CUSTOM && typeof message.contentJsonDump === "string") {
                try { projection = JSON.parse(message.contentJsonDump)?.customPacketType === "boyhouse.projection"; }
                catch { /* Invalid custom content is not control traffic. */ }
            }
            if (!this.active || projection) deliver(message);
            return;
        }
        if (!this.active || !this.epoch || message.epoch !== this.epoch || !validMessage(message.message)) return;
        if (this.released) { deliver(message.message); return; }
        // Three bytes per JSON UTF-16 code unit bounds UTF-8 transport size as
        // well as the retained text. Count and size are independent limits.
        const encoded = JSON.stringify(message.message);
        const bytes = encoded.length * 3;
        if (this.queued.length >= 4096 || this.queuedBytes + bytes > 8 * 1024 * 1024) {
            this.requireEpoch();
            throw Error("Pending campaign replication exceeded its limit");
        }
        this.queued.push(JSON.parse(encoded));
        this.queuedBytes += bytes;
    }

    release(epoch: string, deliver: (message: any) => void): boolean {
        if (!this.active || !this.epoch || epoch !== this.epoch || this.released || this.releasing) return false;
        const generation = this.generation;
        const pending = this.queued;
        this.releasing = true;
        try {
            for (const message of pending) {
                // A delivered callback can disconnect or start another phase.
                if (generation !== this.generation) return false;
                deliver(message);
            }
        } catch (error) {
            if (generation === this.generation) this.requireEpoch();
            throw error;
        }
        if (generation !== this.generation) return false;
        this.queued = [];
        this.queuedBytes = 0;
        this.releasing = false;
        this.released = true;
        return true;
    }
}
