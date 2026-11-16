export interface ProjectedReference {
    original: number;
    copy: number;
    baseId: number;
    kind: "actor" | "object";
    disabled: boolean;
    deleted: boolean;
}

export interface WorldProjection {
    schemaVersion: 1;
    actor: number;
    campaignOwner: number;
    instance: string;
    contextEpoch: string;
    identity: string;
    references: ProjectedReference[];
}

export interface WorldBaseline {
    schemaVersion: 1;
    actor: number;
    packetEpoch: string;
    messages: Record<string, any>[];
}

const object = (v: any): boolean => v !== null && typeof v === "object" && !Array.isArray(v);
const integer = (v: any, min: number, max: number): boolean => Number.isSafeInteger(v) && v >= min && v <= max;
const token = (v: any): boolean => typeof v === "string" && /^[a-f0-9]{32}$/.test(v);
const baselineChunkSize = 16384;

function validBaseline(b: any, actor: number, epoch: string): b is WorldBaseline {
    if (!object(b) || Object.keys(b).length !== 4 || b.schemaVersion !== 1 || b.actor !== actor ||
        b.packetEpoch !== epoch || !Array.isArray(b.messages) || b.messages.length < 2 ||
        b.messages.length > 8192 || b.messages[0]?.t !== 32) return false;
    const gamemode = b.messages[0];
    if (![gamemode.eventSources, gamemode.updateOwnerFunctions, gamemode.updateNeighborFunctions].every(
        list => Array.isArray(list) && list.every(pair => object(pair) &&
            typeof pair.name === "string" && typeof pair.content === "string"))) return false;
    const ids = new Set<number>(), refs = new Set<number>(), hosts = new Set<number>();
    let self = 0, hosting = false;
    for (let i = 1; i < b.messages.length; ++i) {
        const m = b.messages[i];
        if (!object(m)) return false;
        if (m.t === 26) {
            hosting = true;
            if (!refs.has(m.target) || m.target === actor || hosts.has(m.target)) return false;
            hosts.add(m.target);
        } else if (m.t === 33) {
            if (hosting || !integer(m.idx, 0, 0xffffffff) || ids.has(m.idx) ||
                !integer(m.refrId, 1, 0x1feffffff) || refs.has(m.refrId) ||
                typeof m.isMe !== "boolean" || !object(m.transform) ||
                !integer(m.transform.worldOrCell, 1, 0xffffffff) ||
                ![m.transform.pos, m.transform.rot].every(v => Array.isArray(v) && v.length === 3 && v.every(Number.isFinite)) ||
                !object(m.props) || !Array.isArray(m.customPropsJsonDumps) ||
                m.customPropsJsonDumps.some((p: any) => !object(p) || typeof p.propName !== "string" ||
                    p.propName.startsWith("private.") || typeof p.propValueJsonDump !== "string")) return false;
            ids.add(m.idx); refs.add(m.refrId);
            if (m.isMe) {
                if (i !== 1 || m.refrId !== actor || !object(m.props.inventory) ||
                    !Array.isArray(m.props.inventory.entries) || typeof m.props.isRaceMenuOpen !== "boolean") return false;
                ++self;
            }
        } else return false; // Baselines never carry snippets, custom commands or input.
    }
    return self === 1 && ids.size <= 4096;
}

function validMetadata(m: any): boolean {
    if (!object(m) || Object.keys(m).length !== 6 || m.schemaVersion !== 1 || !integer(m.actor, 0xff000000, 0xffffffff) ||
        !integer(m.campaignOwner, 0xff000000, 0xffffffff) || typeof m.instance !== "string" ||
        typeof m.contextEpoch !== "string" || !/^\d{1,20}:\d{1,20}$/.test(m.contextEpoch) ||
        (m.identity !== "" && !token(m.identity))) return false;
    return m.instance === "" ? m.campaignOwner === m.actor && m.identity === "" :
        new RegExp(`^campaign:${m.campaignOwner}:[a-zA-Z0-9_-]{1,48}$`).test(m.instance);
}

function validReference(r: any): r is ProjectedReference {
    return object(r) && Object.keys(r).length === 6 && integer(r.original, 1, 0xfeffffff) && r.original !== 0x14 &&
        integer(r.copy, 0xff000000, 0xffffffff) && integer(r.baseId, 1, 0xfeffffff) &&
        (r.kind === "actor" || r.kind === "object") &&
        typeof r.disabled === "boolean" && typeof r.deleted === "boolean";
}

// Network assembly only. The engine adapter must finish applying `prepared()`
// before calling applied(); receipt of a packet is never an acknowledgement.
export class WorldProjectionReplica {
    private session = "";
    private sequence = 0;
    private pending: { token: string; metadata: Omit<WorldProjection, "references">;
        count: number; chunks: number; parts: Map<number, ProjectedReference[]>; applied: boolean;
        baselineLength: number; baselineChunks: number; baselineParts: Map<number, string>;
        baseline?: WorldBaseline } | undefined;
    private current: WorldProjection | undefined;
    private released = false;
    private epoch = "";

    begin(sessionId: string): void {
        this.session = sessionId;
        this.sequence = 0;
        this.pending = undefined;
        this.current = undefined;
        this.released = false;
        this.epoch = "";
    }

