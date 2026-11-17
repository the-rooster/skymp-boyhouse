import { CampaignFactionRank, campaignFactionRanks } from './campaignFactions';

export interface NativeActorFactionsStatus {
    token: string;
    actorId: number;
    revision?: number;
    status: 'idle' | 'queued' | 'observing' | 'settled' | 'restoring' | 'restored' | 'failed' | 'superseded';
    mutationPending: boolean;
    uncertain: boolean;
    error?: string;
}

export interface NativeActorFactionsApi {
    actorFactionsApiVersion: number;
    beginActorFactions(actorId: number): string;
    applyActorFactions(token: string, revision: number, entries: CampaignFactionRank[]): NativeActorFactionsStatus;
    restoreActorFactions(token: string): NativeActorFactionsStatus;
    getActorFactionsStatus(token: string): NativeActorFactionsStatus;
    releaseActorFactions(token: string): void;
}

export interface FactionProgress { ready: boolean; error?: string; }
interface Lease {
    id: number;
    owner: string;
    actorId: number;
    target: CampaignFactionRank[];
    targetKey: string;
    revision: number;
    submitted?: number;
    token?: string;
    retiring: boolean;
    restoring: boolean;
    ready: boolean;
    error?: string;
    cleanupError?: string;
    deadline: number;
    timeoutError?: string;
}

export interface ActorFactionLeaseState {
    version: 1;
    runtime: number;
    sequence: number;
    cursor: number;
    leases: Map<number, Lease>;
    active: Map<string, number>;
}

export function requireActorFactionsApi(platform: unknown): NativeActorFactionsApi {
    const api = platform as NativeActorFactionsApi;
    if (!api || api.actorFactionsApiVersion !== 1 || ['beginActorFactions', 'applyActorFactions',
        'restoreActorFactions', 'getActorFactionsStatus', 'releaseActorFactions']
        .some(name => typeof (api as any)[name] !== 'function'))
        throw Error('Updated Skyrim Platform client required: actor factions API version 1 is missing');
    return api;
}

const states = ['idle', 'queued', 'observing', 'settled', 'restoring', 'restored', 'failed', 'superseded'];
function status(value: NativeActorFactionsStatus, lease: Lease): NativeActorFactionsStatus {
    const released = value && lease.retiring && value.status === 'superseded' && value.actorId === 0 &&
        value.mutationPending === false && value.uncertain === false;
    if (!value || value.token !== lease.token || (value.actorId !== lease.actorId && !released) || !states.includes(value.status) ||
        typeof value.mutationPending !== 'boolean' || typeof value.uncertain !== 'boolean' ||
        (value.revision !== undefined && (!Number.isSafeInteger(value.revision) || value.revision < 1)))
        throw Error('Invalid native actor faction lease status');
    return value;
}

// Only update() calls native methods. Tokens and retirement records survive
// JavaScript reload; a replacement coordinator drains them before reusing an
// actor. Receiving a target or canceling a view never proves engine completion.
export class ActorFactionLeases {
    static createState(): ActorFactionLeaseState {
        return { version: 1, runtime: 0, sequence: 0, cursor: 0, leases: new Map(), active: new Map() };
    }

    private runtime: number;
    constructor(private platform: unknown, readonly state = ActorFactionLeases.createState(), private now = () => Date.now()) {
        this.runtime = ++state.runtime;
        this.retireAll();
    }

    private current(): boolean { return this.runtime === this.state.runtime; }
    hasOwner(owner: string): boolean { return this.current() && this.state.active.has(owner); }
    newOwner(): string {
        if (!this.current()) throw Error('Actor faction coordinator was replaced');
        return `factions:${++this.state.sequence}`;
    }

    request(owner: string, actorId: number, value: unknown): void {
        if (!this.current()) throw Error('Actor faction coordinator was replaced');
        if (!owner || !Number.isInteger(actorId) || (actorId !== 0x14 && (actorId < 0xff000000 || actorId > 0xffffffff)))
            throw Error('Campaign factions require the player or an owned temporary actor');
        const target = campaignFactionRanks(value), key = JSON.stringify(target);
        const previousId = this.state.active.get(owner);
        const previous = previousId === undefined ? undefined : this.state.leases.get(previousId);
        if (previous && previous.actorId === actorId) {
            if (previous.targetKey !== key) {
                previous.target = target; previous.targetKey = key; ++previous.revision; previous.ready = false;
                previous.deadline = this.now() + 30000;
            }
            return;
        }
        if (Array.from(this.state.leases.values()).some(lease => !lease.retiring && lease.owner !== owner && lease.actorId === actorId))
            throw Error('An actor already has an authoritative faction owner');
        this.retire(owner);
        if (this.state.leases.size >= 4096) throw Error('Actor faction lease limit exceeded');
        const id = ++this.state.sequence;
        this.state.leases.set(id, { id, owner, actorId, target, targetKey: key, revision: 1,
            retiring: false, restoring: false, ready: false, deadline: this.now() + 30000 });
        this.state.active.set(owner, id);
    }

    retire(owner: string): void {
        if (!this.current()) return;
        const id = this.state.active.get(owner), lease = id === undefined ? undefined : this.state.leases.get(id);
        this.state.active.delete(owner);
        if (lease) this.retireLease(lease);
    }

