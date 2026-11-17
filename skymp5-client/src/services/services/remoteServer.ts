// @ts-expect-error (TODO: Remove in 2.10.0)
import { Actor, Form, FormType, Menu, interruptCast, castSpellImmediate, printConsole, applyAnimationVariablesToActor, ActorAnimationVariables } from 'skyrimPlatform';
import {
  Cell,
  Game,
  ObjectReference,
  TESModPlatform,
  Ui,
  WorldSpace,
  storage, // TODO: use this.sp.storage instead
} from 'skyrimPlatform';

import * as messages from '../../messages';

/* eslint-disable @typescript-eslint/no-empty-function */
import { ObjectReferenceEx } from '../../extensions/objectReferenceEx';
import { IdManager } from '../../lib/idManager';
import { nameof } from '../../lib/nameof';
import { setActorValuePercentage } from '../../sync/actorvalues';
import { applyAppearanceToPlayer } from '../../sync/appearance';
import { Inventory } from '../../sync/inventory';
import { Movement } from '../../sync/movement';
import { ModelApplyUtils } from '../../view/modelApplyUtils';
import { FormModel, WorldModel } from '../../view/model';
import { UpdateMovementMessage } from '../messages/updateMovementMessage';
import { ChangeValuesMessage } from '../messages/changeValuesMessage';
import { UpdateAnimationMessage } from '../messages/updateAnimationMessage';
import { UpdateEquipmentMessage } from '../messages/updateEquipmentMessage';
import { RagdollService } from './ragdollService';
import { UpdateAppearanceMessage } from '../messages/updateAppearanceMessage';
import { TeleportMessage } from '../messages/teleportMessage';
import { DeathStateContainerMessage } from '../messages/deathStateContainerMessage';
import { RespawnNeededError } from '../../lib/errors';
import { OpenContainerMessage } from '../messages/openContainerMessage';
import { ActivateMessage } from '../messages/activateMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { HostStartMessage } from '../messages/hostStartMessage';
import { HostStopMessage } from '../messages/hostStopMessage';
import { ConnectionMessage } from '../events/connectionMessage';
import { CreateActorMessage, CreateActorMessageAdditionalProps } from '../messages/createActorMessage';
import { DestroyActorMessage } from '../messages/destroyActorMessage';
import { UpdatePropertyMessage } from '../messages/updatePropertyMessage';
import { TeleportMessage2 } from '../messages/teleportMessage2';

// TODO: refactor worldViewMisc into service
import {
  getObjectReference,
  getViewFromStorage,
  remoteIdToLocalId,
} from '../../view/worldViewMisc';
import { logTrace, logError } from '../../logging';

import { SpellCastMessage } from '../messages/spellCastMessage';
import { UpdateAnimVariablesMessage } from '../messages/updateAnimVariablesMessage';
import { PlayerSpawnService } from './playerSpawnService';
import { PlayerLoadoutService } from './playerLoadoutService';
import { campaignFactionRanks, campaignFactionsFromBaseline, campaignFactionsProperty } from '../../sync/campaignFactions';
import { ActorFactionsService } from './actorFactionsService';
import { LoadGameService } from './loadGameService';

export const getPcInventory = (): Inventory | undefined => {
  const res = storage['pcInv'];
  if (typeof res === 'object' && (res as any)['entries']) {
    return res as Inventory;
  }
  return undefined;
};

