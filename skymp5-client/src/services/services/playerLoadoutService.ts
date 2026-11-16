import { Equipment, applyPlayerEquipment, isPlayerEquipmentSettled, isBadMenuShown } from '../../sync/equipment';
import { Inventory, applyInventory, isInventorySettled } from '../../sync/inventory';
import { Appearance, getAppearance } from '../../sync/appearance';
import { CreateActorMessage } from '../messages/createActorMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';

type CreationPhase = 'idle' | 'pending' | 'preparing' | 'opening' | 'editing' | 'capturing' | 'confirming' | 'failed';
export interface PendingPlayerAppearance { generation: number; actorIdx: number; data: Appearance; }
interface LoadoutState {
  version: 1;
  generation: number;
  actorIdx?: number;
  actorId?: number;
  ready: boolean;
  creation: CreationPhase;
  openAfter: number;
  inventory?: Inventory;
  equipment?: Equipment;
  inventoryPending: boolean;
  equipmentPending: boolean;
  lastInventoryApply: number;
  pendingAppearance?: PendingPlayerAppearance;
  creationCompletionPending?: boolean;
  deadline?: number;
  pausedAt?: number;
  closedSamples?: number;
  error?: string;
  initialApplied?: boolean;
  previewPrepared?: boolean;
  inventorySamples?: number;
  equipmentSamples?: number;
  lastInventoryAttempt?: number;
  lastEquipmentAttempt?: number;
}

const stateKey = 'playerLoadoutState';

// The authoritative echo may use a different JSON property order. Every
// appearance field must match; a different actor's appearance is not an ack.
const sameAppearance = (a: Appearance | undefined, b: Appearance | undefined): boolean => {
  if (!a || !b) return false;
  const canonical = (value: any): any => Array.isArray(value) ? value.map(canonical) :
    value && typeof value === 'object' ? Object.keys(value).sort().reduce((result, key) => {
      result[key] = canonical(value[key]); return result;
    }, {} as Record<string, unknown>) : value;
  return JSON.stringify(canonical(a)) === JSON.stringify(canonical(b));
};

