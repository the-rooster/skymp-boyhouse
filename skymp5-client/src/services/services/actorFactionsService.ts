import { ActorFactionLeases, ActorFactionLeaseState, FactionProgress } from '../../sync/actorFactionLease';
import { CampaignFactionRank, campaignFactionRanks, campaignFactionsFromBaseline } from '../../sync/campaignFactions';
import { CreateActorMessage } from '../messages/createActorMessage';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { NetworkingService } from './networkingService';

interface SelfFactions {
    generation: number;
    owner: string;
    idx: number;
    target?: CampaignFactionRank[];
    available: boolean;
}

// Packet handlers only replace desired snapshots. Native operations and
// restoration run on update, including after disconnect and JS hot reload.
export class ActorFactionsService extends ClientListener {
    readonly leases: ActorFactionLeases;
    private self?: SelfFactions;
    private loading = false;
    private session = 0;
    private reported?: string;

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        const key = 'boyhouseActorFactionLeases';
        const stored = sp.storage[key] as ActorFactionLeaseState | undefined;
        if (stored !== undefined && stored.version !== 1) throw Error('Unsupported actor faction lease state');
        this.leases = new ActorFactionLeases(sp, stored || ActorFactionLeases.createState());
        sp.storage[key] = this.leases.state;
        controller.on('update', () => this.update());
        controller.on('tick', () => { this.leases.checkTimeouts(); this.reportFailure(); });
        controller.on('preLoadGame', () => {
            this.loading = true;
            this.leases.retireAll();
            if (this.self) this.self.available = false;
        });
        // PostLoadGame also runs after a failed attempt. Native lease identity
        // decides whether restoration applies to the old world or is obsolete.
        controller.on('postLoadGame', () => { this.loading = false; });
        controller.emitter.on('gameLoad', () => { this.loading = false; });
        controller.emitter.on('worldModelReset', () => this.reset());
        for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied'] as const)
            controller.emitter.on(event, () => { ++this.session; this.reset(); });
    }

    private reset(): void {
        this.self = undefined; this.reported = undefined;
        this.leases.retireAll();
    }

    beginSpawn(message: CreateActorMessage): number {
        const target = campaignFactionsFromBaseline(message);
        if (this.self) this.leases.retire(this.self.owner);
        const owner = this.leases.newOwner(), generation = this.leases.state.sequence;
        this.self = { generation, owner, idx: message.idx, target, available: false };
        return generation;
    }

    cancelSpawn(generation: number): void {
        if (this.self?.generation !== generation) return;
        this.leases.retire(this.self.owner); this.self = undefined;
    }

    markSpawnAvailable(generation: number): void {
        const self = this.self;
        if (!self || self.generation !== generation || this.loading) return;
        self.available = true;
        if (self.target !== undefined) this.leases.request(self.owner, 0x14, self.target);
    }

    spawnReadiness(generation: number): FactionProgress {
        const self = this.self;
        if (!self || self.generation !== generation || !self.available || this.loading) return { ready: false };
        return self.target === undefined ? this.leases.drained(0x14) : this.leases.readiness(self.owner);
    }

    updateSelf(idx: number, snapshot: unknown): void {
        const target = campaignFactionRanks(snapshot), self = this.self;
        if (!self || self.idx !== idx) return;
        self.target = target;
        if (self.available && !this.loading) this.leases.request(self.owner, 0x14, target);
    }

    prepareDisposal(actorId: number): boolean {
        this.leases.retireActor(actorId);
        return this.leases.drained(actorId).ready;
    }

    get readiness(): FactionProgress { return this.loading ? { ready: false } : this.leases.readiness(); }
    get drained(): FactionProgress { return this.leases.drained(); }
    get blocksGameplayInput(): boolean {
        return !this.leases.readiness().ready || !!this.self && this.self.target !== undefined &&
            (!this.self.available || !this.spawnReadiness(this.self.generation).ready);
    }

    private update(): void {
        if (this.loading) return;
        this.leases.update();
        this.reportFailure();
    }

    private reportFailure(): void {
        const error = this.leases.readiness().error;
        if (!error || error === this.reported) return;
        this.reported = error;
        logError(this, 'Actor faction synchronization failed', error);
        const session = this.session;
        this.controller.once('tick', () => {
            if (session !== this.session || !this.leases.readiness().error) return;
            const network = this.controller.lookupListener(NetworkingService);
            if (network.isConnected()) network.close();
        });
    }
}
