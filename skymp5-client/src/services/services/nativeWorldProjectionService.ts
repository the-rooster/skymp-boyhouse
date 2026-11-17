import { ProjectionEngine, ProjectionProgress } from '../../campaign/worldProjectionCoordinator';
import { ProjectedReference, WorldBaseline, WorldProjection } from '../../campaign/worldProjection';
import { WorldView } from '../../view/worldView';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { NativeReferenceOperationService } from './nativeReferenceOperationService';
import { OwnedReferenceService } from './ownedReferenceService';
import { ReferenceSuppressionService } from './referenceSuppressionService';
import { WorldCleanerService } from './worldCleanerService';
import { RemoteServer } from './remoteServer';
import { PlayerSpawnService } from './playerSpawnService';
import { PlayerLoadoutService } from './playerLoadoutService';
import { NetworkingService } from './networkingService';
import { ActorFactionsService } from './actorFactionsService';

interface ControlLease { version: 1; enabled?: boolean[]; }

// Engine adapter for experimental projection connections. Network callbacks
// only retire/reset models or dispatch baselines; native work runs on update.
export class NativeWorldProjectionService extends ClientListener implements ProjectionEngine {
    private target?: WorldProjection;
    private references: ProjectedReference[] = [];
    private spawnGeneration = 0;
    private acquired = 0;
    private installed = false;
    private restoreIds: number[] = [];
    private blocked = false;
    private loading = false;
    private error?: string;
    private controls: ControlLease;

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        const key = 'boyhouseProjectionControls';
        const stored = sp.storage[key] as ControlLease | undefined;
        this.controls = stored?.version === 1 ? stored : { version: 1 };
        sp.storage[key] = this.controls;
        controller.on('preLoadGame', () => { this.loading = true; });
        controller.on('postLoadGame', (event?: { success: boolean }) => {
            if (event?.success === false) this.loading = false;
        });
        controller.emitter.on('gameLoad', () => { this.loading = false; });
        controller.on('update', () => this.update());
    }

    private get view() { return this.controller.lookupListener(WorldView); }
    private get suppression() { return this.controller.lookupListener(ReferenceSuppressionService).references; }
    private get owned() { return this.controller.lookupListener(OwnedReferenceService).references; }
    private get operations() { return this.controller.lookupListener(NativeReferenceOperationService).operations; }
    private get loadout() { return this.controller.lookupListener(PlayerLoadoutService); }
    private get factions() { return this.controller.lookupListener(ActorFactionsService); }

    cancel(): void {
        this.blocked = true; this.target = undefined; this.references = []; this.acquired = 0; this.installed = false;
        this.error = undefined;
        this.view.setFormViewUpdateAllowed(false);
        this.controller.lookupListener(RemoteServer).resetWorldModel();
        this.owned.retireAll();
        this.controller.lookupListener(WorldCleanerService).setProjectionMode(true);
        this.restoreIds = Array.from(this.suppression.state.entries.keys());
        this.suppression.releaseOwner('campaignProjection');
    }

    drained(): ProjectionProgress {
        const ownedError = Array.from(this.owned.state.references.values()).find(reference => reference.error)?.error;
        const nativeError = this.operations.state.errors[0]?.error;
        const suppressionError = this.restoreIds.map(id => this.suppression.error(id)).find(Boolean);
        const factions = this.factions.drained;
        const error = this.error || ownedError || nativeError || suppressionError || factions.error;
        return { ready: !error && !this.owned.hasRetiringReferences() && !this.operations.isBusy() &&
            this.suppression.isSettled(this.restoreIds) && factions.ready, error };
    }

    apply(projection: WorldProjection, baseline: WorldBaseline): void {
        if (!this.drained().ready || this.target) throw Error('Old engine work has not drained');
        const copies = new Map(projection.references.map(reference => [reference.copy, reference]));
        const originals = new Set(projection.references.map(reference => reference.original));
        const references = [...projection.references];
        for (const message of baseline.messages) {
            if (message.t !== 33 || message.isMe) continue;
            const mapping = copies.get(message.refrId);
            if (mapping && (mapping.disabled || mapping.deleted || message.baseId !== mapping.baseId))
                throw Error('Baseline copy conflicts with its projection manifest');
            if (originals.has(message.refrId) || originals.has(message.refrId - 0x100000000))
                throw Error('Baseline contains an original replaced by a campaign copy');
            // The server represents ordinary authored NPCs as 64-bit virtual
            // IDs too. Suppress those originals to prevent a duplicate beside
            // their replicated FormView, even outside a personal instance.
            if (message.refrId > 0x100000000) references.push({
                original: message.refrId - 0x100000000, copy: message.refrId, baseId: message.baseId,
                kind: 'actor', disabled: false, deleted: false,
            });
        }
        this.target = projection; this.acquired = 0; this.installed = false;
        this.references = references;
        if (!this.controller.lookupListener(NetworkingService).applyCampaignBaseline(baseline))
            throw Error('The baseline lost its selected network epoch');
        const spawn = this.controller.lookupListener(PlayerSpawnService).status;
        if (!spawn) throw Error('Baseline did not request a player spawn');
        this.spawnGeneration = spawn.generation;
    }

    observe(): ProjectionProgress {
        const error = this.error || this.operations.state.errors[0]?.error;
        if (error) return { ready: false, error };
        const target = this.target;
        if (!target || this.loading || !this.installed) return { ready: false };
        const player = this.controller.lookupListener(PlayerSpawnService);
        if (player.status?.phase === 'failed') return { ready: false, error: player.status.error || 'Player spawn failed' };
        if (!player.isReady(this.spawnGeneration)) return { ready: false };
        const loadout = this.loadout.projectionReadiness;
        if (!loadout.ready) return loadout;
        const factions = this.factions.readiness;
        if (!factions.ready) return factions;
        const originals = this.controller.lookupListener(RemoteServer).getOriginalApplicationProgress();
        if (!originals.ready) return originals;
        const ids = this.references.map(reference => reference.original);
        const suppressionError = ids.map(id => this.suppression.error(id)).find(Boolean);
        if (suppressionError) return { ready: false, error: suppressionError };
        if (!this.suppression.isSettled(ids) || this.operations.isBusy() || this.owned.hasRetiringReferences())
            return { ready: false };
        const views = this.view.getSpawnReadiness(this.controller.lookupListener(RemoteServer).getWorldModel());
        return { ready: views.ready, error: views.failed[0]?.error };
    }

    release(): void {
        if (!this.observe().ready) throw Error('Projection is no longer ready for gameplay');
        this.blocked = false;
        this.reconcileControls();
    }

    disconnect(): void {
        this.cancel(); this.blocked = false;
        this.controller.lookupListener(WorldCleanerService).setProjectionMode(false);
    }

    private update(): void {
        if (this.loading || (!this.blocked && !this.controls.enabled && !this.target &&
            !this.loadout.blocksGameplayInput && !this.factions.blocksGameplayInput)) return;
        try {
            this.reconcileControls();
            const target = this.target;
            if (!target || this.installed || this.error) return;
            const player = this.controller.lookupListener(PlayerSpawnService);
            if (player.status?.phase === 'failed') throw Error(player.status.error || 'Player spawn failed');
            if (!player.isReady(this.spawnGeneration)) return;
            const end = Math.min(this.acquired + 16, this.references.length);
            for (; this.acquired < end; ++this.acquired) {
                const mapping = this.references[this.acquired];
                const original = this.sp.ObjectReference.from(this.sp.Game.getFormEx(mapping.original));
                if (!original || original.isDeleted() || original.getBaseObject()?.getFormID() !== mapping.baseId ||
                    (!!this.sp.Actor.from(original)) !== (mapping.kind === 'actor'))
                    throw Error('Projection original does not match installed content: ' + mapping.original.toString(16));
                // Disabled/deleted campaign copies still suppress their authored
                // original; there must not be a fallback visible vanilla actor.
                this.suppression.acquire('campaignProjection', mapping.original);
            }
            if (this.acquired !== this.references.length) return;
            const ids = this.references.map(reference => reference.original);
            const error = ids.map(id => this.suppression.error(id)).find(Boolean);
            if (error) throw Error(error);
            if (!this.suppression.isSettled(ids)) return;
            this.view.setProjectionReferences(this.references);
            this.view.setFormViewUpdateAllowed(true);
            this.installed = true;
        } catch (error) {
            this.error = String(error); logError(this, 'Applying campaign world failed', error);
            if (this.loadout.blocksGameplayInput) this.loadout.failCreation(this.error);
        }
    }

    private controlFlags(): boolean[] {
        const game = this.sp.Game;
        return [game.isMovementControlsEnabled(), game.isFightingControlsEnabled(),
            game.isCamSwitchControlsEnabled(), game.isLookingControlsEnabled(),
            game.isSneakingControlsEnabled(), game.isMenuControlsEnabled(),
            game.isActivateControlsEnabled(), game.isJournalControlsEnabled()];
    }

    private reconcileControls(): void {
        if (!this.sp.Game.getPlayer()) return;
        const creationBlocked = this.loadout.blocksGameplayInput;
        const blocking = this.blocked || creationBlocked || this.factions.blocksGameplayInput;
        if (blocking && !this.controls.enabled) {
            const flags = this.controlFlags();
            // Camera/looking remain available. Only flags changed by this lease
            // are restored; controls already disabled by the game stay disabled.
            this.controls.enabled = flags.map((enabled, index) => enabled && index !== 2 && index !== 3);
        }
        const owned = this.controls.enabled;
        if (!owned) return;
        // Keep the movement/action lease continuously through projection ->
        // creation. The creator needs menu/camera controls even during a later
        // same-actor projection, but ordinary gameplay stays disabled.
        const desired = [blocking, blocking, false, false, blocking,
            blocking && !this.loadout.creationMenuActive, blocking, blocking];
        const current = this.controlFlags();
        const disable = desired.map((hold, index) => hold && current[index]);
        const enable = owned.map((held, index) => held && !desired[index] && !current[index]);
        if (disable.some(Boolean))
            this.sp.Game.disablePlayerControls(disable[0], disable[1], false, false, disable[4], disable[5], disable[6], disable[7], 0);
        if (enable.some(Boolean))
            this.sp.Game.enablePlayerControls(enable[0], enable[1], false, false, enable[4], enable[5], enable[6], enable[7], 0);
        const observed = this.controlFlags();
        if (observed.some((enabled, index) => desired[index] && enabled))
            throw Error('Native player controls did not become blocked');
        if (observed.some((enabled, index) => enable[index] && !enabled))
            throw Error('Native player controls did not restore');
        if (!blocking) this.controls.enabled = undefined;
    }
}
