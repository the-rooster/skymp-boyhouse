import { WorldProjectionCoordinator } from '../../campaign/worldProjectionCoordinator';
import { MsgType } from '../../messages';
import { logError } from '../../logging';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { NativeWorldProjectionService } from './nativeWorldProjectionService';
import { NetworkingService } from './networkingService';

export class CampaignProjectionService extends ClientListener {
    readonly coordinator: WorldProjectionCoordinator;
    readonly enabled: boolean;

    constructor(private sp: Sp, private controller: CombinedController, engine: NativeWorldProjectionService) {
        super();
        this.enabled = sp.settings['skymp5-client']['campaign-projection'] === true;
        const network = () => controller.lookupListener(NetworkingService);
        this.coordinator = new WorldProjectionCoordinator(engine, {
            block: epoch => {
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
            controller.emitter.on(event, () => this.coordinator.disconnect());
        controller.emitter.on('customPacketMessage', event => {
            let packet: unknown;
            try { packet = JSON.parse(event.message.contentJsonDump); } catch { return; }
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
        });
        controller.once('tick', () => {
            if (network().isConnected()) { network().close(); network().reconnect(); }
        });
    }
}