export class RemoteServer extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    const key = 'boyhouseRemoteWorldLifetime';
    const old = sp.storage[key] as { generation: number } | undefined;
    this.lifetime = { generation: (old?.generation || 0) + 1 };
    sp.storage[key] = this.lifetime;
    const originalState = sp.storage['boyhouseRemoteOriginals'] as { version: number; references: Map<number, { idx: number }> } | undefined;
    this.originals = originalState?.version === 1 ? originalState.references : new Map();
    sp.storage['boyhouseRemoteOriginals'] = { version: 1, references: this.originals };
    const ids = sp.storage['idManager'] as IdManager | undefined;
    if (ids?.version !== 2) sp.storage['idManager'] = new IdManager(this.worldModel.forms.map(form => form?.idx));
    controller.on('preLoadGame', () => {
      this.loading = true;
      // Initial server packets target the generated world being loaded. A
      // personal save must discard them before its first native update.
      if (!controller.lookupListener(LoadGameService).isLoadingPlatformSave) this.resetWorldModel();
    });
    controller.on('postLoadGame', (event?: { success: boolean }) => {
      if (event?.success === false) {
        this.loading = false;
        // Drop deferred bootstrap effects before the failed-load disconnect.
        ++this.lifetime.generation;
      }
    });
    controller.emitter.on('gameLoad', event => {
      this.loading = false;
      if (!event.isCausedBySkyrimPlatform) this.resetWorldModel();
    });
    for (const event of ['connectionDisconnect', 'connectionDenied', 'connectionFailed'] as const)
      controller.emitter.on(event, () => this.resetWorldModel());

    this.controller.emitter.on("hostStartMessage", (e) => this.onHostStartMessage(e));
    this.controller.emitter.on("hostStopMessage", (e) => this.onHostStopMessage(e));
    this.controller.emitter.on("openContainerMessage", (e) => this.onOpenContainerMessage(e));
    this.controller.emitter.on("updateMovementMessage", (e) => this.onUpdateMovementMessage(e));
    this.controller.emitter.on("updateAnimationMessage", (e) => this.onUpdateAnimationMessage(e));
    this.controller.emitter.on("updateEquipmentMessage", (e) => this.onUpdateEquipmentMessage(e));
    this.controller.emitter.on("changeValuesMessage", (e) => this.onChangeValuesMessage(e));
    this.controller.emitter.on("updateAppearanceMessage", (e) => this.onUpdateAppearanceMessage(e));
    this.controller.emitter.on("teleportMessage", (e) => this.onTeleportMessage(e));
    this.controller.emitter.on("teleportMessage2", (e) => this.onTeleportMessage(e));
    this.controller.emitter.on("createActorMessage", (e) => this.onCreateActorMessage(e));
    this.controller.emitter.on("destroyActorMessage", (e) => this.onDestroyActorMessage(e));
    this.controller.emitter.on("updatePropertyMessage", (e) => this.onUpdatePropertyMessage(e));
    this.controller.emitter.on("deathStateContainerMessage", (e) => this.onDeathStateContainerMessage(e));

    this.controller.emitter.on("connectionAccepted", () => this.handleConnectionAccepted());

    this.controller.emitter.on("spellCastMessage", (e) => this.onSpellCastMessage(e));
    this.controller.emitter.on("updateAnimVariablesMessage", (e) => this.onUpdateAnimVariablesMessage(e));

  }

  // Called before installing another authoritative baseline. It invalidates
  // queued native effects as well as model, index and hosting state.
  resetWorldModel(): void {
    ++this.lifetime.generation;
    this.originals.clear();
    this.originalApplications.clear();
    this.actorValueApplications.clear();
    this.sp.storage['worldModel'] = { forms: [], playerCharacterFormIdx: -1, playerCharacterRefrId: 0 };
    this.sp.storage['idManager'] = new IdManager();
    this.sp.storage['hosted'] = [];
    this.controller.lookupListener(PlayerSpawnService).cancel();
    this.controller.emitter.emit('worldModelReset');
  }

  private capture(): () => boolean {
    const generation = this.lifetime.generation;
    return () => this.sp.storage['boyhouseRemoteWorldLifetime'] === this.lifetime &&
      generation === this.lifetime.generation && this.sp.mpClientPlugin.isConnected();
  }

  private captureActor(idx: number): () => boolean {
    const current = this.capture(), id = this.getIdManager().getId(idx), form = this.worldModel.forms[id];
    return () => current() && !!form && this.getIdManager().getId(idx) === id && this.worldModel.forms[id] === form;
  }

  private captureOriginal(refrId: number): () => boolean {
    const current = this.capture(), identity = this.originals.get(refrId);
    return () => current() && !!identity && this.originals.get(refrId) === identity;
  }

  private defer(callback: () => void, current = this.capture()): void {
    this.controller.once('update', () => {
      if (!current()) return;
      if (this.loading) this.defer(callback, current);
      else callback();
    });
  }

  private captureRemote(refrId: number): () => boolean {
    const form = refrId === 0x14 ? this.worldModel.forms[this.getMyActorIndex()] :
      this.worldModel.forms.find(form => form?.refrId === refrId || form?.refrId === refrId + 0x100000000);
    return form?.idx !== undefined ? this.captureActor(form.idx) : this.captureOriginal(refrId);
  }

  private lifetime: { generation: number };
  private loading = false;
  private originals = new Map<number, { idx: number }>();
  private originalApplications = new Map<number, { pending: boolean; error?: string }>();
  private actorValueApplications = new Set<{ current: () => boolean; pending: boolean; error?: string }>();

  getActorValueApplicationProgress(): { ready: boolean; error?: string } {
    // Selecting another projection epoch resets this world generation. Actor
    // replacement also retires its queued writes, even when an idx is reused.
    this.actorValueApplications.forEach(application => {
      if (!application.current()) this.actorValueApplications.delete(application);
    });
    const entries = Array.from(this.actorValueApplications);
    const error = entries.find(entry => entry.error)?.error;
    return { ready: !error && entries.every(entry => !entry.pending), error };
  }

  getOriginalApplicationProgress(): { ready: boolean; error?: string } {
    const entries = Array.from(this.originalApplications.values());
    const error = entries.find(entry => entry.error)?.error;
    return { ready: !error && entries.every(entry => !entry.pending), error };
  }

  private onHostStartMessage(event: ConnectionMessage<HostStartMessage>) {
    const msg = event.message;
    const target = msg.target;

    let hosted = storage['hosted'];
    if (typeof hosted !== typeof []) {
      // if you try to switch to Set please checkout .concat usage.
      // concat compiles but doesn't work as expected
      hosted = new Array<number>();
      storage['hosted'] = hosted;
    }

    if (!(hosted as Array<unknown>).includes(target)) {
      (hosted as Array<unknown>).push(target);
    }
  }

  private onHostStopMessage(event: ConnectionMessage<HostStopMessage>) {
    const msg = event.message;
    const target = msg.target;
    logTrace(this, 'hostStop ' + target.toString(16));

    const hosted = storage['hosted'] as Array<number>;
    if (typeof hosted === typeof []) {
      storage['hosted'] = hosted.filter((x) => x !== target);
    }
  }

  private onOpenContainerMessage(event: ConnectionMessage<OpenContainerMessage>): void {
    const msg = event.message, epoch = this.captureRemote(event.message.target);
    const request = ++this.openRequest;
    const current = () => epoch() && request === this.openRequest;
    const activateAfter = Date.now() + 100; // Let preceding inventory updates apply.
    const activate = () => {
      if (Date.now() < activateAfter) { this.defer(activate, current); return; }
      const localId = remoteIdToLocalId(msg.target);
      const handle = this.sp.getReferenceHandle(localId);
      const reference = ObjectReference.from(Game.getFormEx(localId));
      if (!reference || !handle) return;
      const sameReference = () => current() && remoteIdToLocalId(msg.target) === localId &&
        this.sp.getReferenceHandle(localId) === handle;
      const type = reference.getBaseObject()?.getType();
      reference.activate(Game.getPlayer(), true);
      if (!current() || (type !== FormType.Container && type !== FormType.Furniture)) return;
      const started = Date.now();
      let observedOpen = false, nextCheck = 0;
      const poll = () => {
        if (!sameReference()) return;
        const now = Date.now();
        if (now >= nextCheck) {
          nextCheck = now + 100;
          const open = type === FormType.Container ? Ui.isMenuOpen('ContainerMenu') :
            Game.getPlayer()?.getFurnitureReference()?.getFormID() === localId;
          if (open) observedOpen = true;
          else if (observedOpen) {
            this.controller.emitter.emit('sendMessage', {
              message: { t: messages.MsgType.Activate,
                data: { caster: 0x14, target: msg.target, isSecondActivation: true } },
              reliability: 'reliable',
            });
            return;
          } else if (now - started > 10000) return;
        }
        this.defer(poll, current);
      };
      this.defer(poll, current);
    };
    this.defer(activate, current);
  }

  private openRequest = 0;

  private onTeleportMessage(event: ConnectionMessage<TeleportMessage> | ConnectionMessage<TeleportMessage2>): void {
    const msg = event.message;
    const idx = 'idx' in msg && typeof msg.idx === 'number' ? msg.idx : this.worldModel.forms[this.getMyActorIndex()]?.idx;
    if (idx === undefined) return;
    const current = this.captureActor(idx);
    this.defer(() => {
      const id = this.getIdManager().getId(idx);
      const reference = id === this.getMyActorIndex() ? Game.getPlayer() : getObjectReference(id);
      const localId = reference?.getFormID();
      const handle = localId ? this.sp.getReferenceHandle(localId) : 0;
      if (!reference || !localId || !handle) return;
      const move = () => {
        if (!current() || this.sp.getReferenceHandle(localId) !== handle) return;
        const fresh = ObjectReference.from(Game.getFormEx(localId));
        if (!fresh) return;
        TESModPlatform.moveRefrToPosition(fresh, Cell.from(Game.getFormEx(msg.worldOrCell)),
          WorldSpace.from(Game.getFormEx(msg.worldOrCell)), msg.pos[0], msg.pos[1], msg.pos[2],
          msg.rot[0], msg.rot[1], msg.rot[2]);
      };
      const actor = Actor.from(reference);
      if (actor) this.controller.lookupListener(RagdollService).safeRemoveRagdollFromWorld(actor,
        () => this.defer(move, current), current);
      else move();
    }, current);
  }

  private onCreateActorMessage(event: ConnectionMessage<CreateActorMessage>): void {
    const msg = event.message;
    const campaignFactions = campaignFactionsFromBaseline(msg);
    if (campaignFactions !== undefined && this.skipFormViewCreation(msg))
      throw Error('Campaign factions cannot target a shared authored reference');
    this.originals.forEach((entry, id) => {
      if (entry.idx === msg.idx) { this.originals.delete(id); this.originalApplications.delete(id); }
    });
    const previous = this.getIdManager().getId(msg.idx);
    if (previous >= 0) getViewFromStorage()?.getFormViews().destroyForm(previous);
    if (this.skipFormViewCreation(msg)) {
      if (previous >= 0) {
        this.worldModel.forms[previous] = undefined;
        this.getIdManager().freeIdFor(msg.idx);
      }
      const refrId = msg.refrId!;
      this.originals.set(refrId, { idx: msg.idx });
      const application: { pending: boolean; error?: string } = { pending: true };
      this.originalApplications.set(refrId, application);
      const current = this.captureOriginal(refrId);
      this.onceLoad(refrId, (refr: ObjectReference | null) => {
        try {
          if (refr) {
            ObjectReferenceEx.dealWithRef(refr, refr.getBaseObject() as Form, true, current);
            if (msg.props) {
              if (msg.props.inventory) {
                ModelApplyUtils.applyModelInventory(refr, msg.props.inventory);
              }
              ModelApplyUtils.applyModelIsOpen(refr, !!msg.props['isOpen']);
              ModelApplyUtils.applyModelIsHarvested(refr, !!msg.props['isHarvested'], current);

              ModelApplyUtils.applyModelNodeScale(refr, msg.props.setNodeScale);

              ModelApplyUtils.applyModelNodeTextureSet(refr, msg.props.setNodeTextureSet);

              ModelApplyUtils.applyModelIsDisabled(refr, !!msg.props['disabled'], current);

              // TODO: move to a separate module
              const animation = msg.props.lastAnimation;
              if (typeof animation === "string") {
                const refrid = refr.getFormID();

                const handle = this.sp.getReferenceHandle(refrid);
                let attempts = 0, nextAttempt = 0;
                const play = () => {
                  if (!current() || this.sp.getReferenceHandle(refrid) !== handle) return;
                  if (Date.now() >= nextAttempt) {
                    ++attempts; nextAttempt = Date.now() + 2000;
                    if (ObjectReference.from(Game.getFormEx(refrid))?.playAnimation(animation) || attempts >= 5) return;
                  }
                  this.defer(play, current);
                };
                this.defer(play, current);
              }


              let displayName = msg.props.displayName;

              // keep in sync with spSnippetService.ts
              if (typeof displayName === "string") {

                const replaceValue = refr.getBaseObject()?.getName();

                if (replaceValue !== undefined) {
                  displayName = displayName.replace(/%original_name%/g, replaceValue);
                } else {
                  logError(this, "Couldn't get a replaceValue for SetDisplayName, refr.getFormID() was", refr.getFormID().toString(16));
                }

                refr.setDisplayName(displayName, true);
                logTrace(this, `calling setDisplayName`, displayName, `for`, refr.getFormID().toString(16));
              }
            }
          } else {
            application.error = 'Original reference did not load: ' + refrId.toString(16);
            logError(this, 'Failed to apply model to', refrId.toString(16));
          }
        } catch (error) { application.error = String(error); throw error; }
        finally { application.pending = false; }
      }, 120, current);
      return;
    }

    logTrace(this, "Create actor");

    const i = this.getIdManager().allocateIdFor(msg.idx);
    if (this.worldModel.forms.length <= i) {
      this.worldModel.forms.length = i + 1;
    }

    let movement: Movement | undefined = undefined;
    // TODO: better check if it is an npc (not an object reference)
    if (msg.refrId !== undefined && msg.refrId >= 0xff000000) {
      movement = {
        pos: msg.transform.pos,
        rot: msg.transform.rot,
        worldOrCell: msg.transform.worldOrCell,
        runMode: 'Standing',
        direction: 0,
        isInJumpState: false,
        isSneaking: false,
        isBlocking: false,
        isWeapDrawn: false,
        isDead: false,
        healthPercentage: 1.0,
        speed: 0,
      };
    }

    const form: FormModel = {
      idx: msg.idx,
      movement,
      numMovementChanges: 0,
      numAppearanceChanges: 0,
      baseId: msg.baseId,
      refrId: msg.refrId,
      isMyClone: msg.isMe,
    };
    this.worldModel.forms[i] = form;

    if (msg.appearance) {
      form.appearance = msg.appearance;
    }

    if (msg.equipment) {
      form.equipment = msg.equipment;
    }

    if (msg.isDead) {
      form.isDead = msg.isDead;
    }

    if (msg.animation) {
      form.animation = msg.animation;
    }

    if (msg.props) {
      for (const propName in msg.props) {
        (form as Record<string, unknown>)[propName] = msg.props[propName as keyof CreateActorMessageAdditionalProps];
      }
    }

    msg.customPropsJsonDumps.forEach(element => {
      if (element.propName === campaignFactionsProperty) return;
      let parsed: unknown;
      try {
        parsed = JSON.parse(element.propValueJsonDump);
      } catch (e) {
        if (e instanceof SyntaxError) {
          logError(this, "createActor", msg.refrId?.toString(16), "failed to parse custom prop", element.propName, element.propValueJsonDump, e.message);
        } else {
          throw e;
        }
      }
      (form as Record<string, unknown>)[element.propName] = parsed;
    });
    form.campaignFactions = campaignFactions;

    // Reliable death state is authoritative over movement's animation hint.
    // In particular, a dead baseline must not immediately resurrect its copy
    // through the synthetic initial movement record.
    if (form.movement && typeof form.isDead === 'boolean') form.movement.isDead = form.isDead;

    if (msg.isMe) {
      this.worldModel.playerCharacterFormIdx = i;
      this.worldModel.playerCharacterRefrId = msg.refrId || 0;
    }

    // TODO: move to a separate module

    if (msg.props && !msg.props.isHostedByOther) {
    }

    if (msg.isMe) this.controller.lookupListener(PlayerSpawnService).request(msg);
  }

  private onDestroyActorMessage(event: ConnectionMessage<DestroyActorMessage>): void {
    const msg = event.message;

    this.originals.forEach((entry, id) => {
      if (entry.idx === msg.idx) { this.originals.delete(id); this.originalApplications.delete(id); }
    });
    const i = this.getIdManager().getId(msg.idx);
    if (i < 0) return;
    this.worldModel.forms[i] = undefined;
    getViewFromStorage()?.syncFormArray(this.worldModel);

    // Shrink to fit
    while (1) {
      const length = this.worldModel.forms.length;
      if (!length) {
        break;
      }
      if (this.worldModel.forms[length - 1]) {
        break;
      }
      this.worldModel.forms.length = length - 1;
    }

    if (this.worldModel.playerCharacterFormIdx === i) {
      this.worldModel.playerCharacterFormIdx = -1;
      this.worldModel.playerCharacterRefrId = 0;

      // TODO: move to a separate module
      const current = this.capture();
      this.defer(() => {
        if (this.getMyActorIndex() === -1) Game.quitToMainMenu();
      }, current);
    }

    this.getIdManager().freeIdFor(msg.idx);
  }

  private onUpdateMovementMessage(event: ConnectionMessage<UpdateMovementMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateMovementMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.movement = typeof form.isDead === 'boolean' ? { ...msg.data, isDead: form.isDead } : msg.data;
    if (!form.numMovementChanges) {
      form.numMovementChanges = 0;
    }
    form.numMovementChanges++;
  }

  private onUpdateAnimationMessage(event: ConnectionMessage<UpdateAnimationMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateAnimationMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.animation = msg.data;
  }

  private onUpdateAppearanceMessage(event: ConnectionMessage<UpdateAppearanceMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateAppearanceMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.appearance = msg.data || undefined;
    if (!form.numAppearanceChanges) {
      form.numAppearanceChanges = 0;
    }
    form.numAppearanceChanges++;

    const newAppearance = msg.data;

    if (i === this.getMyActorIndex() && newAppearance) {
        // The creator already owns the displayed face. The loadout service
        // consumes its matching server echo as confirmation; reapplying that
        // echo would queue another 3D rebuild after creation controls release.
        if (this.controller.lookupListener(PlayerLoadoutService).preservesCreationAppearance) return;
      const current = this.captureActor(msg.idx);
      this.defer(() => {
        applyAppearanceToPlayer(newAppearance, current);
        logTrace(this, "Applied appearance to the player");
      }, current);
    }
  }

  private onUpdateEquipmentMessage(event: ConnectionMessage<UpdateEquipmentMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateEquipmentMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.equipment = msg.data;
  }

  private onUpdatePropertyMessage(event: ConnectionMessage<UpdatePropertyMessage>): void {
    const msg = event.message;
    const parsed = this.extractUpdatePropertyMessageData(msg);
    const msgData = msg.propName === campaignFactionsProperty ? campaignFactionRanks(parsed) : parsed;
    if (msg.propName === campaignFactionsProperty && this.skipFormViewCreation(msg))
      throw Error('Campaign factions cannot target a shared authored reference');

    if (this.skipFormViewCreation(msg)) {
      const refrId = msg.refrId!;
      const current = this.captureOriginal(refrId);
      this.defer(() => {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (!refr) {
          logError(this, 'UpdateProperty: refr not found');
          return;
        }
        if (msg.propName === 'inventory') {
          ModelApplyUtils.applyModelInventory(refr, msgData as Inventory);
        } else if (msg.propName === 'isOpen') {
          ModelApplyUtils.applyModelIsOpen(refr, !!msgData);
        } else if (msg.propName === 'isHarvested') {
          ModelApplyUtils.applyModelIsHarvested(refr, !!msgData, current);
        } else if (msg.propName === 'disabled') {
          ModelApplyUtils.applyModelIsDisabled(refr, !!msgData, current);
        }
      }, current);
      return;
    }
    const i = this.getIdManager().getId(msg.idx);
    const form = this.worldModel.forms[i];
    if (form) {
      (form as Record<string, unknown>)[msg.propName] = msgData;
      if (msg.propName === campaignFactionsProperty && i === this.worldModel.playerCharacterFormIdx)
        this.controller.lookupListener(ActorFactionsService).updateSelf(msg.idx, msgData);
      if (msg.propName === 'isDead' && typeof msgData === 'boolean' && form.movement) form.movement.isDead = msgData;
    }
  }

  private onDeathStateContainerMessage(event: ConnectionMessage<DeathStateContainerMessage>): void {
    const msg = event.message;

    logTrace(this, `Received death state:`, JSON.stringify(msg.tIsDead));

    const id = this.getIdManager().getId(msg.tIsDead.idx);
    const form = this.worldModel.forms[id];

    if (form === undefined) {
      logError(this, `onDeathStateContainerMessage - Form with idx`, msg.tIsDead.idx, `not found`);
      return;
    }

    if (msg.tIsDead.propName !== nameof<FormModel>('isDead')) {
      logError(this, `onDeathStateContainerMessage - Invalid propName`, msg.tIsDead.propName);
      return;
    }

    const msgData = this.extractUpdatePropertyMessageData(msg.tIsDead);
    if (typeof msgData !== 'boolean') {
      logError(this, `onDeathStateContainerMessage - Invalid data`, msgData);
      return;
    }

    if (msg.tChangeValues) {
      this.onChangeValuesMessage({ message: msg.tChangeValues });
    }
    const current = this.captureActor(msg.tIsDead.idx);
    this.defer(() => this.onUpdatePropertyMessage({ message: msg.tIsDead }), current);

    if (msg.tTeleport) {
      this.onTeleportMessage({ message: msg.tTeleport });
    }

    this.defer(() => {
      const actor =
        id === this.getWorldModel().playerCharacterFormIdx
          ? Game.getPlayer()!
          : Actor.from(Game.getFormEx(remoteIdToLocalId(form.refrId ?? 0)));
      if (actor) {
        try {
          this.controller.emitter.emit("applyDeathStateEvent", {
            actor: actor,
            isDead: msgData
          });
        } catch (e) {
          if (e instanceof RespawnNeededError) {
            // The view owns temporary-reference cleanup. Never delete an
            // original reference (or the player) from a late death callback.
            if (id !== this.getMyActorIndex()) getViewFromStorage()?.getFormViews().destroyForm(id);
          } else {
            throw e;
          }
        }
      }
    }, current);
  }

  private handleConnectionAccepted(): void {
    this.resetWorldModel();

    logTrace(this, "Handle connection accepted");
  }

  private onChangeValuesMessage(event: ConnectionMessage<ChangeValuesMessage>): void {
    const idx = event.message.idx, values = { ...event.message.data }, current = this.captureActor(idx);
    if (!current()) return;
    if (this.actorValueApplications.size >= 4096) {
      this.getActorValueApplicationProgress();
      if (this.actorValueApplications.size >= 4096) throw Error('Too many pending actor-value applications');
    }
    const application: { current: () => boolean; pending: boolean; error?: string } = { current, pending: true };
    this.actorValueApplications.add(application);
    // A reliable frame is only delivered during epoch draining; its native
    // work runs on a later update. Keep projection/input blocked until that
    // work completes, including through generated-world loading.
    const apply = () => {
      if (!current()) { this.actorValueApplications.delete(application); return; }
      if (this.loading) { this.controller.once('update', apply); return; }
      try {
        const id = this.getIdManager().getId(idx);
        const refr = id === this.getMyActorIndex() ? Game.getPlayer() : getObjectReference(id);
        const ac = Actor.from(refr);
        if (!current()) return;
        if (!ac) throw Error('Actor-value target is unavailable');
        for (const name of ['health', 'stamina', 'magicka'] as const) {
          if (!current()) return;
          const value = values[name];
          if (typeof value !== 'number') continue;
          if (!Number.isFinite(value)) throw Error('Nonfinite actor-value percentage');
          // Restore/DamageActorValue are non-latent; the SP bridge waits for
          // their native completion and throws on failure.
          setActorValuePercentage(ac, name, value);
        }
      } catch (error) {
        if (current()) {
          application.error = String(error);
          logError(this, 'Applying actor values failed', error);
        }
      } finally {
        application.pending = false;
        if (!application.error || !current()) this.actorValueApplications.delete(application);
      }
    };
    try { this.controller.once('update', apply); }
    catch (error) { this.actorValueApplications.delete(application); throw error; }
  }

  /** Packet handlers end **/

  getWorldModel(): WorldModel {
    return this.worldModel;
  }

  getMyActorIndex(): number {
    return this.worldModel.playerCharacterFormIdx;
  }

  getMyRemoteRefrId(): number {
    return this.worldModel.playerCharacterRefrId;
  }

  getIdManager() {
    return this.idManager_;
  }

  private get worldModel(): WorldModel {
    if (!storage['worldModel'] || typeof storage['worldModel'] !== 'object') {
      storage["worldModel"] = { forms: [], playerCharacterFormIdx: -1, playerCharacterRefrId: 0 };
    }
    return storage["worldModel"] as WorldModel;
  }

  private get idManager_(): IdManager {
    if (!storage['idManager'] || typeof storage['idManager'] !== 'object') {
      // Note: full IdManager object preserved across hot-reloads, including methods.
      storage["idManager"] = new IdManager();
    }
    return storage["idManager"] as IdManager;
  }

  private onceLoad(
    refrId: number,
    callback: (refr: ObjectReference | null) => void,
    maxAttempts: number = 120,
    current = this.capture(),
  ) {
    this.defer(() => {
      const refr = ObjectReference.from(Game.getFormEx(refrId));
      if (refr) {
        callback(refr);
      } else {
        maxAttempts--;
        if (maxAttempts > 0) {
          this.defer(() => this.onceLoad(refrId, callback, maxAttempts, current), current);
        } else {
          logError(this, 'Failed to load object reference ' + refrId.toString(16));
          callback(null);
        }
      }
    }, current);
  };

  private skipFormViewCreation(
    msg: UpdatePropertyMessage | CreateActorMessage,
  ) {
    // Optimization added in #1186, however it doesn't work for doors for some reason
    return msg.refrId && msg.refrId < 0xff000000 && msg.baseRecordType !== 'DOOR';
  };

  private extractUpdatePropertyMessageData(updatePropertyMessage: UpdatePropertyMessage) {
    let msgData: unknown = updatePropertyMessage.data;

    if (updatePropertyMessage.dataDump !== undefined) {
      try {
        msgData = JSON.parse(updatePropertyMessage.dataDump);
      } catch (e) {
        if (e instanceof SyntaxError) {
          logError(this, 'extractUpdatePropertyMessageData - Failed to parse dataDump', updatePropertyMessage.dataDump);
          return;
        } else {
          throw e;
        }
      }
    }

    return msgData;
  }

  private onSpellCastMessage(event: ConnectionMessage<SpellCastMessage>): void {
    const msg = event.message;
    const current = this.captureRemote(msg.data.caster);

    this.defer(() => {
      const ac = Actor.from(Game.getFormEx(remoteIdToLocalId(msg.data.caster)));
      if (!ac) {
        return;
      }

      const actorAnimationVariables: ActorAnimationVariables = {
        booleans: new Uint8Array(msg.data.actorAnimationVariables.booleans),
        floats: new Uint8Array(msg.data.actorAnimationVariables.floats),
        integers: new Uint8Array(msg.data.actorAnimationVariables.integers)
      };

      if (msg.data.interruptCast) {
        interruptCast(ac.getFormID(), msg.data.castingSource, actorAnimationVariables);
        return;
      }

      const spell = ac.getEquippedSpell(msg.data.castingSource);
      if (spell) {
        castSpellImmediate(ac.getFormID(), msg.data.castingSource, spell.getFormID(), remoteIdToLocalId(msg.data.target),
          msg.data.aimAngle, msg.data.aimHeading, actorAnimationVariables);
      }
    }, current);
  }

  private onUpdateAnimVariablesMessage(event: ConnectionMessage<UpdateAnimVariablesMessage>): void {
    const msg = event.message;
    const current = this.captureRemote(msg.data.actorRemoteId);

    this.defer(() => {
      const ac = Actor.from(Game.getFormEx(remoteIdToLocalId(msg.data.actorRemoteId)));
      if (!ac) {
        return;
      }

      const actorAnimationVariables: ActorAnimationVariables = {
        booleans: new Uint8Array(msg.data.actorAnimationVariables.booleans),
        floats: new Uint8Array(msg.data.actorAnimationVariables.floats),
        integers: new Uint8Array(msg.data.actorAnimationVariables.integers)
      };

      const isApplyed = applyAnimationVariablesToActor(ac.getFormID(), actorAnimationVariables);

      if (!isApplyed) {
        logError(this, 'Failed apply AnimationVariables to actor with id: ' + ac.getFormID().toString(16));
      }
    }, current);
  }

}
