import { ClientListener, CombinedController, Sp } from "./clientListener";
import { GameLoadEvent } from "../events/gameLoadEvent";
import { NetworkingService } from "./networkingService";

export class SinglePlayerService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("gameLoad", (e) => this.onGameLoad(e));
    }

    get isSinglePlayer() {
        return this._isSinglePlayer;
    }

    private onGameLoad(event: GameLoadEvent) {
        const generation = ++this.worldGeneration;
        if (event.isNewGame) {
            // Invalidate every session, including a reconnect after an earlier
            // personal world. The native notification runs in tick context.
            this._isSinglePlayer = true;
            const network = this.controller.lookupListener(NetworkingService);
            if (network.isConnected()) network.close();
            this.controller.once('update', () => {
                if (generation === this.worldGeneration &&
                    !this.controller.lookupListener(NetworkingService).isConnected())
                    this.sp.Game.setInChargen(false, false, false);
            });
            return;
        }
        if (!event.isCausedBySkyrimPlatform && !this._isSinglePlayer) {
            this.sp.Debug.messageBox(
                'Save has been loaded in multiplayer, switching to the single-player mode',
            );
            this.controller.lookupListener(NetworkingService).close();
            this._isSinglePlayer = true;
            this.sp.Game.setInChargen(false, false, false);
        }
    }

    private _isSinglePlayer = false;
    private worldGeneration = 0;
}
