import { ClientListener, CombinedController, Sp } from './clientListener';
import { logError } from '../../logging';
import { NetworkingService } from './networkingService';
import { ReferenceSuppressionService } from './referenceSuppressionService';
import { SinglePlayerService } from './singlePlayerService';

// Hide unmanaged actors without deleting vanilla references, killing them or
// moving them out of their authored locations. Other suppression owners can
// retain a reference while this service releases its own claim.
export class WorldCleanerService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('update', () => this.onUpdate());
        controller.on('preLoadGame', () => { this.loading = true; this.protection.clear(); this.active = false; });
        controller.emitter.on('gameLoad', () => { this.loading = false; this.protection.clear(); this.active = false; });
    }

    modWcProtection(actorId: number, mod: number): void {
        const protection = Math.max(0, this.getWcProtection(actorId) + mod);
        if (protection) {
            this.protection.set(actorId, protection);
            this.references.release('worldCleaner', actorId);
        } else this.protection.delete(actorId);
    }

    getWcProtection(actorId: number): number { return this.protection.get(actorId) || 0; }

    setProjectionMode(enabled: boolean): void {
        this.projectionMode = enabled;
        if (enabled) { this.references.releaseOwner('worldCleaner'); this.active = false; }
    }

    private get references() { return this.controller.lookupListener(ReferenceSuppressionService).references; }

    private onUpdate(): void {
        if (this.loading) return;
        const multiplayer = !this.controller.lookupListener(SinglePlayerService).isSinglePlayer &&
            this.controller.lookupListener(NetworkingService).isConnected();
        if (!multiplayer || this.projectionMode) {
            if (this.active) this.references.releaseOwner('worldCleaner');
            this.active = false;
            return;
        }
        this.active = true;
        const player = this.sp.Game.getPlayer();
        if (!player) return;
        const actor = this.sp.Game.findRandomActor(player.getPositionX(), player.getPositionY(), player.getPositionZ(), 8192);
        if (!actor) return;
        const id = actor.getFormID();
        if (id === 0x14 || this.getWcProtection(id) > 0 || actor.isDisabled() || actor.isDeleted()) return;
        const authoredCorpse = actor.isDead() && id < 0xff000000;
        if (authoredCorpse && actor.isActivationBlocked()) return;
        try {
            // Preserve authored corpses, including their original activation flag.
            this.references.acquire('worldCleaner', id, authoredCorpse ? 'activation' : 'hidden');
        } catch (error) { logError(this, 'Could not suppress unmanaged actor', id.toString(16), error); }
    }

    private protection = new Map<number, number>();
    // Also release a persisted claim if a plugin reload starts disconnected.
    private active = true;
    private loading = false;
    private projectionMode = false;
}
