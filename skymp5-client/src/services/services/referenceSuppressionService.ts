import { ReferenceSuppression, SuppressionState } from '../../campaign/referenceSuppression';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';

export class ReferenceSuppressionService extends ClientListener {
    readonly references: ReferenceSuppression;

    constructor(private sp: Sp, controller: CombinedController) {
        super();
        const key = 'boyhouseReferenceSuppression';
        const stored = sp.storage[key] as SuppressionState | undefined;
        const state = stored?.version === 1 ? stored : ReferenceSuppression.createState();
        sp.storage[key] = state;
        const resolve = (id: number, baseId: number) => {
            const reference = sp.ObjectReference.from(sp.Game.getFormEx(id));
            if (!reference || reference.isDeleted() || reference.getBaseObject()?.getFormID() !== baseId)
                throw Error(`Reference ${id.toString(16)} is no longer the captured form`);
            return reference;
        };
        this.references = new ReferenceSuppression({
            read: id => {
                const reference = sp.ObjectReference.from(sp.Game.getFormEx(id));
                const baseId = reference?.getBaseObject()?.getFormID();
                if (!reference || reference.isDeleted() || !baseId) return undefined;
                const actor = sp.Actor.from(reference);
                return { baseId, disabled: reference.isDisabled(), activationBlocked: reference.isActivationBlocked(),
                    inDialogue: !!actor && (actor.isInDialogueWithPlayer() || actor.getDialogueTarget() !== null) };
            },
            blockActivation: (id, baseId, blocked) => resolve(id, baseId).blockActivation(blocked),
            setDisabled: (id, baseId, disabled) => {
                const reference = resolve(id, baseId);
                return disabled ? reference.disable(false) : reference.enable(false);
            },
            report: (id, error) => logError(this, 'Reference suppression failed', id.toString(16), error),
        }, state);
        controller.on('update', () => this.references.update());
        controller.emitter.on('gameLoad', () => this.references.forgetWorld());
        for (const event of ['connectionDisconnect', 'connectionDenied', 'connectionFailed'] as const)
            controller.emitter.on(event, () => this.references.releaseAll());
    }
}
