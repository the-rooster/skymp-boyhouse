import { Equipment, applyEquipment, isBadMenuShown } from '../../sync/equipment';
import { Inventory, applyInventory } from '../../sync/inventory';
import { CreateActorMessage } from '../messages/createActorMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';

type CreationPhase = 'idle' | 'pending' | 'preparing' | 'opening' | 'editing';
interface LoadoutState {
  version: 1;
  generation: number;
  actorIdx?: number;
  ready: boolean;
  creation: CreationPhase;
  openAfter: number;
  inventory?: Inventory;
  equipment?: Equipment;
  inventoryPending: boolean;
  equipmentPending: boolean;
  lastInventoryApply: number;
}

const stateKey = 'playerLoadoutState';

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
    for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied'] as const) {
      controller.emitter.on(event, () => this.reset());
    }
    controller.emitter.on('gameLoad', event => {
      if (!event.isCausedBySkyrimPlatform) this.reset();
    });
    controller.emitter.on('setRaceMenuOpenMessage', ({ message }) => {
      if (message.open) {
        if (this.state.creation === 'idle') this.state.creation = 'pending';
      } else if (this.state.creation === 'pending' || this.state.creation === 'preparing') {
        // Once dispatched, wait for the native menu's actual open/close cycle.
        this.state.creation = 'idle';
        this.state.equipmentPending = !!this.state.equipment;
        this.state.inventoryPending = !!this.state.inventory;
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
          this.state.inventoryPending = !!this.state.inventory;
        }
      }
    });
    controller.emitter.on('destroyActorMessage', ({ message }) => {
      if (message.idx === this.state.actorIdx) this.reset();
    });
  }

  beginSpawn(message: CreateActorMessage): number {
    this.reset();
    this.state.actorIdx = message.idx;
    this.state.creation = message.props?.isRaceMenuOpen ? 'pending' : 'idle';
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
    if (generation === this.state.generation) this.reset();
  }

  get blocksEquipmentInput(): boolean {
    return !this.state.ready || this.state.creation !== 'idle' ||
      this.state.equipmentPending || this.state.inventoryPending ||
      this.sp.Ui.isMenuOpen('RaceSex Menu');
  }

  private setInventory(inventory: Inventory): void {
    this.state.inventory = inventory;
    this.state.inventoryPending = true;
    this.sp.storage.pcInv = inventory;
  }

  private onUpdate(): void {
    const state = this.state;
    if (!state.ready || state.actorIdx === undefined) return;
    const player = this.sp.Game.getPlayer();
    if (!player) return;
    const shown = this.sp.Ui.isMenuOpen('RaceSex Menu');

    if (shown) {
      if (state.creation === 'idle' || state.creation === 'pending') player.unequipAll();
      state.creation = 'editing';
      return;
    }
    if (state.creation === 'pending') {
      player.unequipAll();
      state.creation = 'preparing';
      state.openAfter = Date.now() + 300;
      return;
    }
    if (state.creation === 'preparing') {
      if (Date.now() >= state.openAfter) {
        this.sp.Game.showRaceMenu();
        state.creation = 'opening';
      }
      return;
    }
    if (state.creation === 'opening') return; // Wait until the menu was actually observed.
    if (state.creation === 'editing') {
      state.creation = 'idle';
      state.equipmentPending = !!state.equipment;
      state.inventoryPending = !!state.inventory;
    }
    if (isBadMenuShown()) return;

    const inventoryDue = state.inventory &&
      (state.inventoryPending || Date.now() - state.lastInventoryApply > 5000);
    if (!state.equipmentPending && !inventoryDue) return;
    let blocked = false;
    this.controller.emitter.emit('queryBlockSetInventoryEvent', { block: () => blocked = true });
    if (blocked || this.state !== state || state.creation !== 'idle') return;

    if (state.equipmentPending && state.equipment) {
      const equipment = state.equipment;
      if (!applyEquipment(player, equipment) || this.state !== state) return;
      // Applying native equipment may raise events that replace the snapshot.
      if (state.equipment !== equipment) return;
      state.equipmentPending = false;
    }
    if (state.inventory && inventoryDue) {
      const inventory = state.inventory;
      if (applyInventory(player, inventory, false, true) &&
          this.state === state && state.inventory === inventory) {
        state.inventoryPending = false;
        state.lastInventoryApply = Date.now();
      }
    }
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
}
