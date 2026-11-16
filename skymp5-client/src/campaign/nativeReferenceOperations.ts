export type NativeReferenceStep = () => void | Promise<void>;

export interface NativeReferenceJob {
    id: number;
    handle: number;
    world: number;
    epoch: number;
    key: string;
    persistent: boolean;
    status: 'queued' | 'running' | 'complete' | 'cancelled' | 'failed';
    error?: string;
    current(): boolean;
    steps: NativeReferenceStep[];
    step: number;
    complete?(): void;
    settled: boolean;
    onTerminal?(job: NativeReferenceJob): void;
}

interface Lane { handle: number; queued: NativeReferenceJob[]; active?: NativeReferenceJob; }

export interface NativeReferenceOperationState {
    version: 1;
    world: number;
    epoch: number;
    lanes: Map<number, Lane>;
    queue: number[];
    scheduled: Set<number>;
    cursor: number;
    errors: { id: number; epoch: number; key: string; error: string }[];
}

interface Adapter {
    identity(id: number): number;
    canStart(id: number, handle: number): boolean;
    report(job: NativeReferenceJob): void;
}

// Operations on authored references are borrowed, never owned or deleted.
// Native completions only settle records; all follow-up work runs on update.
export class NativeReferenceOperations {
    static createState(): NativeReferenceOperationState {
        return { version: 1, world: 0, epoch: 0, lanes: new Map(), queue: [], scheduled: new Set(), cursor: 0, errors: [] };
    }

    private adapter: Adapter;
    readonly state: NativeReferenceOperationState;

    constructor(adapter: Adapter, state = NativeReferenceOperations.createState()) {
        this.adapter = adapter; this.state = state;
    }

