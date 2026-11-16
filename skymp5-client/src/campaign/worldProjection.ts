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

const object = (v: any): boolean => v !== null && typeof v === "object" && !Array.isArray(v);
const integer = (v: any, min: number, max: number): boolean => Number.isSafeInteger(v) && v >= min && v <= max;
const token = (v: any): boolean => typeof v === "string" && /^[a-f0-9]{32}$/.test(v);

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
        count: number; chunks: number; parts: Map<number, ProjectedReference[]>; applied: boolean } | undefined;
    private current: WorldProjection | undefined;
    private released = false;

    begin(sessionId: string): void {
        this.session = sessionId;
        this.sequence = 0;
        this.pending = undefined;
        this.current = undefined;
        this.released = false;
    }

    get ready(): boolean { return this.released; }
    get version(): number { return this.sequence; }
    snapshot(): WorldProjection | undefined { return this.current ? JSON.parse(JSON.stringify(this.current)) : undefined; }

    accept(packet: any): boolean {
        if (!this.session || !object(packet) || packet.customPacketType !== "boyhouse.projection" ||
            packet.sessionId !== this.session || !integer(packet.sequence, 1, Number.MAX_SAFE_INTEGER) ||
            packet.sequence < this.sequence) return false;
        if (packet.kind === "pause") {
            if (packet.sequence <= this.sequence) return false;
            this.sequence = packet.sequence;
            this.pending = undefined;
            this.released = false;
            return true;
        }
        if (packet.kind === "begin") {
            if (packet.sequence <= this.sequence || !token(packet.token) || !validMetadata(packet.metadata) ||
                !integer(packet.count, 0, 4096) || packet.chunks !== Math.ceil(packet.count / 128) ||
                (packet.count > 0 && (!packet.metadata.instance || !packet.metadata.identity))) return false;
            this.sequence = packet.sequence;
            this.pending = { token: packet.token, metadata: JSON.parse(JSON.stringify(packet.metadata)),
                count: packet.count, chunks: packet.chunks, parts: new Map(), applied: false };
            this.released = false;
            return true;
        }
        const pending = this.pending;
        if (!pending || packet.sequence !== this.sequence || packet.token !== pending.token) return false;
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
        if (!p || p.applied || p.parts.size !== p.chunks) return undefined;
        const references: ProjectedReference[] = [];
        for (let index = 0; index < p.chunks; ++index) references.push(...p.parts.get(index)!);
        return JSON.parse(JSON.stringify({ ...p.metadata, references }));
    }

    applied(version: number): Record<string, unknown> | undefined {
        const next = this.prepared();
        if (!next || version !== this.sequence || !this.pending) return undefined;
        this.current = next;
        this.pending.applied = true;
        return { customPacketType: "boyhouse.projection", action: "ack", sessionId: this.session,
            sequence: this.sequence, token: this.pending.token };
    }

    failed(version: number): Record<string, unknown> | undefined {
        if (!this.pending || version !== this.sequence) return undefined;
        return { customPacketType: "boyhouse.projection", action: "failed", sessionId: this.session,
            sequence: this.sequence, token: this.pending.token };
    }
}
