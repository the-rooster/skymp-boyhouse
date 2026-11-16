import { WorldModel } from './model';
import { FormViewArray } from './formViewArray';
import { PlayerCharacterDataHolder } from './playerCharacterDataHolder';
import { ClientListener, CombinedController, Sp } from '../services/services/clientListener';
import { logTrace } from "../logging";
import { SinglePlayerService } from "../services/services/singlePlayerService";
import { RemoteServer } from "../services/services/remoteServer";
import { NetworkingService } from '../services/services/networkingService';

export class WorldView extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();

    controller.on("update", () => this.onUpdate());
    controller.once("update", () => this.onceUpdate());

    this.state = this.makeEmptyState();

    const oldView = this.sp.storage["view"];

    // can't use instanceof here because each hot reload creates a new class
    this.oldView = typeof oldView === "object" ? oldView as WorldView : undefined;

    this.sp.storage["view"] = this;
    controller.emitter.on('worldModelReset', () => this.destroy());
    controller.on('preLoadGame', () => { this.loading = true; this.destroy(); });
    controller.emitter.on('gameLoad', () => {
      this.loading = false;
      this.destroy();
      if (!this.explicitlyBlocked) this.waitGameTimeAndAllowFormViewUpdate(1);
    });
    controller.emitter.on('connectionAccepted', () => {
      this.destroy();
      if (!this.explicitlyBlocked) {
        const generation = this.allowUpdateGeneration;
        // Connection delivery runs in tick context, where Papyrus waits are
        // forbidden. A load or explicit gate can invalidate this queued resume.
        controller.once('update', () => this.waitGameTimeAndAllowFormViewUpdate(1, generation));
      }
    });
  }

  getRemoteRefrId(clientsideRefrId: number): number {
    return this.state.formViews.getRemoteRefrId(clientsideRefrId);
  }

  getLocalRefrId(remoteRefrId: number): number {
    return this.state.formViews.getLocalRefrId(remoteRefrId);
  }

  syncFormArray(model: WorldModel) {
    const { settings } = this.sp;
    const showMe = settings['skymp5-client']['show-me'];
    this.state.formViews.syncFormView(model, !!showMe);
  }

  destroy() {
    ++this.allowUpdateGeneration;
    this.state.formViews.resize(0);
    this.state.cloneFormViews.resize(0); // Recenrly added, not tested if it's needed
    this.state = this.makeEmptyState();
  }

  getFormViews() {
    return this.state.formViews;
  }

  getSpawnReadiness(model: WorldModel) {
    const player = this.sp.Game.getPlayer();
    const world = (player?.getWorldSpace() || player?.getParentCell())?.getFormID() || 0;
    const result = this.state.formViews.getSpawnReadiness(model, world);
    const self = model.forms[model.playerCharacterFormIdx];
    return { ...result, ready: result.ready && !!world && self?.movement?.worldOrCell === world &&
      !this.loading && this.state.allowUpdate && this.sp.storage['view'] === this &&
      !this.controller.lookupListener(SinglePlayerService).isSinglePlayer &&
      this.controller.lookupListener(NetworkingService).isConnected() };
  }

  private onUpdate() {
    if (this.loading) return;
    const singlePlayerService = this.controller.lookupListener(SinglePlayerService);
    const connected = this.controller.lookupListener(NetworkingService).isConnected();
    if (singlePlayerService.isSinglePlayer || !connected) {
      if (this.hadConnectedWorld) this.destroy();
      this.hadConnectedWorld = false;
      return;
    }
    this.hadConnectedWorld = true;
    if (!this.sp.Game.getPlayer()) return;
    this.resetAllFormViewsIfPlayerChangedWorld();
    const modelSource = this.controller.lookupListener(RemoteServer);
    this.updateWorld(modelSource.getWorldModel());
  }

  private onceUpdate() {
    if (this.oldView) {
      this.oldView.destroy();
      this.oldView = undefined;
      logTrace(this, 'Previous View destroyed');
    }
    if (this.allowUpdateGeneration === 0 && !this.explicitlyBlocked)
      this.waitGameTimeAndAllowFormViewUpdate(1.0);
  }

  private resetAllFormViewsIfPlayerChangedWorld() {
    const state = this.state;
    const pc = this.sp.Game.getPlayer();
    const pcWorldOrCell = (pc?.getWorldSpace() || pc?.getParentCell())?.getFormID();
    if (!pcWorldOrCell) return;
    if (state.pcWorldOrCell !== pcWorldOrCell) {
      if (state.pcWorldOrCell) {
        logTrace(this, 'Reset all form views');
        state.formViews.resize(0);
        state.cloneFormViews.resize(0);
      }
      state.pcWorldOrCell = pcWorldOrCell;
    }
  }

  // Work around showRaceMenu issue
  // Default nord in Race Menu will have very ugly face
  // If other players are spawning when we show this menu
  // TODO: separate listener
  public waitGameTimeAndAllowFormViewUpdate(seconds: number, expectedGeneration?: number) {
    if (expectedGeneration !== undefined && !this.isUpdateGateCurrent(expectedGeneration)) return;
    this.explicitlyBlocked = false;
    const generation = ++this.allowUpdateGeneration;
    const state = this.state;
    // Wait 1s game time (time spent in Race Menu isn't counted)
    this.sp.Utility.wait(seconds).then(() => {
      if (generation !== this.allowUpdateGeneration || state !== this.state || this.sp.storage['view'] !== this) return;
      state.allowUpdate = true;
      logTrace(this, 'Update is now allowed');
    });
  }

  public setFormViewUpdateAllowed(allowed: boolean) {
    this.explicitlyBlocked = !allowed;
    ++this.allowUpdateGeneration;
    this.state.allowUpdate = allowed;
    logTrace(this, 'Update is now', allowed ? 'allowed' : 'disallowed');
    return this.allowUpdateGeneration;
  }

  public isUpdateGateCurrent(generation: number): boolean {
    return generation === this.allowUpdateGeneration && this.sp.storage['view'] === this;
  }

  private updateWorld(model: WorldModel): void {
    const { settings } = this.sp;
    const state = this.state;

    if (!state.allowUpdate) {
      model = {
        forms: [],
        playerCharacterFormIdx: model.playerCharacterFormIdx,
        playerCharacterRefrId: model.playerCharacterRefrId
      }
    }

    const skipUpdates = settings['skymp5-client']['skipUpdates'];

    // skip 50% of updates if specified in the settings
    state.counter = !state.counter;
    if (state.counter && skipUpdates) {
      return;
    }

    state.formViews.resize(model.forms.length);

    const showMe = settings['skymp5-client']['show-me'];
    const showClones = settings['skymp5-client']['show-clones'];

    PlayerCharacterDataHolder.updateData();

    state.formViews.updateAll(model, !!showMe, false);

    if (showClones) {
      state.cloneFormViews.updateAll(model, false, true);
    } else {
      state.cloneFormViews.resize(0);
    }
  }

  private makeEmptyState() {
    return {
      formViews: new FormViewArray(),
      cloneFormViews: new FormViewArray(),
      allowUpdate: false,
      pcWorldOrCell: 0,
      counter: false,
    }
  }

  private state: {
    formViews: FormViewArray;
    cloneFormViews: FormViewArray;
    allowUpdate: boolean;
    pcWorldOrCell: number;
    counter: boolean;
  };

  private oldView?: WorldView;
  private allowUpdateGeneration = 0;
  private hadConnectedWorld = false;
  private explicitlyBlocked = false;
  private loading = false;
}
