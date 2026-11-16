import { OwnedReferenceLifetime, OwnedReferenceState } from '../../campaign/ownedReferenceLifetime';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { WorldCleanerService } from './worldCleanerService';
import { NativeReferenceOperationService } from './nativeReferenceOperationService';

export class OwnedReferenceService extends ClientListener {
    readonly references: OwnedReferenceLifetime;

    constructor(sp: Sp, controller: CombinedController) {
        super();
        if (typeof sp.getReferenceHandle !== 'function')
            throw Error('This client requires the Boyhouse native reference-lifetime API');
        const key = 'boyhouseOwnedReferences';
        const stored = sp.storage[key] as OwnedReferenceState | undefined;
        const state = stored?.version === 1 ? stored : OwnedReferenceLifetime.createState();
        sp.storage[key] = state;
        this.references = new OwnedReferenceLifetime({
            identity: id => sp.getReferenceHandle(id),
            canStart: id => !controller.lookupListener(NativeReferenceOperationService).operations.isBusy(id),
            dispose: (id, handle) => {
                if (sp.getReferenceHandle(id) !== handle) throw Error('Owned reference identity changed before disposal');
                const reference = sp.ObjectReference.from(sp.Game.getFormEx(id));
                if (!reference) throw Error('Owned reference disappeared before disposal');
                const actor = sp.Actor.from(reference);
                if (actor) sp.TESModPlatform.setWeaponDrawnMode(actor, -1);
                return reference.delete();
            },
            released: id => controller.lookupListener(WorldCleanerService).modWcProtection(id, -1),
            report: (id, error) => logError(this, 'Owned reference work failed', id.toString(16), error),
        }, state);
        // A hot reload retires previous views, including unfinished native work.
        const recovering = Array.from(state.references.values());
        this.references.retireAll();
        controller.once('update', () => {
            recovering.forEach(reference => {
                if (state.references.get(reference.id) === reference)
                    controller.lookupListener(WorldCleanerService).modWcProtection(reference.id, 1);
            });
        });
        let loading = false;
        controller.on('preLoadGame', () => { loading = true; this.references.forgetWorld(); });
        controller.on('update', () => { if (!loading) this.references.update(); });
        controller.emitter.on('gameLoad', () => { loading = false; this.references.forgetWorld(); });
        for (const event of ['connectionDisconnect', 'connectionDenied', 'connectionFailed'] as const)
            controller.emitter.on(event, () => this.references.retireAll());
    }
}
