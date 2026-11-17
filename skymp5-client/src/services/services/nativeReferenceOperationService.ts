import { ObjectReference } from 'skyrimPlatform';
import { NativeReferenceJob, NativeReferenceOperations, NativeReferenceOperationState } from '../../campaign/nativeReferenceOperations';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { OwnedReferenceService } from './ownedReferenceService';

type NativeStep = (reference: ObjectReference) => void | Promise<void>;

export class NativeReferenceOperationService extends ClientListener {
    readonly operations: NativeReferenceOperations;
    private loading = false;

    constructor(private sp: Sp, controller: CombinedController) {
        super();
        const key = 'boyhouseNativeReferenceOperations';
        const stored = sp.storage[key] as NativeReferenceOperationState | undefined;
        const state = stored?.version === 1 ? stored : NativeReferenceOperations.createState();
        sp.storage[key] = state;
        this.operations = new NativeReferenceOperations({
            identity: id => sp.getReferenceHandle(id),
            canStart: id => !controller.lookupListener(OwnedReferenceService).references.hasNativeOperation(id),
            report: job => logError(this, job.key, job.id.toString(16), job.error),
        }, state);
        this.operations.cancelEpoch();
        controller.on('preLoadGame', () => { this.loading = true; this.operations.cancelEpoch(); });
        controller.on('postLoadGame', (event?: { success: boolean }) => {
            if (event?.success === false) this.loading = false;
        });
        controller.emitter.on('gameLoad', () => { this.loading = false; this.operations.forgetWorld(); });
        controller.emitter.on('worldModelReset', () => this.operations.cancelEpoch());
        controller.on('update', () => { if (!this.loading) this.operations.update(); });
    }

    submit(id: number, key: string, current: () => boolean,
        run: NativeStep | NativeStep[], complete?: () => void, persistent = false): NativeReferenceJob {
        const steps = (Array.isArray(run) ? run : [run]).map(step => () => {
            const reference = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!reference || reference.isDeleted()) throw Error('Native operation reference disappeared');
            return step(reference);
        });
        return this.operations.submit(id, key, current, steps, complete, persistent);
    }

    // Suppression owns restoration across campaign epochs; it supplies a world
    // lifetime guard and waits for the actual native result through this bridge.
    submitPersistent(id: number, key: string, current: () => boolean,
        run: (reference: ObjectReference) => void | Promise<void>): Promise<void> {
        const job = this.submit(id, key, current, run, undefined, true);
        return new Promise((resolve, reject) => {
            job.onTerminal = () => {
                if (job.status === 'complete') resolve();
                else reject(Error(job.error || 'Native operation cancelled'));
            };
        });
    }
}
