interface ReferenceWork {
    run(): void | Promise<void>;
    complete?(): void;
}

interface ReferenceOperation {
    work?: ReferenceWork;
    disposal: boolean;
    settled: boolean;
    error?: string;
}

export interface OwnedReference {
    readonly id: number;
    readonly handle: number;
    readonly generation: number;
    retired: boolean;
    work: ReferenceWork[];
    operation?: ReferenceOperation;
    error?: string;
    cleanupFailed?: boolean;
}

export interface OwnedReferenceState {
    version: 1;
    generation: number;
    references: Map<number, OwnedReference>;
    queue: number[];
    queued: Set<number>;
    cursor: number;
}

export interface OwnedReferenceAdapter {
    identity(id: number): number;
    dispose(id: number, handle: number): Promise<void>;
    released(id: number): void;
    report(id: number, error: string): void;
}

// Native promises only settle records. All following work and cleanup run on
// update, with identity checked again. No native wrapper survives a callback.
export class OwnedReferenceLifetime {
    static createState(): OwnedReferenceState {
        return { version: 1, generation: 0, references: new Map(), queue: [], queued: new Set(), cursor: 0 };
    }

    readonly state: OwnedReferenceState;
    private adapter: OwnedReferenceAdapter;
    private limit: number;

    constructor(adapter: OwnedReferenceAdapter, state = OwnedReferenceLifetime.createState(), limit = 4096) {
        this.adapter = adapter; this.state = state; this.limit = limit;
    }

    claim(id: number): OwnedReference {
        if (!Number.isInteger(id) || id < 0xff000000 || id > 0xffffffff) throw Error('Only temporary references can be owned');
        if (this.state.references.has(id)) throw Error('Reference is already owned or awaiting cleanup');
        this.requireCapacity();
        const generation = this.state.generation;
        const handle = this.adapter.identity(id);
        if (!handle || generation !== this.state.generation) throw Error('Cannot capture reference identity');
        const reference: OwnedReference = { id, handle, generation, retired: false, work: [] };
        this.state.references.set(id, reference);
        return reference;
    }

    requireCapacity(): void {
        if (this.state.references.size >= this.limit) throw Error('Owned reference limit exceeded');
    }

    isCurrent(reference: OwnedReference): boolean {
        return this.contains(reference) && !reference.retired;
    }

    enqueue(reference: OwnedReference, run: ReferenceWork['run'], complete?: ReferenceWork['complete']): boolean {
        if (!this.isCurrent(reference)) return false;
        if (reference.work.length >= 16) throw Error('Reference work queue limit exceeded');
        reference.work.push({ run, complete }); this.schedule(reference.id);
        return true;
    }

    retire(reference: OwnedReference): void {
        if (!this.contains(reference)) return;
        reference.retired = true; reference.work = [];
        // Do not retain old FormViews through a completed spawn callback.
        if (reference.operation) reference.operation.work = undefined;
        this.schedule(reference.id);
    }

    retireAll(): void { this.state.references.forEach(reference => this.retire(reference)); }

    forgetWorld(): void {
        this.state.references.forEach(reference => {
            reference.retired = true; reference.work = [];
            if (reference.operation) reference.operation.work = undefined;
        });
        ++this.state.generation;
        this.state.references.clear(); this.state.queue = []; this.state.queued.clear(); this.state.cursor = 0;
    }

    // A coordinator must wait for actual native disposal, not just cancellation.
    hasRetiringReferences(): boolean {
        return Array.from(this.state.references.values()).some(reference => reference.retired);
    }

    update(budget = 16): void {
        if (!Number.isInteger(budget) || budget < 1 || budget > 64) throw Error('Invalid reference work budget');
        const count = Math.min(budget, this.state.queue.length - this.state.cursor);
        const generation = this.state.generation;
        for (let i = 0; i < count && generation === this.state.generation; ++i) {
            const id = this.state.queue[this.state.cursor++]; this.state.queued.delete(id);
            const reference = this.state.references.get(id);
            if (reference) this.advance(reference);
        }
        if (this.state.cursor > 1024 || this.state.cursor === this.state.queue.length) {
            this.state.queue = this.state.queue.slice(this.state.cursor); this.state.cursor = 0;
        }
    }

    private contains(reference: OwnedReference): boolean {
        return reference.generation === this.state.generation && this.state.references.get(reference.id) === reference;
    }

    private schedule(id: number): void {
        if (!this.state.queued.has(id)) { this.state.queued.add(id); this.state.queue.push(id); }
    }

    private start(reference: OwnedReference, run: ReferenceWork['run'], operation: ReferenceOperation): void {
        reference.operation = operation;
        const finish = (error?: unknown) => {
            operation.settled = true;
            if (error !== undefined) operation.error = String(error);
            if (this.contains(reference)) this.schedule(reference.id);
        };
        try { Promise.resolve(run()).then(() => finish(), finish); }
        catch (error) { finish(error); }
    }

    private release(reference: OwnedReference): void {
        if (!this.contains(reference)) return;
        this.state.references.delete(reference.id);
        this.adapter.released(reference.id);
    }

    private advance(reference: OwnedReference): void {
        const wasRetired = reference.retired;
        try {
            const operation = reference.operation;
            if (operation && !operation.settled) return;
            if (reference.cleanupFailed) return;
            if (operation) {
                reference.operation = undefined;
                if (operation.error) {
                    if (operation.disposal) reference.cleanupFailed = true;
                    throw Error(operation.error);
                }
            }
            const identity = this.adapter.identity(reference.id);
            if (!this.contains(reference)) return;
            if (identity !== reference.handle) {
                // The original vanished or the numeric FormID was reused. Never
                // touch the replacement, even if it has the same NPC base.
                this.release(reference); return;
            }
            if (operation?.disposal) {
                reference.cleanupFailed = true;
                throw Error('Native deletion completed without deleting the owned reference');
            }
            if (!reference.retired && operation?.work?.complete) operation.work.complete();
            if (!this.contains(reference)) return;
            if (reference.retired) {
                this.start(reference, () => this.adapter.dispose(reference.id, reference.handle),
                    { disposal: true, settled: false });
            } else {
                const work = reference.work.shift();
                if (work) this.start(reference, work.run, { work, disposal: false, settled: false });
            }
        } catch (error) {
            if (!this.contains(reference)) return;
            if (wasRetired) reference.cleanupFailed = true;
            reference.error = String(error);
            this.retire(reference);
            this.adapter.report(reference.id, reference.error);
        }
    }
}
