import { ObjectReference, Game, Actor, MotionType } from "skyrimPlatform";
import { Appearance, applyTints } from "../sync/appearance";
import { NiPoint3 } from "../sync/movement";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { OwnedReference, OwnedReferenceLifetime } from '../campaign/ownedReferenceLifetime';

export class SpawnProcess {
  constructor(
    appearance: Appearance | null,
    pos: NiPoint3,
    private reference: OwnedReference,
    private lifetime: OwnedReferenceLifetime,
    private callback: () => void,
    rotation?: NiPoint3,
  ) {
    const position = [...pos] as NiPoint3;
    if (rotation) {
      const angles = [...rotation] as NiPoint3;
      lifetime.enqueue(reference, () => this.resolve().setAngle(...angles));
    }
    lifetime.enqueue(reference, () => this.resolve().setPosition(...position), () => this.enable(appearance));
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
      if (actor) return actor.resurrect();
      ObjectReferenceEx.dealWithRef(refr, refr.getBaseObject()!, false);
      if (this.lifetime.isCurrent(this.reference)) return refr.setMotionType(MotionType.Keyframed, true);
    }, this.callback);
  }
}
