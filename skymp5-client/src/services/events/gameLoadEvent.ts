export interface GameLoadEvent {
    isCausedBySkyrimPlatform: boolean;
    // SKSE delivers newGame on tick; Papyrus work must wait for update.
    isNewGame?: boolean;
}