    retireActor(actorId: number): void {
        if (!this.current()) return;
        for (const lease of Array.from(this.state.leases.values())) if (lease.actorId === actorId) this.retireLease(lease);
    }

    retireAll(): void {
        if (!this.current()) return;
        for (const owner of Array.from(this.state.active.keys())) this.retire(owner);
        // Recover a token even if the prior runtime was interrupted between
        // recording it and publishing the active-owner mapping.
        for (const lease of Array.from(this.state.leases.values())) this.retireLease(lease);
    }

    private retireLease(lease: Lease): void {
        if (this.state.active.get(lease.owner) === lease.id) this.state.active.delete(lease.owner);
        if (!lease.retiring) { lease.deadline = this.now() + 30000; lease.timeoutError = undefined; }
        lease.retiring = true; lease.ready = false; lease.error = undefined;
    }

    readiness(owner?: string): FactionProgress {
        if (!this.current()) return { ready: false, error: 'Actor faction coordinator was replaced' };
        const leases = Array.from(this.state.leases.values()).filter(lease => owner === undefined || lease.owner === owner);
        const error = leases.map(lease => lease.retiring ? lease.cleanupError || lease.timeoutError : lease.error).find(Boolean);
        return { ready: !error && leases.every(lease => !lease.retiring && lease.ready), error };
    }

    drained(actorId?: number): FactionProgress {
        if (!this.current()) return { ready: false, error: 'Actor faction coordinator was replaced' };
        const leases = Array.from(this.state.leases.values()).filter(lease => actorId === undefined || lease.actorId === actorId);
        return { ready: leases.length === 0, error: leases.map(lease => lease.cleanupError || lease.timeoutError).find(Boolean) };
    }

    // A tick may detect a stalled native update queue, but may not dispatch
    // Papyrus work. Timed-out cleanup remains resumable and retains its token.
    checkTimeouts(): void {
        if (!this.current()) return;
        for (const lease of Array.from(this.state.leases.values())) {
            if (lease.ready || this.now() < lease.deadline) continue;
            if (lease.retiring) lease.timeoutError = 'Actor faction restoration timed out';
            else lease.error = lease.error || 'Actor faction application timed out';
        }
    }

    update(budget = 16): void {
        if (!this.current() || !this.state.leases.size) return;
        if (!Number.isInteger(budget) || budget < 1 || budget > 64) throw Error('Invalid actor faction work budget');
        const ids = Array.from(this.state.leases.keys()), start = this.state.cursor % ids.length;
        this.state.cursor = (start + Math.min(budget, ids.length)) % ids.length;
        for (let i = 0; i < Math.min(budget, ids.length) && this.current(); ++i) {
            const lease = this.state.leases.get(ids[(start + i) % ids.length]);
            if (!lease) continue;
            try { this.advance(lease); }
            catch (error) {
                lease.ready = false;
                if (lease.retiring) lease.cleanupError = String(error);
                else lease.error = String(error);
            }
        }
    }

    private advance(lease: Lease): void {
        if (lease.retiring && !lease.token) { this.state.leases.delete(lease.id); return; }
        if (!lease.retiring && lease.error) return;
        const api = requireActorFactionsApi(this.platform);
        if (lease.retiring) {
            // A failed restoration must not be dispatched again blindly.
            // Still poll its retained token: a confirmed world replacement or
            // completed native cleanup can later prove it safe to release.
            const observed = status(lease.restoring || lease.cleanupError ? api.getActorFactionsStatus(lease.token!) :
                api.restoreActorFactions(lease.token!), lease);
            lease.restoring = true;
            if (observed.uncertain || observed.status === 'failed')
                throw Error(observed.error || 'Native actor faction restoration failed');
            if ((observed.status === 'restored' || observed.status === 'superseded') && !observed.mutationPending) {
                api.releaseActorFactions(lease.token!);
                this.state.leases.delete(lease.id);
            }
            return;
        }
        if (!lease.token) {
            if (Array.from(this.state.leases.values()).some(other => other !== lease && other.actorId === lease.actorId && other.token))
                return; // Preserve the original state until the previous lease has restored.
            const token = api.beginActorFactions(lease.actorId);
            if (typeof token !== 'string' || !token) throw Error('Invalid native actor faction lease token');
            lease.token = token;
            if (!this.current() || lease.retiring) return;
        }
        const revision = lease.revision, submitting = lease.submitted !== revision;
        const observed = status(submitting ?
            api.applyActorFactions(lease.token, revision, lease.target) : api.getActorFactionsStatus(lease.token), lease);
        lease.submitted = revision;
        if (!this.current() || lease.retiring || revision !== lease.revision) return;
        if (observed.uncertain || observed.status === 'failed' || observed.status === 'superseded')
            throw Error(observed.error || 'Native actor faction application lost its actor');
        if (observed.revision !== revision || !['queued', 'observing', 'settled'].includes(observed.status))
            throw Error('Native actor faction revision does not match the current snapshot');
        lease.ready = !submitting && observed.status === 'settled' && !observed.mutationPending;
    }
}
