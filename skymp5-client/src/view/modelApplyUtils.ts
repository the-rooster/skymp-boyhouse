import { ObjectReference, Game, FormType, TextureSet, NetImmerse } from "skyrimPlatform";
import { Inventory, applyInventory } from "../sync/inventory";
import { logError, logTrace } from "../logging";
import { SpApiInteractor } from '../services/spApiInteractor';
import { NativeReferenceOperationService } from '../services/services/nativeReferenceOperationService';
import { SetNodeScaleEntry, SetNodeTextureSetEntry } from "src/services/messages/createActorMessage";

// For 0xff000000+ used from FormView
// For objects from master files used directly from remoteServer.ts
export class ModelApplyUtils {
  private static get operations(): NativeReferenceOperationService {
    return SpApiInteractor.getControllerInstance().lookupListener(NativeReferenceOperationService);
  }

  static applyModelInventory(refr: ObjectReference, inventory: Inventory) {
    applyInventory(refr, inventory, false, true);
  }

  static applyModelIsOpen(refr: ObjectReference, isOpen: boolean) {
    refr.setOpen(isOpen);

    // See also objectReferenceEx.ts
    const caveGSecretDoor01 = 0x6f703;

    // TODO: add more activators to support more cells
    const parentActivatorId = 0x460ca;

    if (refr.getBaseObject()?.getFormID() === caveGSecretDoor01) {
      const openOrOpening = [1, 2].includes(refr.getOpenState());
      if (openOrOpening) {
        if (!isOpen) {
          refr.activate(ObjectReference.from(Game.getForm(parentActivatorId)), false);
        }
      }
      if (!openOrOpening) {
        if (isOpen) {
          refr.activate(ObjectReference.from(Game.getForm(parentActivatorId)), false);
        }
      }
    }
  }

  static applyModelIsDisabled(refr: ObjectReference, disabled: boolean, current: () => boolean = () => true): void {
    this.operations.submit(refr.getFormID(), 'visibility', current, [reference => {
      if (reference.isDisabled() !== disabled) return disabled ? reference.disable(false) : reference.enable(false);
    }, reference => {
      if (reference.isDisabled() !== disabled) throw Error('Native visibility did not reach the requested state');
    }]);
  }

  static applyModelIsHarvested(refr: ObjectReference, harvested: boolean, current: () => boolean = () => true): void {
    let refresh = false, plant = false, expectedDisabled = false;
    this.operations.submit(refr.getFormID(), 'harvest', current, [reference => {
      const type = reference.getBaseObject()?.getType();
      plant = type === FormType.Tree || type === FormType.Flora;
      expectedDisabled = reference.isDisabled();
      if (plant) {
        if (reference.isHarvested() === harvested) return;
        reference.setHarvested(harvested);
        // Refresh the model without triggering a nearby NPC's activation
        // scripts or changing a reference that was already disabled.
        if (!reference.isDisabled()) { refresh = true; return reference.disable(false); }
      } else if (reference.isDisabled() !== harvested) {
        return harvested ? reference.disable(false) : reference.enable(false);
      }
    }, reference => {
      if (refresh) {
        if (!reference.isDisabled()) throw Error('Harvest refresh disable did not complete');
        return reference.enable(false);
      }
    }, reference => {
      if (plant ? reference.isHarvested() !== harvested || reference.isDisabled() !== expectedDisabled :
        reference.isDisabled() !== harvested) throw Error('Native harvest did not reach the requested state');
    }]);
  }

  static applyModelNodeTextureSet(refr: ObjectReference, setNodeTextureSet?: SetNodeTextureSetEntry[]) {
    if (setNodeTextureSet) {
      setNodeTextureSet.forEach(element => {
        const firstPerson = false;

        const textureSet = TextureSet.from(Game.getFormEx(element.textureSetId));
        if (textureSet !== null) {
          NetImmerse.setNodeTextureSet(refr, element.nodeName, textureSet, firstPerson);
          logTrace("ModelApplyUtils", refr.getFormID().toString(16), `Applied texture set`, element.textureSetId.toString(16), `to`, element.nodeName);
        } else {
          logError("ModelApplyUtils", refr.getFormID().toString(16), `Failed to apply texture set`, element.textureSetId.toString(16), `to`, element.nodeName);
        }
      });
    }
  }

  static applyModelNodeScale(refr: ObjectReference, setNodeScale?: SetNodeScaleEntry[]) {
    if (setNodeScale) {
      setNodeScale.forEach(element => {
        const firstPerson = false;
        NetImmerse.setNodeScale(refr, element.nodeName, element.scale, firstPerson);
        logTrace("ModelApplyUtils", refr.getFormID().toString(16), `Applied node scale`, element.scale, `to`, element.nodeName);
      });
    }
  }
}
