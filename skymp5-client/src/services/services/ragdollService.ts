import { Actor } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { NativeReferenceJob } from '../../campaign/nativeReferenceOperations';
import { NativeReferenceOperationService } from './nativeReferenceOperationService';
import { logError } from '../../logging';

const damageSettings = ['fDiffMultHPToPCE', 'fDiffMultHPToPCH', 'fDiffMultHPToPCL',
    'fDiffMultHPToPCN', 'fDiffMultHPToPCVE', 'fDiffMultHPToPCVH'];
interface DamageLeaseState { version: 1; previous?: number[]; leases: NativeReferenceJob[]; }

export class RagdollService extends ClientListener {
    private state: DamageLeaseState;

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        const key = 'boyhouseRagdollDamageLease';
        const stored = sp.storage[key] as DamageLeaseState | undefined;
        this.state = stored?.version === 1 ? stored : { version: 1, leases: [] };
        sp.storage[key] = this.state;
        // Reload may discard an old native promise callback. Restore the owned
        // global override on the first update instead of leaving damage at zero.
        this.state.leases = [];
        controller.on('update', () => this.reconcileDamage());
    }

    public safeRemoveRagdollFromWorld = (
        actor: Actor, afterRemoveCallback: () => void, current: () => boolean = () => true,
    ) => {
        const id = actor.getFormID();
        let job: NativeReferenceJob;
        job = this.controller.lookupListener(NativeReferenceOperationService).submit(id, 'ragdoll', current, reference => {
            const fresh = this.sp.Actor.from(reference);
            if (!fresh) throw Error('Ragdoll target is no longer an actor');
            this.state.leases.push(job);
            if (!this.state.previous) {
                const previous = damageSettings.map(name => this.sp.Game.getGameSettingFloat(name));
                if (!previous.every(Number.isFinite)) throw Error('Invalid original damage multipliers');
                this.state.previous = previous;
                damageSettings.forEach(name => this.sp.Game.setGameSettingFloat(name, 0));
            }
            return fresh.forceRemoveRagdollFromWorld();
        }, afterRemoveCallback);
        return job;
    };

    private reconcileDamage(): void {
        this.state.leases = this.state.leases.filter(job => job.status === 'running');
        if (this.state.leases.length || !this.state.previous) return;
        try {
            damageSettings.forEach((name, index) => {
                // Preserve a change made by another owner while we were active.
                if (this.sp.Game.getGameSettingFloat(name) === 0)
                    this.sp.Game.setGameSettingFloat(name, this.state.previous![index]);
            });
            this.state.previous = undefined;
        } catch (error) { logError(this, 'Restoring ragdoll damage settings failed', error); }
    }
}