    get ready(): boolean { return this.released; }
    get version(): number { return this.sequence; }
    get packetEpoch(): string { return this.epoch; }
    snapshot(): WorldProjection | undefined { return this.current ? JSON.parse(JSON.stringify(this.current)) : undefined; }

    accept(packet: any): boolean {
        if (!this.session || !object(packet) || packet.customPacketType !== "boyhouse.projection" ||
            packet.protocolVersion !== 2 ||
            packet.sessionId !== this.session || !integer(packet.sequence, 1, Number.MAX_SAFE_INTEGER) ||
            packet.sequence < this.sequence) return false;
        if (packet.kind === "pause") {
            if (packet.sequence <= this.sequence) return false;
            this.sequence = packet.sequence;
            this.pending = undefined;
            this.released = false;
            this.epoch = "";
            return true;
        }
        if (packet.kind === "begin") {
            if (packet.sequence <= this.sequence || !token(packet.token) || !validMetadata(packet.metadata) ||
                !integer(packet.count, 0, 4096) || packet.chunks !== Math.ceil(packet.count / 128) ||
                !integer(packet.baselineLength, 1, 2 * 1024 * 1024) ||
                packet.baselineChunks !== Math.ceil(packet.baselineLength / baselineChunkSize) ||
                (packet.count > 0 && (!packet.metadata.instance || !packet.metadata.identity))) return false;
            this.sequence = packet.sequence;
            this.pending = { token: packet.token, metadata: JSON.parse(JSON.stringify(packet.metadata)),
                count: packet.count, chunks: packet.chunks, parts: new Map(), applied: false,
                baselineLength: packet.baselineLength, baselineChunks: packet.baselineChunks, baselineParts: new Map() };
            this.epoch = packet.token;
            this.released = false;
            return true;
        }
        const pending = this.pending;
        if (!pending || packet.sequence !== this.sequence || packet.token !== pending.token) return false;
        if (packet.kind === "baseline") {
            if (pending.applied || !integer(packet.index, 0, pending.baselineChunks - 1) ||
                typeof packet.data !== "string" || packet.data.length !==
                    Math.min(baselineChunkSize, pending.baselineLength - packet.index * baselineChunkSize)) return false;
            const previous = pending.baselineParts.get(packet.index);
            if (previous !== undefined) return previous === packet.data;
            const parts = new Map(pending.baselineParts); parts.set(packet.index, packet.data);
            if (parts.size === pending.baselineChunks) {
                const text: string[] = [];
                for (let i = 0; i < pending.baselineChunks; ++i) text.push(parts.get(i)!);
                let baseline: unknown;
                try { baseline = JSON.parse(text.join("")); } catch { return false; }
                if (!validBaseline(baseline, pending.metadata.actor, pending.token)) return false;
                pending.baseline = baseline;
            }
            pending.baselineParts = parts;
            return true;
        }
        if (packet.kind === "chunk") {
            if (pending.applied || !integer(packet.index, 0, pending.chunks - 1) ||
                !Array.isArray(packet.references) || packet.references.length !==
                    Math.min(128, pending.count - packet.index * 128) ||
                !packet.references.every(validReference)) return false;
            const originals = new Set<number>(), copies = new Set<number>();
            const parts = Array.from(pending.parts.entries()).filter(([index]) => index !== packet.index)
                .map(([, refs]) => refs).concat([packet.references]);
            for (const refs of parts) for (const ref of refs) {
                if (originals.has(ref.original) || copies.has(ref.copy) ||
                    ref.copy === pending.metadata.actor || ref.copy === pending.metadata.campaignOwner) return false;
                originals.add(ref.original); copies.add(ref.copy);
            }
            const previous = pending.parts.get(packet.index);
            if (previous) return JSON.stringify(previous) === JSON.stringify(packet.references);
            pending.parts.set(packet.index, JSON.parse(JSON.stringify(packet.references)));
            return true;
        }
        if (packet.kind === "ready" && pending.applied) {
            this.released = true;
            this.pending = undefined;
            return true;
        }
        return false;
    }

    prepared(): WorldProjection | undefined {
        const p = this.pending;
        if (!p || p.applied || !p.baseline || p.parts.size !== p.chunks) return undefined;
        const references: ProjectedReference[] = [];
        for (let index = 0; index < p.chunks; ++index) references.push(...p.parts.get(index)!);
        return JSON.parse(JSON.stringify({ ...p.metadata, references }));
    }

    preparedBaseline(): WorldBaseline | undefined {
        return this.prepared() ? JSON.parse(JSON.stringify(this.pending!.baseline)) : undefined;
    }

    applied(version: number): Record<string, unknown> | undefined {
        const next = this.prepared();
        if (!next || version !== this.sequence || !this.pending) return undefined;
        this.current = next;
        this.pending.applied = true;
        return { customPacketType: "boyhouse.projection", protocolVersion: 2, action: "ack", sessionId: this.session,
            sequence: this.sequence, token: this.pending.token };
    }

    failed(version: number): Record<string, unknown> | undefined {
        if (!this.pending || version !== this.sequence) return undefined;
        return { customPacketType: "boyhouse.projection", protocolVersion: 2, action: "failed", sessionId: this.session,
            sequence: this.sequence, token: this.pending.token };
    }
}
