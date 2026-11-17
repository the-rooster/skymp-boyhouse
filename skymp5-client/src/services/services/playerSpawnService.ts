import { applyAppearanceToPlayer } from '../../sync/appearance';
import { setActorValuePercentage } from '../../sync/actorvalues';
import { learnSpells, removeAllSpells } from '../../sync/spell';
import { logError, logTrace } from '../../logging';
import { CreateActorMessage } from '../messages/createActorMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { LoadGameService } from './loadGameService';
import { PlayerLoadoutService } from './playerLoadoutService';
import { TimeService } from './timeService';
import { NetworkingService } from './networkingService';
import { ActorFactionsService } from './actorFactionsService';

type SpawnPhase = 'pending' | 'positioning' | 'applying' | 'ready' | 'failed';
interface SpawnState {
    generation: number;
    message: CreateActorMessage;
    loadoutGeneration: number;
    factionGeneration: number;
    phase: SpawnPhase;
    started: number;
    lastMove: number;
    moves: number;
    stableSamples: number;
    sawWorld: boolean;
    presentationApplied: boolean;
    error?: string;
}
interface SpawnLifetime { version: 1; generation: number; loading: boolean; }

// One self-spawn owns bootstrap, positioning, and presentation. Readiness comes
// from engine observations, never from a delay expiring. Delays cannot retain a
// player wrapper or continue work for a superseded self/baseline.
export class PlayerSpawnService extends ClientListener {
    private state?: SpawnState;
    private lifetime: SpawnLifetime;
    private loading = false;
    private loadingScreen = false;

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        const key = 'boyhousePlayerSpawnLifetime';
        const stored = sp.storage[key] as SpawnLifetime | undefined;
        this.lifetime = stored?.version === 1 ? stored : { version: 1, generation: 0, loading: false };
        ++this.lifetime.generation;
        sp.storage[key] = this.lifetime;
        controller.on('tick', () => this.onTick());
        controller.on('update', () => this.onUpdate());
        // Papyrus Ui calls are forbidden in tick context (including at the
        // main menu, where no update callbacks run). Track these native events.
        controller.on('menuOpen', event => {
            if (event.name === 'Loading Menu') this.loadingScreen = true;
        });
        controller.on('menuClose', event => {
            if (event.name === 'Loading Menu') this.loadingScreen = false;
        });
        controller.on('preLoadGame', () => {
            this.loading = true;
            if (!this.lifetime.loading) this.cancel();
        });
        controller.on('postLoadGame', (event?: { success: boolean }) => {
            if (event?.success !== false) return;
            const ownLoad = this.lifetime.loading;
            this.loading = false;
            this.lifetime.loading = false;
            if (ownLoad && this.state) this.fail(this.state, 'Generated multiplayer save failed to load');
        });
        controller.emitter.on('gameLoad', event => {
            const ownLoad = this.lifetime.loading;
            this.loading = false;
            this.lifetime.loading = false;
            if (!event.isCausedBySkyrimPlatform || !ownLoad) this.cancel();
            else if (this.state) {
                // A tick can precede the first update in the loaded world.
                // Do not bootstrap another save while waiting to observe it.
                this.state.phase = 'pending'; this.state.sawWorld = true;
                this.state.stableSamples = 0; this.state.presentationApplied = false;
            }
        });
        for (const event of ['connectionAccepted', 'connectionDisconnect', 'connectionFailed', 'connectionDenied'] as const)
            controller.emitter.on(event, () => this.cancel());
        controller.emitter.on('destroyActorMessage', ({ message }) => {
            if (this.state?.message.idx === message.idx) this.cancel();
        });
    }

    request(message: CreateActorMessage): number {
        if (!message.isMe || !Number.isInteger(message.transform.worldOrCell) || message.transform.worldOrCell <= 0 ||
            message.transform.worldOrCell > 0xffffffff ||
            ![message.transform.pos, message.transform.rot].every(v => Array.isArray(v) && v.length === 3 && v.every(Number.isFinite)))
            throw Error('Invalid player spawn transform');
        this.cancel();
        const snapshot: CreateActorMessage = JSON.parse(JSON.stringify(message));
        const generation = ++this.lifetime.generation;
        const loadoutGeneration = this.controller.lookupListener(PlayerLoadoutService).beginSpawn(snapshot);
        const factionGeneration = this.controller.lookupListener(ActorFactionsService).beginSpawn(snapshot);
        this.state = { generation, message: snapshot, loadoutGeneration, factionGeneration, phase: 'pending', started: Date.now(),
            lastMove: 0, moves: 0, stableSamples: 0, sawWorld: false, presentationApplied: false };
        return generation;
    }

    cancel(): void {
        ++this.lifetime.generation;
        const previous = this.state;
        this.state = undefined;
        if (previous) {
            this.controller.lookupListener(PlayerLoadoutService).cancelSpawn(previous.loadoutGeneration);
            this.controller.lookupListener(ActorFactionsService).cancelSpawn(previous.factionGeneration);
        }
    }

    isReady(generation: number): boolean {
        return this.state?.generation === generation && this.current(this.state) && this.state.phase === 'ready';
    }

    get status(): { generation: number; phase: SpawnPhase; error?: string } | undefined {
        const state = this.state;
        return state ? { generation: state.generation, phase: state.phase, error: state.error } : undefined;
    }

    private current(state: SpawnState): boolean {
        return this.state === state && state.generation === this.lifetime.generation && state.phase !== 'failed';
    }

    private fail(state: SpawnState, error: unknown): void {
        if (!this.current(state)) return;
        state.error = String(error); state.phase = 'failed';
        this.controller.lookupListener(PlayerLoadoutService).cancelSpawn(state.loadoutGeneration);
        this.controller.lookupListener(ActorFactionsService).cancelSpawn(state.factionGeneration);
        logError(this, 'Player spawn failed', state.error);
    }

    private onTick(): void {
        const state = this.state;
        if (!state || !this.current(state) || state.phase === 'ready') return;
        if (!this.controller.lookupListener(NetworkingService).isConnected()) return this.cancel();
        if (Date.now() - state.started > 60000) return this.fail(state, 'Player spawn timed out');
        if (this.loading || this.lifetime.loading || state.sawWorld || Date.now() - state.started < 500) return;
        if (this.loadingScreen) return;
        try {
            const order: string[] = [];
            for (let i = 0; i < this.sp.Game.getModCount(); ++i) order.push(this.sp.Game.getModName(i));
            const appearance = state.message.appearance;
            const npc = appearance ? {
                name: appearance.name, raceId: appearance.raceId, isFemale: appearance.isFemale,
                face: { hairColor: appearance.hairColor, bodySkinColor: appearance.skinColor,
                    headTextureSetId: appearance.headTextureSetId, headPartIds: appearance.headpartIds,
                    presets: appearance.presets },
            } : undefined;
            this.lifetime.loading = true;
            this.controller.lookupListener(LoadGameService).loadGame(state.message.transform.pos,
                state.message.transform.rot, state.message.transform.worldOrCell, npc, order,
                { minutes: 0, seconds: 0, hours: this.controller.lookupListener(TimeService).getTime().newGameHourValue });
        } catch (error) { this.lifetime.loading = false; this.fail(state, error); }
    }

    private onUpdate(): void {
        const state = this.state;
        if (!state || !this.current(state) || state.phase === 'ready' || this.loading || this.lifetime.loading) return;
        if (!this.controller.lookupListener(NetworkingService).isConnected()) return this.cancel();
        try {
            this.loadingScreen = this.sp.Ui.isMenuOpen('Loading Menu');
            const player = this.sp.Game.getPlayer();
            if (!player || this.loadingScreen || this.sp.Ui.isMenuOpen('Main Menu')) return;
            const world = (player.getWorldSpace() || player.getParentCell())?.getFormID();
            if (!world) return;
            state.sawWorld = true;
            const transform = state.message.transform;
            const position = [player.getPositionX(), player.getPositionY(), player.getPositionZ()];
            const distanceSquared = position.reduce((sum, n, i) => sum + (n - transform.pos[i]) ** 2, 0);
            const arrived = world === transform.worldOrCell && Number.isFinite(distanceSquared) &&
                distanceSquared <= 128 ** 2 && player.is3DLoaded();
            if (!arrived || state.moves === 0) {
                state.stableSamples = 0;
                if (Date.now() - state.lastMove < 500 && state.moves > 0) return;
                if (state.moves >= 8) throw Error('Player did not reach the authoritative spawn');
                ++state.moves; state.lastMove = Date.now();
                if (!state.presentationApplied) state.phase = 'positioning';
                this.sp.TESModPlatform.moveRefrToPosition(player,
                    this.sp.Cell.from(this.sp.Game.getFormEx(transform.worldOrCell)),
                    this.sp.WorldSpace.from(this.sp.Game.getFormEx(transform.worldOrCell)),
                    ...transform.pos, ...transform.rot);
                return;
            }
            if (++state.stableSamples < 2) return;
            if (!state.presentationApplied) {
                if (state.message.appearance && this.controller.lookupListener(PlayerLoadoutService)
                    .shouldApplySpawnAppearance(state.loadoutGeneration))
                    applyAppearanceToPlayer(state.message.appearance, () => this.current(state));
                if (!this.current(state)) return;
                const props = state.message.props;
                if (props?.learnedSpells) {
                    removeAllSpells(player);
                    if (!this.current(state)) return;
                    learnSpells(player, props.learnedSpells);
                }
                if (!this.current(state)) return;
                for (const name of ['healRate', 'healRateMult', 'health', 'magickaRate', 'magickaRateMult',
                    'magicka', 'staminaRate', 'staminaRateMult', 'stamina']) {
                    const value = (props as Record<string, unknown> | undefined)?.[name];
                    if (typeof value === 'number' && this.current(state)) player.setActorValue(name, value);
                }
                for (const name of ['health', 'magicka', 'stamina']) {
                    const value = (props as Record<string, unknown> | undefined)?.[`${name}Percentage`];
                    if (typeof value === 'number' && this.current(state)) setActorValuePercentage(player, name, value);
                }
                if (!this.current(state)) return;
                if (props?.isDead) this.controller.emitter.emit('applyDeathStateEvent', { actor: player, isDead: true });
                if (this.current(state)) { state.phase = 'applying'; state.presentationApplied = true; }
                return; // Observe the current player again on a later update.
            }
            const factions = this.controller.lookupListener(ActorFactionsService);
            factions.markSpawnAvailable(state.factionGeneration);
            const factionProgress = factions.spawnReadiness(state.factionGeneration);
            if (factionProgress.error) throw Error(factionProgress.error);
            if (!factionProgress.ready) return;
            this.controller.lookupListener(PlayerLoadoutService).markSpawnReady(state.loadoutGeneration);
            if (this.current(state)) {
                state.phase = 'ready';
                logTrace(this, 'Player spawn observed ready', state.generation);
            }
        } catch (error) { this.fail(state, error); }
    }
}
