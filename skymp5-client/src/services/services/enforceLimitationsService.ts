import { GameLoadEvent } from "../events/gameLoadEvent";
import { ClientListener, CombinedController, Sp } from "./clientListener";

export class EnforceLimitationsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.once("update", () => this.onceUpdate());
        controller.emitter.on("gameLoad", (e) => this.onGameLoad(e));
    }

    private onceUpdate() {
        this.sp.Game.setInChargen(true, true, false);
    }

    private onGameLoad(event: GameLoadEvent) {
        // A personal new game is not subject to multiplayer chargen limits,
        // and its native notification is delivered outside Papyrus update.
        if (event.isNewGame) return;
        this.sp.Game.setInChargen(true, true, false);
    }
}
