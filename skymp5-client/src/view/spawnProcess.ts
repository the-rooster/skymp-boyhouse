import { ObjectReference, Game, Actor, MotionType } from "skyrimPlatform";
import { Appearance, applyTints } from "../sync/appearance";
import { NiPoint3 } from "../sync/movement";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { OwnedReference, OwnedReferenceLifetime } from '../campaign/ownedReferenceLifetime';

export class SpawnProcess {
  private position: NiPoint3;
  private started = Date.now();
  private stableSamples = 0;
  private originalAI?: boolean;
  private aiCaptured = false;
  private configured = false;

  constructor(
    appearance: Appearance | null,
    pos: NiPoint3,
    private reference: OwnedReference,
    private lifetime: OwnedReferenceLifetime,
    private callback: () => void,
    rotation?: NiPoint3,
    private isDead = false,
  ) {
    const position = [...pos] as NiPoint3;
    this.position = position;
    // A newly enabled actor can start its package before model loading and
    // placement have completed. Hold only this owned copy's AI until observed.
    if (rotation) {
      const angles = [...rotation] as NiPoint3;
      lifetime.enqueue(reference, () => { this.holdAI(); return this.resolve().setAngle(...angles); });
    }
    lifetime.enqueue(reference, () => { this.holdAI(); return this.resolve().setPosition(...position); }, () => this.enable(appearance));
  }

  private holdAI(): void {
    if (this.aiCaptured) return;
    this.aiCaptured = true;
    const actor = Actor.from(this.resolve());
    if (actor) { this.originalAI = actor.isAIEnabled(); actor.enableAI(false); }
  }

  private resolve(): ObjectReference {
    const refr = ObjectReference.from(Game.getFormEx(this.reference.id));
    if (!refr || refr.isDeleted()) throw Error('Spawn reference disappeared');
    return refr;
  }

  private enable(appearance: Appearance | null): void {
    this.lifetime.enqueue(this.reference, () => {
      const refr = this.resolve(), actor = Actor.from(refr);
      if (actor && appearance) applyTints(actor, appearance);
      if (this.lifetime.isCurrent(this.reference)) return refr.enable(false);
    }, () => this.finish());
  }

  private finish(): void {
    this.lifetime.enqueue(this.reference, () => {
      const refr = this.resolve(), actor = Actor.from(refr);
      if (refr.isDisabled()) throw Error('Native spawn enable did not take effect');
      if (Date.now() - this.started > 30000) throw Error('Native spawn model did not load before its deadline');
      if (!refr.is3DLoaded()) return;
      this.configured = true;
      if (actor) {
        if (this.isDead) {
          if (!actor.isDead()) { actor.endDeferredKill(); actor.killSilent(null); }
        } else if (actor.isDead()) return actor.resurrect();
        return;
      }
      ObjectReferenceEx.dealWithRef(refr, refr.getBaseObject()!, false);
      if (this.lifetime.isCurrent(this.reference)) return refr.setMotionType(MotionType.Keyframed, true);
    }, () => {
      if (!this.configured) return this.finish();
      // Enabling/loading an actor can replace its disabled placement with the
      // engine's PlaceAtMe position. Apply the target again to the loaded copy.
      this.lifetime.enqueue(this.reference, () => this.resolve().setPosition(...this.position), () => this.observe());
    });
  }

  private observe(): void {
    this.lifetime.enqueue(this.reference, () => {
      const refr = this.resolve(), actor = Actor.from(refr);
      const position = [refr.getPositionX(), refr.getPositionY(), refr.getPositionZ()];
      const distance = position.reduce((sum, value, index) => sum + (value - this.position[index]) ** 2, 0);
      const disabled = refr.isDisabled(), loaded = refr.is3DLoaded(), dead = actor?.isDead();
      if (Date.now() - this.started > 30000) throw Error('Native spawn did not become observable before its deadline: ' +
        JSON.stringify({ disabled, loaded, dead, expectedDead: this.isDead, position, target: this.position }));
      const matches = !disabled && loaded && distance <= 128 ** 2 && (!actor || dead === this.isDead);
      this.stableSamples = matches ? this.stableSamples + 1 : 0;
    }, () => {
      if (this.stableSamples >= 2) this.lifetime.enqueue(this.reference, () => {
        const actor = Actor.from(this.resolve());
        if (actor && this.originalAI !== undefined) actor.enableAI(this.originalAI);
      }, () => {
        const actor = Actor.from(this.resolve());
        if (actor && this.originalAI !== undefined && actor.isAIEnabled() !== this.originalAI)
          throw Error('Native spawn did not restore its original AI state');
        this.callback();
      });
      else this.observe();
    });
  }
}
