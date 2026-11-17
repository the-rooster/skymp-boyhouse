import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ChangeFormNpc } from "skyrimPlatform";
import { NetworkingService } from './networkingService';

export class LoadGameService extends ClientListener {
    get isLoadingPlatformSave(): boolean { return this._isCausedBySkyrimPlatform; }

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("loadGame", () => this.onLoadGame());
        this.controller.on('newGame', () => {
            this._isCausedBySkyrimPlatform = false;
            this.onLoadGame(true);
        });
        this.controller.on('postLoadGame', (event?: { success: boolean }) => {
            if (event?.success !== false) return;
            this._isCausedBySkyrimPlatform = false;
            // Pre-load already invalidated the self/model. A failed attempt
            // cannot resume that campaign as ready without a fresh baseline.
            const generation = this.connectionGeneration;
            this.controller.once('tick', () => {
                if (generation !== this.connectionGeneration) return;
                const network = this.controller.lookupListener(NetworkingService);
                if (network.isConnected()) network.close();
            });
        });
        for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied'] as const)
            controller.emitter.on(event, () => { ++this.connectionGeneration; });
    }

    public loadGame(pos: number[], rot: number[], worldOrCell: number, changeFormNpc?: ChangeFormNpc, loadOrder?: string[], time?: { seconds: number, minutes: number, hours: number }) {
        try {
            // @ts-ignore
            this.sp.loadGame(pos, rot, worldOrCell, changeFormNpc, loadOrder, time);
        } catch (e) {
            // Hotfix non-vanilla headparts bug
            // @ts-ignore
            this.sp.loadGame(pos, rot, worldOrCell, undefined, loadOrder, time);
        }
        this._isCausedBySkyrimPlatform = true;
    }

    private onLoadGame(isNewGame = false) {
        try {
            const gameLoadEvent = {
                isCausedBySkyrimPlatform: this._isCausedBySkyrimPlatform,
                isNewGame,
            };
            this.controller.emitter.emit("gameLoad", gameLoadEvent);
        } catch (e) {
            this.controller.once("tick", () => {
                this._isCausedBySkyrimPlatform = false;
            });
            throw e;
        }
        this.controller.once("tick", () => {
            this._isCausedBySkyrimPlatform = false;
        });
    }

    private _isCausedBySkyrimPlatform = false;
    private connectionGeneration = 0;
}
