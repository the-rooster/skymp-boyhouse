export type SuppressionMode = 'activation' | 'hidden';

export interface ReferenceState {
    baseId: number;
    disabled: boolean;
    activationBlocked: boolean;
    inDialogue: boolean;
}

export interface SuppressionAdapter {
    read(id: number): ReferenceState | undefined;
    blockActivation(id: number, baseId: number, blocked: boolean): void;
    setDisabled(id: number, baseId: number, disabled: boolean): Promise<void>;
    report(id: number, error: string): void;
}

interface PendingOperation {
    disabled: boolean;
    settled: boolean;
    error?: string;
}

interface SuppressedReference {
    original: ReferenceState;
    owners: Map<string, SuppressionMode>;
    settled: boolean;
    operation?: PendingOperation;
    error?: string;
}

// Stored across plugin reloads. In-flight completions only mark this shared
// operation record; they never perform another engine mutation.
export interface SuppressionState {
    version: 1;
    generation: number;
    entries: Map<number, SuppressedReference>;
    queue: number[];
    queued: Set<number>;
    cursor: number;
}

export class ReferenceSuppression {
    static createState(): SuppressionState {
        return { version: 1, generation: 0, entries: new Map(), queue: [], queued: new Set(), cursor: 0 };
    }

    private adapter: SuppressionAdapter;
    readonly state: SuppressionState;
    private maxEntries: number;

    constructor(adapter: SuppressionAdapter, state = ReferenceSuppression.createState(), maxEntries = 16384) {
        this.adapter = adapter; this.state = state; this.maxEntries = maxEntries;
    }

    acquire(owner: string, id: number, mode: SuppressionMode = 'hidden'): void {
        if (!owner || !Number.isInteger(id) || id <= 0 || id > 0xffffffff || id === 0x14 ||
            (mode !== 'hidden' && mode !== 'activation')) throw Error('Invalid reference suppression request');
        let entry = this.state.entries.get(id);
        if (!entry) {
            if (this.state.entries.size >= this.maxEntries) throw Error('Reference suppression limit exceeded');
            const generation = this.state.generation;
            const original = this.adapter.read(id);
            if (!original || generation !== this.state.generation) throw Error(`Cannot capture reference ${id.toString(16)}`);
            entry = { original: { ...original }, owners: new Map(), settled: false };
            this.state.entries.set(id, entry);
        }
        if (entry.owners.get(owner) !== mode) entry.error = undefined;
        entry.owners.set(owner, mode);
        entry.settled = false;
        this.enqueue(id);
    }

    release(owner: string, id: number): void {
        const entry = this.state.entries.get(id);
        if (!entry?.owners.delete(owner)) return;
        entry.error = undefined;
        entry.settled = false;
        this.enqueue(id);
    }

    releaseOwner(owner: string): void {
        this.state.entries.forEach((_entry, id) => this.release(owner, id));
    }

    releaseAll(): void {
        this.state.entries.forEach((entry, id) => {
            entry.owners.clear(); entry.error = undefined; entry.settled = false;
            this.enqueue(id);
        });
    }

    // A newly loaded save has its own engine state. Never replay a previous
    // world's restoration into a reference with the same numeric FormID.
    forgetWorld(): void {
        ++this.state.generation;
        this.state.entries.clear(); this.state.queue = []; this.state.queued.clear(); this.state.cursor = 0;
    }

    isSettled(ids: readonly number[]): boolean {
        return ids.every(id => !this.state.entries.has(id) || this.state.entries.get(id)!.settled);
    }

    error(id: number): string | undefined { return this.state.entries.get(id)?.error; }

    update(budget = 16): void {
        if (!Number.isInteger(budget) || budget < 1 || budget > 64) throw Error('Invalid suppression update budget');
        const count = Math.min(budget, this.state.queue.length - this.state.cursor);
        const generation = this.state.generation;
        for (let i = 0; i < count && this.state.generation === generation; ++i) {
            const id = this.state.queue[this.state.cursor++]; this.state.queued.delete(id);
            const entry = this.state.entries.get(id);
            if (entry) this.reconcile(id, entry, generation);
        }
        if (this.state.cursor > 1024 || this.state.cursor === this.state.queue.length) {
            this.state.queue = this.state.queue.slice(this.state.cursor); this.state.cursor = 0;
        }
    }

    private enqueue(id: number): void {
        if (!this.state.queued.has(id)) { this.state.queued.add(id); this.state.queue.push(id); }
    }

    private reconcile(id: number, entry: SuppressedReference, generation: number): void {
        const current = () => generation === this.state.generation && this.state.entries.get(id) === entry;
        try {
            const completed = entry.operation;
            if (completed) {
                // Finish a native disable before reconciling a release/reacquire;
                // never race it against a competing enable.
                if (!completed.settled) return;
                entry.operation = undefined;
            }
            if (entry.error) return;
            const observed = this.adapter.read(id);
            if (!current()) return;
            if (!observed) {
                if (entry.owners.size === 0) { this.state.entries.delete(id); return; }
                throw Error('Suppressed reference disappeared');
            }
            if (observed.baseId !== entry.original.baseId) throw Error('Suppressed reference identity changed');
            const disabled = entry.original.disabled || Array.from(entry.owners.values()).includes('hidden');
            const blocked = entry.original.activationBlocked || entry.owners.size > 0;
            if (completed?.error && disabled === completed.disabled) throw Error(completed.error);
            if (completed && observed.disabled !== completed.disabled && disabled === completed.disabled)
                throw Error('Native disable/enable did not reach the requested state');
            if (disabled && !observed.disabled && observed.inDialogue) { this.enqueue(id); return; }
            if (observed.disabled !== disabled) {
                // Block interaction first when hiding. Restore activation only
                // after an enable has completed and its state was observed.
                if (disabled && !observed.activationBlocked) this.adapter.blockActivation(id, observed.baseId, true);
                if (!current()) return;
                const operation: PendingOperation = { disabled, settled: false };
                entry.operation = operation;
                const finish = (error?: unknown) => {
                    operation.settled = true;
                    if (error !== undefined) operation.error = String(error);
                    if (current()) this.enqueue(id);
                };
                try { this.adapter.setDisabled(id, observed.baseId, disabled).then(() => finish(), finish); }
                catch (error) { finish(error); }
                return;
            }
            if (observed.activationBlocked !== blocked) this.adapter.blockActivation(id, observed.baseId, blocked);
            if (!current()) return;
            const verified = this.adapter.read(id);
            if (!current()) return;
            if (!verified || verified.baseId !== entry.original.baseId || verified.disabled !== disabled ||
                verified.activationBlocked !== blocked) throw Error('Reference suppression did not reach the requested state');
            if (entry.owners.size === 0) this.state.entries.delete(id);
            else entry.settled = true;
        } catch (error) {
            if (!current()) return;
            entry.error = String(error); entry.settled = false;
            this.adapter.report(id, entry.error);
        }
    }
}
