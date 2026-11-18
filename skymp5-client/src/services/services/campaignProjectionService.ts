import { WorldProjectionCoordinator } from '../../campaign/worldProjectionCoordinator';
import { MsgType } from '../../messages';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { NativeWorldProjectionService } from './nativeWorldProjectionService';
import { NetworkingService } from './networkingService';

export interface SceneDialogueCue {
    customPacketType: 'boyhouse.sceneDialogue';
    protocolVersion: 1;
    packetEpoch: string;
    sequence: number;
    sceneId: number;
    actionIndex: number;
    speaker: number;
    topicId: number;
    slotMs?: number;
}

function isSceneDialogueCue(packet: unknown): packet is SceneDialogueCue {
    if (!packet || typeof packet !== 'object') return false;
    const cue = packet as Partial<SceneDialogueCue>;
    const integer = (v: unknown, min: number, max: number) =>
        typeof v === 'number' && Number.isSafeInteger(v) && v >= min && v <= max;
    return cue.customPacketType === 'boyhouse.sceneDialogue' && cue.protocolVersion === 1 &&
        typeof cue.packetEpoch === 'string' && /^[a-f0-9]{32}$/.test(cue.packetEpoch) &&
        integer(cue.sequence, 1, Number.MAX_SAFE_INTEGER) && integer(cue.actionIndex, 0, 0xffffffff) &&
        integer(cue.sceneId, 1, 0xfeffffff) && integer(cue.topicId, 1, 0xfeffffff) &&
        integer(cue.speaker, 0xff000000, 0xffffffff) &&
        (cue.slotMs === undefined || integer(cue.slotMs, 1, Number.MAX_SAFE_INTEGER));
}

export class CampaignProjectionService extends ClientListener {
    readonly coordinator: WorldProjectionCoordinator;
    readonly enabled: boolean;
    private dialogue: { cue: SceneDialogueCue; expires: number }[] = [];
    private lastDialogueSequence = 0;

    private clearDialogue(): void {
        this.dialogue = [];
        this.lastDialogueSequence = 0;
    }

    constructor(private sp: Sp, private controller: CombinedController, engine: NativeWorldProjectionService) {
        super();
        this.enabled = sp.settings['skymp5-client']['campaign-projection'] === true;
        const network = () => controller.lookupListener(NetworkingService);
        this.coordinator = new WorldProjectionCoordinator(engine, {
            block: epoch => {
                this.clearDialogue();
                if (epoch) network().selectPacketEpoch(epoch); else network().requirePacketEpoch();
                controller.emitter.emit('campaignProjectionPending');
            },
            send: content => controller.emitter.emit('sendMessage', { message: {
                t: MsgType.CustomPacket, contentJsonDump: JSON.stringify(content),
            }, reliability: 'reliable' }),
            drain: epoch => network().drainPacketEpoch(epoch),
            release: epoch => network().releasePacketEpoch(epoch),
            close: error => {
                logError(this, 'Campaign transition failed', error);
                const coordinatorGeneration = this.coordinator.status;
                // Never destroy the native network client from its own packet
                // callback. A newer session invalidates this deferred close.
                controller.once('tick', () => {
                    const current = this.coordinator.status;
                    if (current.phase === 'failed' && current.generation === coordinatorGeneration.generation) network().close();
                });
            },
        });
        // Read-only diagnostics in the same plugin runtime can observe real
        // transition progress without bypassing the coordinator.
        sp.storage['boyhouseProjectionCoordinator'] = this.coordinator;
        if (!this.enabled) return;
        controller.emitter.on('connectionAccepted', () => this.coordinator.connect(
            `${Date.now()}_${Math.random().toString(36).slice(2)}`));
        for (const event of ['connectionDisconnect', 'connectionDenied', 'connectionFailed'] as const)
            controller.emitter.on(event, () => { this.clearDialogue(); this.coordinator.disconnect(); });
        controller.emitter.on('customPacketMessage', event => {
            let packet: unknown;
            try { packet = JSON.parse(event.message.contentJsonDump); } catch { return; }
            if (isSceneDialogueCue(packet)) {
                const phase = this.coordinator.status.phase;
                if ((phase === 'releasing' || phase === 'ready') &&
                    packet.packetEpoch === this.coordinator.replica.packetEpoch &&
                    packet.sequence > this.lastDialogueSequence) {
                    this.lastDialogueSequence = packet.sequence;
                    // A new timed slot ends the previous one, even if its NPC never loaded.
                    if (packet.slotMs !== undefined)
                        this.dialogue = this.dialogue.filter(line => line.cue.slotMs === undefined);
                    // Preserve the newest live cues if an engine release stalls.
                    if (this.dialogue.length === 32) this.dialogue.shift();
                    this.dialogue.push({ cue: packet, expires: Date.now() + Math.min(packet.slotMs ?? 5000, 5000) });
                }
                return;
            }
            this.coordinator.receive(packet);
        });
        controller.emitter.on('gameLoad', event => {
            if (!event.isCausedBySkyrimPlatform && network().isConnected()) network().close();
        });
        controller.on('tick', () => this.coordinator.advance(false));
        controller.on('update', () => {
            const before = this.coordinator.status.phase;
            this.coordinator.advance(true);
            if (before !== 'ready' && this.coordinator.status.phase === 'ready')
                controller.emitter.emit('campaignProjectionReady');
            if (this.coordinator.status.phase === 'ready') this.presentDialogue(engine);
        });
        controller.once('tick', () => {
            if (network().isConnected()) { network().close(); network().reconnect(); }
        });
    }

    private presentDialogue(engine: NativeWorldProjectionService): void {
        if (!this.dialogue.length) return;
        const epoch = this.coordinator.replica.packetEpoch;
        const now = Date.now();
        const pending = this.dialogue = this.dialogue.filter(line => line.expires > now && line.cue.packetEpoch === epoch);
        const next = pending[0];
        if (!next) return;
        try {
            const result = engine.presentSceneDialogue(next.cue);
            // An authored reply must wait for the preceding speaker or its expiry.
            if (result === 'missing' || result === 'unloaded') return;
        } catch (error) {
            // Say may already have reached Skyrim. Never submit it twice.
            logError(this, 'Presenting campaign scene dialogue failed', error);
        }
        pending.shift();
    }
}
