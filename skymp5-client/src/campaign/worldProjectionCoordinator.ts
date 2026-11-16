import { WorldBaseline, WorldProjection, WorldProjectionReplica } from './worldProjection';

export interface ProjectionProgress { ready: boolean; error?: string; }
export interface ProjectionEngine {
    cancel(): void;
    drained(): ProjectionProgress;
    apply(projection: WorldProjection, baseline: WorldBaseline): void;
    observe(): ProjectionProgress;
    release(): void;
    disconnect(): void;
}
export interface ProjectionTransport {
    block(epoch?: string): void;
    send(message: Record<string, unknown>): void;
    drain(epoch: string): boolean;
    release(epoch: string): boolean;
    close(error: string): void;
}
type Phase = 'disconnected' | 'receiving' | 'draining' | 'applying' | 'awaiting-ready' | 'releasing' | 'ready' | 'failed';

// Protocol and engine progress are independent. A received baseline is not an
// applied baseline, and an applied baseline is not permission to resume input.
export class WorldProjectionCoordinator {
    readonly replica = new WorldProjectionReplica();
    private phase: Phase = 'disconnected';
    private generation = 0;
    private deadline = 0;
    private prepared?: { projection: WorldProjection; baseline: WorldBaseline };
    private error?: string;
    private engine: ProjectionEngine;
    private transport: ProjectionTransport;
    private now: () => number;

    constructor(engine: ProjectionEngine, transport: ProjectionTransport, now = () => Date.now()) {
        this.engine = engine; this.transport = transport; this.now = now;
    }

    get status(): { phase: Phase; version: number; generation: number; error?: string } {
        return { phase: this.phase, version: this.replica.version, generation: this.generation, error: this.error };
    }

    connect(session: string): void {
        ++this.generation; this.replica.begin(session); this.prepared = undefined; this.error = undefined;
        this.phase = 'receiving'; this.deadline = this.now() + 30000;
        try {
            this.transport.block(); this.engine.cancel();
            this.transport.send({ customPacketType: 'boyhouse.projection', protocolVersion: 2,
                action: 'hello', sessionId: session });
        } catch (error) { this.fail(error); }
    }

    disconnect(): void {
        ++this.generation; this.phase = 'disconnected'; this.prepared = undefined; this.deadline = 0;
        this.replica.begin('');
        this.engine.disconnect();
    }

    receive(packet: unknown): boolean {
        if (this.phase === 'disconnected' || this.phase === 'failed' || !this.replica.accept(packet)) return false;
        try {
            const kind = (packet as { kind: string }).kind;
            if (kind === 'pause' || kind === 'begin') {
                ++this.generation;
                if (!this.deadline) this.deadline = this.now() + 30000;
                this.prepared = undefined; this.phase = 'receiving';
                this.transport.block(this.replica.packetEpoch || undefined); this.engine.cancel();
            } else if (kind === 'ready') this.phase = 'releasing';
            if (this.phase === 'receiving') {
                const projection = this.replica.prepared();
                const baseline = projection && this.replica.preparedBaseline();
                if (projection && baseline) { this.prepared = { projection, baseline }; this.phase = 'draining'; }
            }
            return true;
        } catch (error) { this.fail(error); return false; }
    }

    advance(nativeUpdate: boolean): void {
        if (this.phase === 'disconnected' || this.phase === 'failed' || this.phase === 'ready') return;
        const generation = this.generation, version = this.replica.version;
        const current = () => generation === this.generation && version === this.replica.version;
        try {
            if (this.now() >= this.deadline) throw Error('Campaign projection transition timed out');
            if (this.phase === 'draining' && this.prepared) {
                const progress = this.engine.drained();
                if (!current()) return;
                if (progress.error) throw Error(progress.error);
                if (!progress.ready) return;
                // Dispatch is safe before a gameplay update exists; the self
                // baseline asks PlayerSpawnService to bootstrap a generated save.
                const { projection, baseline } = this.prepared;
                this.phase = 'applying'; this.engine.apply(projection, baseline);
                return;
            }
            if (!nativeUpdate) return;
            if (this.phase === 'applying') {
                const progress = this.engine.observe();
                if (!current()) return;
                if (progress.error) throw Error(progress.error);
                if (!progress.ready) return;
                const ack = this.replica.applied(version);
                if (!ack) throw Error('Applied projection no longer matches the current transfer');
                this.phase = 'awaiting-ready'; this.transport.send(ack);
            } else if (this.phase === 'releasing') {
                const epoch = this.replica.packetEpoch;
                const drained = this.transport.drain(epoch);
                if (!current()) return;
                if (!drained) throw Error('Campaign replication could not be drained');
                const progress = this.engine.observe();
                if (!current()) return;
                if (progress.error) throw Error(progress.error);
                if (!progress.ready) return;
                const released = this.transport.release(epoch);
                if (!current()) return;
                if (!released) return;
                this.engine.release();
                if (!current()) return;
                this.phase = 'ready'; this.deadline = 0; this.prepared = undefined;
            }
        } catch (error) { if (current()) this.fail(error); }
    }

    private fail(error: unknown): void {
        if (this.phase === 'failed' || this.phase === 'disconnected') return;
        this.error = String(error); this.phase = 'failed'; ++this.generation;
        const failed = this.replica.failed(this.replica.version);
        try { if (failed) this.transport.send(failed); } catch { /* Close below even if reporting fails. */ }
        try { this.transport.block(); } finally {
            try { this.engine.cancel(); } finally { this.transport.close(this.error); }
        }
    }
}