// Owns player loadout application, including the character-creation preview.
// Replication may update snapshots while paused, but only this update handler
// applies them. Spawn callbacks carry a generation and never retain snapshots.
export class PlayerLoadoutService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    const stored = sp.storage[stateKey] as LoadoutState | undefined;
    this.state = stored?.version === 1 ? stored : this.emptyState(0);
    sp.storage[stateKey] = this.state;

    controller.on('update', () => this.onUpdate());
    controller.on('tick', () => {
      if (!this.projectionBlocked && this.state.deadline && Date.now() >= this.state.deadline)
        this.fail('Character creation ' + this.state.creation + ' timed out');
    });
    controller.on('menuOpen', event => {
      if (event.name === 'RaceSex Menu' && this.state.actorId !== undefined &&
          ['pending', 'preparing', 'opening', 'editing'].includes(this.state.creation)) {
        this.state.creation = 'editing'; this.state.deadline = undefined; this.state.closedSamples = 0;
      }
    });
    controller.on('menuClose', event => {
      if (event.name === 'RaceSex Menu' && this.state.creation === 'editing') {
        this.state.creation = 'capturing'; this.state.closedSamples = 0;
        this.setDeadline();
      }
    });
    controller.emitter.on('campaignProjectionPending', () => {
      this.projectionBlocked = true;
      if (this.state.deadline && this.state.pausedAt === undefined) this.state.pausedAt = Date.now();
    });
    controller.emitter.on('campaignProjectionReady', () => {
      this.projectionBlocked = false;
      if (this.state.deadline && this.state.pausedAt !== undefined)
        this.state.deadline += Date.now() - this.state.pausedAt;
      this.state.pausedAt = undefined;
    });
    for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied'] as const) {
      controller.emitter.on(event, () => { this.projectionBlocked = false; this.reset(); });
    }
    controller.emitter.on('gameLoad', event => {
      if (!event.isCausedBySkyrimPlatform) { this.projectionBlocked = false; this.reset(); }
    });
    controller.emitter.on('setRaceMenuOpenMessage', ({ message }) => {
      if (message.open) {
        if (this.state.creation === 'idle') {
          this.state.creation = 'pending'; this.state.previewPrepared = false;
          this.state.creationCompletionPending = false; this.state.deadline = undefined;
        }
      } else if (this.state.creation === 'pending' || this.state.creation === 'preparing') {
        // Once dispatched, wait for the native menu's actual open/close cycle.
        this.state.creation = 'idle';
        this.state.equipmentPending = !!this.state.equipment;
        this.state.inventoryPending = !!this.state.inventory;
        this.state.equipmentSamples = 0; this.state.inventorySamples = 0;
      }
    });
    controller.emitter.on('updateAppearanceMessage', ({ message }) => {
      const pending = this.state.pendingAppearance;
      if (this.state.creation === 'confirming' && pending &&
          message.idx === this.state.actorIdx && sameAppearance(message.data || undefined, pending.data)) {
        this.confirmAppearance();
      }
    });
    controller.emitter.on('setInventoryMessage', ({ message }) => this.setInventory(message.inventory));
    controller.emitter.on('updateEquipmentMessage', ({ message }) => {
      if (message.idx === this.state.actorIdx) {
        this.state.equipment = message.data;
        // Normal play owns its local equips. Do not reapply an echoed packet
        // and create another equip event; only replace a queued restoration.
        if (!this.state.ready || this.state.creation !== 'idle' || this.state.equipmentPending) {
          this.state.equipmentPending = true;
          this.state.equipmentSamples = 0;
          this.state.lastEquipmentAttempt = undefined;
          this.state.inventoryPending = !!this.state.inventory;
        }
      }
    });
    controller.emitter.on('destroyActorMessage', ({ message }) => {
      if (message.idx === this.state.actorIdx) this.reset();
    });
  }

  beginSpawn(message: CreateActorMessage): number {
    const previous = this.state;
    const editing = ['opening', 'editing', 'capturing', 'confirming'].includes(previous.creation);
    if (editing && (previous.actorId === undefined || previous.actorId !== message.refrId))
      throw Error('Cannot replace the player during character creation');
    const preserve = this.projectionBlocked && previous.actorId !== undefined && previous.actorId === message.refrId &&
      previous.creation !== 'failed' && (previous.creation !== 'idle' || previous.creationCompletionPending);
    this.reset();
    this.state.actorIdx = message.idx;
    this.state.actorId = message.refrId;
    this.state.creation = message.props?.isRaceMenuOpen ? 'pending' : 'idle';
    if (preserve) {
      this.state.creation = previous.creation;
      this.state.openAfter = previous.openAfter;
      this.state.deadline = previous.deadline;
      this.state.pausedAt = previous.pausedAt;
      this.state.closedSamples = previous.closedSamples;
      this.state.creationCompletionPending = previous.creationCompletionPending;
      this.state.previewPrepared = previous.previewPrepared;
      if (previous.pendingAppearance) this.state.pendingAppearance = {
        ...previous.pendingAppearance, generation: this.state.generation, actorIdx: message.idx,
      };
      if (message.props?.isRaceMenuOpen && previous.creation === 'idle') {
        this.state.creation = 'pending'; this.state.creationCompletionPending = false;
        this.state.previewPrepared = false; this.state.deadline = undefined;
      } else if (!message.props?.isRaceMenuOpen) {
        if (this.state.pendingAppearance && sameAppearance(message.appearance, this.state.pendingAppearance.data))
          this.confirmAppearance();
        else if (editing) throw Error('Authoritative baseline revoked an unfinished character creation');
        else this.state.creation = 'idle';
      }
    }
    this.state.equipment = message.equipment;
    this.state.equipmentPending = !!message.equipment;
    if (message.props?.inventory) this.setInventory(message.props.inventory);
    return this.state.generation;
  }

  markSpawnReady(generation: number): void {
    if (generation === this.state.generation && this.state.actorIdx !== undefined) {
      this.state.ready = true;
    }
  }

  cancelSpawn(generation: number): void {
    if (generation !== this.state.generation) return;
    if (this.projectionBlocked && this.state.creation !== 'failed' &&
        (this.state.creation !== 'idle' || this.state.creationCompletionPending)) {
      // Keep the draft across a same-player baseline replacement. A new
      // generation/actor index is assigned only after validating that baseline.
      this.state.ready = false; this.state.actorIdx = undefined;
      ++this.state.generation;
    } else this.reset();
  }

  get pendingAppearance(): PendingPlayerAppearance | undefined {
    return this.state.ready && !this.projectionBlocked && this.state.creation === 'confirming'
      ? this.state.pendingAppearance : undefined;
  }

  get blocksGameplayInput(): boolean {
    return (this.state.actorId !== undefined || this.state.actorIdx !== undefined) && (!this.state.ready || this.state.creation !== 'idle' ||
      !!this.state.creationCompletionPending || !this.state.initialApplied);
  }

  get generation(): number { return this.state.generation; }

  failCreation(error: string): void { this.fail(error); }

  get creationMenuActive(): boolean {
    return ['preparing', 'opening', 'editing', 'capturing'].includes(this.state.creation);
  }

  get projectionReadiness(): { ready: boolean; error?: string } {
    // Human editing happens after the world transfer or survives a same-actor
    // replacement. It must not hold the projection acknowledgement deadline.
    return { ready: this.state.ready && !this.state.error &&
      (this.state.creation !== 'idle' || (!this.state.inventoryPending && !this.state.equipmentPending)),
      error: this.state.error };
  }

  shouldApplySpawnAppearance(generation: number): boolean {
    return generation === this.state.generation &&
      !['opening', 'editing', 'capturing', 'confirming'].includes(this.state.creation);
  }

  get preservesCreationAppearance(): boolean {
    return ['opening', 'editing', 'capturing', 'confirming'].includes(this.state.creation) ||
      !!this.state.creationCompletionPending;
  }

  get blocksEquipmentInput(): boolean {
    return !this.state.ready || this.state.creation !== 'idle' ||
      this.state.equipmentPending || this.state.inventoryPending ||
      this.sp.Ui.isMenuOpen('RaceSex Menu');
  }

  private setInventory(inventory: Inventory): void {
    this.state.inventory = inventory;
    this.state.inventoryPending = true;
    this.state.inventorySamples = 0;
    this.state.equipmentSamples = 0;
    this.state.lastInventoryAttempt = undefined;
    this.sp.storage.pcInv = inventory;
  }

  private onUpdate(): void {
    const state = this.state;
    try { this.applyUpdate(); } catch (error) {
      if (this.state === state) this.fail(String(error));
    }
  }

  private applyUpdate(): void {
    const state = this.state;
    if (!state.ready || state.actorIdx === undefined || state.creation === 'failed') return;
    const player = this.sp.Game.getPlayer();
    if (!player) return;
    const shown = this.sp.Ui.isMenuOpen('RaceSex Menu');

    if (shown) {
      if (state.creation === 'confirming' || state.creationCompletionPending)
        throw Error('Character menu reopened before creation completed');
      if (!state.previewPrepared) { player.unequipAll(); state.previewPrepared = true; }
      state.creation = 'editing'; state.deadline = undefined; state.closedSamples = 0;
      return;
    }
    if (state.creation === 'pending') {
      if (this.projectionBlocked) return;
      player.unequipAll();
      state.previewPrepared = true;
      state.creation = 'preparing';
      state.openAfter = Date.now() + 300;
      return;
    }
    if (state.creation === 'preparing') {
      if (this.projectionBlocked) return;
      if (Date.now() >= state.openAfter) {
        state.creation = 'opening';
        this.setDeadline();
        this.sp.Game.showRaceMenu();
      }
      return;
    }
    if (state.creation === 'opening') return; // Wait until the menu was actually observed.
    if (state.creation === 'editing' || state.creation === 'capturing') {
      if (state.creation === 'editing') { state.creation = 'capturing'; this.setDeadline(); }
      state.closedSamples = (state.closedSamples || 0) + 1;
      if (state.closedSamples < 2) return;
      const data = getAppearance(player);
      if (this.state !== state) return;
      state.pendingAppearance = { generation: state.generation, actorIdx: state.actorIdx, data };
      state.creation = 'confirming'; this.setDeadline();
      return;
    }
    if (state.creation === 'confirming') return;
    if (!state.initialApplied && !state.deadline) this.setDeadline();
    if (isBadMenuShown() || this.sp.Utility.isInMenuMode() || !player.is3DLoaded()) return;

    const inventoryDue = state.inventory &&
      (state.inventoryPending || state.equipmentPending || Date.now() - state.lastInventoryApply > 5000);
    if (!state.equipmentPending && !inventoryDue) { this.finishCreation(); return; }
    let blocked = false;
    this.controller.emitter.emit('queryBlockSetInventoryEvent', { block: () => blocked = true });
    if (blocked || this.state !== state || state.creation !== 'idle') return;

    if (state.inventory && inventoryDue) {
      const inventory = state.inventory;
      const settled = isInventorySettled(player, inventory);
      if (this.state !== state || state.inventory !== inventory) return;
      if (!settled) {
        state.inventoryPending = true; state.inventorySamples = 0; state.equipmentSamples = 0;
        // Do not retry additive inventory work every frame, or inspect it as
        // completed in the same update that dispatched the native calls.
        if (state.lastInventoryAttempt === undefined || Date.now() - state.lastInventoryAttempt >= 250) {
          state.lastInventoryAttempt = Date.now();
          applyInventory(player, inventory, false, true);
        }
        return;
      }
      if (state.inventoryPending && (state.inventorySamples = (state.inventorySamples || 0) + 1) < 2) return;
      state.inventoryPending = false;
      state.lastInventoryApply = Date.now();
    }
    if (state.equipmentPending && state.equipment) {
      const equipment = state.equipment;
      const settled = isPlayerEquipmentSettled(player, equipment);
      if (this.state !== state || state.equipment !== equipment) return;
      if (!settled) {
        state.equipmentSamples = 0;
        if (state.lastEquipmentAttempt === undefined || Date.now() - state.lastEquipmentAttempt >= 250) {
          state.lastEquipmentAttempt = Date.now();
          applyPlayerEquipment(player, equipment);
        }
        return;
      }
      if ((state.equipmentSamples = (state.equipmentSamples || 0) + 1) < 2) return;
      state.equipmentPending = false;
    }
    this.finishCreation();
  }

  private setDeadline(): void {
    this.state.deadline = Date.now() + 30000;
    this.state.pausedAt = this.projectionBlocked ? Date.now() : undefined;
  }

  private confirmAppearance(): void {
    this.state.pendingAppearance = undefined;
    this.state.creation = 'idle'; this.state.creationCompletionPending = true;
    this.state.equipmentPending = !!this.state.equipment;
    this.state.inventoryPending = !!this.state.inventory;
    this.state.equipmentSamples = 0; this.state.inventorySamples = 0;
    this.state.lastEquipmentAttempt = undefined; this.state.lastInventoryAttempt = undefined;
    this.setDeadline();
  }

  private finishCreation(): void {
    if (this.state.ready && this.state.creation === 'idle' && !this.state.equipmentPending && !this.state.inventoryPending) {
      this.state.initialApplied = true;
      this.state.deadline = undefined;
    }
    if (this.state.creationCompletionPending && this.state.initialApplied &&
        !this.state.equipmentPending && !this.state.inventoryPending) {
      this.state.creationCompletionPending = false; this.state.deadline = undefined;
      this.state.previewPrepared = false;
      this.controller.emitter.emit('playerCreationComplete');
    }
  }

  private fail(error: string): void {
    if (this.state.creation === 'failed') return;
    this.state.creation = 'failed'; this.state.error = error; this.state.deadline = undefined;
    this.controller.emitter.emit('playerLoadoutFailed', { generation: this.state.generation, error });
  }

  private reset(): void {
    this.state = this.emptyState(this.state.generation + 1);
    this.sp.storage[stateKey] = this.state;
    delete this.sp.storage.pcInv;
  }

  private emptyState(generation: number): LoadoutState {
    return { version: 1, generation, ready: false, creation: 'idle', openAfter: 0,
      inventoryPending: false, equipmentPending: false, lastInventoryApply: 0 };
  }

  private state: LoadoutState;
  private projectionBlocked = false;
}