    submit(id: number, key: string, current: () => boolean, run: NativeReferenceStep | NativeReferenceStep[],
        complete?: () => void, persistent = false): NativeReferenceJob {
        if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff || !key || key.length > 128)
            throw Error('Invalid native reference operation');
        const steps = Array.isArray(run) ? [...run] : [run];
        if (!steps.length || steps.length > 8 || steps.some(step => typeof step !== 'function'))
            throw Error('Invalid native operation sequence');
        const handle = this.adapter.identity(id);
        if (!handle) throw Error('Native operation reference is absent');
        let lane = this.state.lanes.get(id);
        if (lane && lane.handle !== handle) {
            // An outstanding native call may still hold the old identity.
            // Observe its completion before starting work at a reused FormID.
            if (lane.active) throw Error('Previous reference identity is still busy');
            this.cancelLane(lane); this.state.lanes.delete(id); lane = undefined;
        }
        if (!lane) {
            if (this.state.lanes.size >= 4096) throw Error('Native reference operation limit exceeded');
            lane = { handle, queued: [] }; this.state.lanes.set(id, lane);
        }
        const job: NativeReferenceJob = { id, handle, key, current, steps, step: 0, complete, persistent,
            world: this.state.world, epoch: this.state.epoch, status: 'queued', settled: false };
        // Coalesce queued updates to one property. Never replace an active call.
        const previous = lane.queued.findIndex(item => item.key === key);
        if (previous >= 0) { this.cancel(lane.queued[previous]); lane.queued[previous] = job; }
        else {
            if (lane.queued.length >= 16) throw Error('Native reference queue limit exceeded');
            lane.queued.push(job);
        }
        this.schedule(id);
        return job;
    }

    cancelEpoch(): void {
        ++this.state.epoch;
        this.state.lanes.forEach((lane, id) => {
            lane.queued = lane.queued.filter(job => {
                if (job.persistent) return true;
                this.cancel(job); return false;
            });
            if (lane.active && !lane.active.persistent) lane.active.complete = undefined;
            this.schedule(id);
        });
    }

    forgetWorld(): void {
        ++this.state.world; ++this.state.epoch;
        this.state.lanes.forEach(lane => this.cancelLane(lane));
        this.state.lanes.clear(); this.state.queue = []; this.state.scheduled.clear(); this.state.cursor = 0;
        this.state.errors = [];
    }

    isBusy(id?: number, handle?: number): boolean {
        if (id === undefined) return this.state.lanes.size > 0;
        const lane = this.state.lanes.get(id);
        return !!lane && (handle === undefined || lane.handle === handle) &&
            (!!lane.active || lane.queued.length > 0);
    }

    update(budget = 16): void {
        if (!Number.isInteger(budget) || budget < 1 || budget > 64) throw Error('Invalid native operation budget');
        const world = this.state.world;
        const count = Math.min(budget, this.state.queue.length - this.state.cursor);
        for (let i = 0; i < count && world === this.state.world; ++i) {
            const id = this.state.queue[this.state.cursor++]; this.state.scheduled.delete(id);
            const lane = this.state.lanes.get(id);
            if (lane) this.advance(id, lane);
        }
        if (this.state.cursor > 1024 || this.state.cursor === this.state.queue.length) {
            this.state.queue = this.state.queue.slice(this.state.cursor); this.state.cursor = 0;
        }
    }

    private valid(job: NativeReferenceJob): boolean {
        if (job.world !== this.state.world || (!job.persistent && job.epoch !== this.state.epoch)) return false;
        try { return job.current(); }
        catch (error) { job.error = String(error); return false; }
    }

    private cancel(job: NativeReferenceJob): void {
        if (job.error !== undefined) {
            if (job.status !== 'failed') this.report(job);
            job.status = 'failed';
        } else job.status = 'cancelled';
        job.complete = undefined;
        this.notify(job);
    }

    private notify(job: NativeReferenceJob): void {
        const callback = job.onTerminal; job.onTerminal = undefined;
        callback?.(job);
    }

    private report(job: NativeReferenceJob): void {
        if (this.state.errors.length < 32) this.state.errors.push({
            id: job.id, epoch: job.epoch, key: job.key, error: job.error || 'Native operation failed',
        });
        this.adapter.report(job);
    }

    private cancelLane(lane: Lane): void {
        lane.queued.forEach(job => this.cancel(job));
        if (lane.active) this.cancel(lane.active);
    }

    private schedule(id: number): void {
        if (!this.state.scheduled.has(id)) { this.state.scheduled.add(id); this.state.queue.push(id); }
    }

    private advance(id: number, lane: Lane): void {
        const active = lane.active;
        if (active && !active.settled) return;
        if (active) {
            if (!active.error && this.valid(active) && this.adapter.identity(id) === active.handle &&
                active.step + 1 < active.steps.length) {
                ++active.step; this.start(active, lane); return;
            }
            lane.active = undefined;
            if (active.error !== undefined) {
                active.status = 'failed'; active.complete = undefined;
                this.report(active); this.notify(active);
            } else if (this.valid(active) && this.adapter.identity(id) === active.handle) {
                active.status = 'complete';
                const complete = active.complete; active.complete = undefined;
                try { complete?.(); } catch (error) {
                    active.error = String(error); active.status = 'failed'; this.report(active);
                }
                this.notify(active);
            } else this.cancel(active);
        }
        if (this.state.lanes.get(id) !== lane) return;
        while (lane.queued.length && !this.valid(lane.queued[0])) this.cancel(lane.queued.shift()!);
        if (!lane.queued.length) { this.state.lanes.delete(id); return; }
        if (this.adapter.identity(id) !== lane.handle) {
            this.cancelLane(lane); this.state.lanes.delete(id); return;
        }
        if (!this.adapter.canStart(id, lane.handle)) { this.schedule(id); return; }
        const job = lane.queued.shift()!;
        lane.active = job; this.start(job, lane);
    }

    private start(job: NativeReferenceJob, lane: Lane): void {
        const id = job.id;
        job.status = 'running'; job.settled = false;
        const finish = (error?: unknown) => {
            job.settled = true;
            if (error !== undefined) job.error = String(error);
            if (this.state.lanes.get(id) === lane) this.schedule(id);
        };
        try { Promise.resolve(job.steps[job.step]()).then(() => finish(), finish); }
        catch (error) { finish(error); }
    }
}
