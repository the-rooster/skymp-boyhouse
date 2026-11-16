import { logTrace, logError } from "../../logging";
import { NeverError } from "../../lib/errors";
import { MsgType } from "../../messages";
import { SendMessageEvent } from "../events/sendMessageEvent";
import { SendMessageWithRefrIdEvent } from "../events/sendMessageWithRefrIdEvent";
import { AnyMessage } from "../messages/anyMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";
import { SendRawMessageEvent } from "../events/sendRawMessageEvent";

import { CampaignPacketChannel } from "../../campaign/campaignPacketChannel";

export class NetworkingService extends ClientListener {
  private campaignPackets = new CampaignPacketChannel();
  private connectionGeneration = 0;

  // The projection coordinator calls these around authoritative baselines and
  // engine application. Receiving a frame never selects or releases its epoch.
  requirePacketEpoch(): void {
    this.campaignPackets.requireEpoch();
    if (this.sp.mpClientPlugin.getVersion() !== "2.1.0")
      throw Error("Campaign packet transport requires MpClientPlugin 2.1.0");
  }

  selectPacketEpoch(epoch: string): void {
    this.requirePacketEpoch();
    this.campaignPackets.select(epoch);
  }

  releasePacketEpoch(epoch: string): boolean {
    try { return this.campaignPackets.release(epoch, message => this.dispatchMessage(message)); }
    catch (error) { this.failCampaignTransport(error); return false; }
  }

  private failCampaignTransport(error: unknown): void {
    if (!this.campaignPackets.enabled) throw error;
    logError(this, "Campaign packet transport failed", error);
    this.campaignPackets.requireEpoch();
    const generation = this.connectionGeneration;
    // Destruction inside the native client's Tick callback could invalidate
    // the client currently delivering packets. Close on the next tick instead.
    this.controller.once("tick", () => {
      if (generation === this.connectionGeneration) this.close();
    });
  }

  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    this.controller.on("tick", () => this.onTick());

    this.controller.emitter.on("sendMessage", (e) => this.onSendMessage(e));
    this.controller.emitter.on("sendRawMessage", (e) => this.onSendRawMessage(e));
    this.controller.emitter.on("sendMessageWithRefrId", (e) => this.onSendMessageWithRefrId(e));
  }

  private onSendMessage(e: SendMessageEvent<unknown>) {
    try {
      const message = this.campaignPackets.outgoing(e.message);
      if (message) this.sp.mpClientPlugin.send(JSON.stringify(message), this.isReliable(e.reliability));
    } catch (error) { this.failCampaignTransport(error); }
  }

  private onSendRawMessage(e: SendRawMessageEvent) {
    if (!this.campaignPackets.enabled) {
      // @ts-expect-error
      this.sp.mpClientPlugin.sendRaw(e.rawMessage, e.rawMessage.byteLength, this.isReliable(e.reliability));
      return;
    }
    try {
      const message = this.campaignPackets.outgoingRaw(e.rawMessage);
      if (message) this.sp.mpClientPlugin.send(JSON.stringify(message), this.isReliable(e.reliability));
    } catch (error) { this.failCampaignTransport(error); }
  }

  private onSendMessageWithRefrId(e: SendMessageWithRefrIdEvent<AnyMessage>) {
    const refrId = e.message._refrId;

    const remoteServer = this.controller.lookupListener(RemoteServer);

    const idxInModel = refrId
      ? remoteServer.getWorldModel().forms.findIndex((f) => f && f.refrId === refrId)
      : remoteServer.getWorldModel().playerCharacterFormIdx;

    // fixes "can't get property idx of null or undefined"
    if (!remoteServer.getWorldModel().forms[idxInModel]) {
      return;
    }

    // @ts-ignore
    e.message.idx = remoteServer.getWorldModel().forms[idxInModel].idx;

    delete e.message._refrId;

    this.onSendMessage(e);
  }

  connect(hostName: string, port: number) {
    this.serverAddress = { hostName, port };
    this.createClientSafe();
  }

  reconnect() {
    this.createClientSafe();
  }

  close() {
    ++this.connectionGeneration;
    this.campaignPackets.clear();
    this.sp.mpClientPlugin.destroyClient();
    // Explicit closure must invalidate world work too. Reconnection belongs
    // to the native disconnect handler, so this notification does not redial.
    this.controller.emitter.emit('connectionDisconnect', {});
  }

  isConnected() {
    return this.sp.mpClientPlugin.isConnected();
  }

  private onTick() {
    this.sp.mpClientPlugin.tick((packetType, rawContent, error) => {
      switch (packetType) {
        case "connectionAccepted":
          ++this.connectionGeneration;
          this.campaignPackets.clear();
          this.controller.emitter.emit("connectionAccepted", {});
          break;
        case "connectionDenied":
          this.controller.emitter.emit("connectionDenied", { error });
          this.reconnect();
          break;
        case "connectionFailed":
          this.controller.emitter.emit("connectionFailed", {});
          this.reconnect();
          break;
        case "disconnect":
          this.controller.emitter.emit("connectionDisconnect", {});
          this.reconnect();
          break;
        case "message":
          // TODO: in theory can be empty jsonContent and non-empty error

          try {
            let msgAny: AnyMessage;
            if (rawContent === null) {
              throw Error("null rawContent");
            } else if ((new Uint8Array(rawContent as unknown as ArrayBuffer)[0]) === 0x7b) {
              msgAny = JSON.parse(this.sp.decodeUtf8(rawContent as unknown as ArrayBuffer));
            } else {
              // An epoch-aware connection cannot accept untagged binary traffic.
              if (this.campaignPackets.enabled) return;
              const event = { rawContent: rawContent as unknown as ArrayBuffer };
              return this.controller.emitter.emit("anyRawMessage", event);
            }
            this.campaignPackets.incoming(msgAny, message => this.dispatchMessage(message));
          } catch (error) { this.failCampaignTransport(error); }
          break;
      }
    });
  }

  private dispatchMessage(msgAny: AnyMessage): void {
    if (msgAny.t === MsgType.OpenContainer) {
      const event = { message: msgAny };
      this.controller.emitter.emit("openContainerMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateMovement) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateMovementMessage", event)
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateAnimation) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateAnimationMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateEquipment) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateEquipmentMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.ChangeValues) {
      const event = { message: msgAny };
      this.controller.emitter.emit("changeValuesMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.SpellCast) {
      const event = { message: msgAny };
      this.controller.emitter.emit("spellCastMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateAnimVariables) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateAnimVariablesMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateAppearance) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateAppearanceMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.Teleport) {
      const event = { message: msgAny };
      this.controller.emitter.emit("teleportMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateProperty) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updatePropertyMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.DeathStateContainer) {
      const event = { message: msgAny };
      this.controller.emitter.emit("deathStateContainerMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.CreateActor) {
      const event = { message: msgAny };
      this.controller.emitter.emit("createActorMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.CustomPacket) {
      const event = { message: msgAny };
      this.controller.emitter.emit("customPacketMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.DestroyActor) {
      const event = { message: msgAny };
      this.controller.emitter.emit("destroyActorMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.HostStart) {
      const event = { message: msgAny };
      this.controller.emitter.emit("hostStartMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.HostStop) {
      const event = { message: msgAny };
      this.controller.emitter.emit("hostStopMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.SetInventory) {
      const event = { message: msgAny };
      this.controller.emitter.emit("setInventoryMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.SetRaceMenuOpen) {
      const event = { message: msgAny };
      this.controller.emitter.emit("setRaceMenuOpenMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.SpSnippet) {
      const event = { message: msgAny };
      this.controller.emitter.emit("spSnippetMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.UpdateGamemodeData) {
      const event = { message: msgAny };
      this.controller.emitter.emit("updateGamemodeDataMessage", event);
      this.controller.emitter.emit("anyMessage", event);
    } else if (msgAny.t === MsgType.Teleport2) {
      const event = { message: msgAny };
      this.controller.emitter.emit("teleportMessage2", event);
      this.controller.emitter.emit("anyMessage", event);
    } else {
      // throw new NeverError(msgAny);
      throw new Error("Unhandled MsgType " + msgAny.t);
    }
  }

  private createClientSafe() {
    ++this.connectionGeneration;
    this.campaignPackets.clear();
    const { hostName, port } = this.serverAddress;

    logTrace(this, "createClientSafe " + hostName + ":" + port);

    if (this.serverAddress.hostName !== "" && this.serverAddress.port !== 0) {
      this.sp.mpClientPlugin.createClient(hostName, port);
    } else {
      logError(this, "createClientSafe failed");
    }
  }

  private get serverAddress(): { hostName: string, port: number } {
    const res: unknown = this.sp.storage["serverAddress"];
    if (typeof res === "object") {
      const result = res as { hostName: string, port: number };
      if (typeof result.hostName === "string" && typeof result.port === "number") {
        return result;
      }
    }
    return { hostName: "", port: 0 };
  }

  private set serverAddress(newValue: { hostName: string, port: number }) {
    this.sp.storage["serverAddress"] = newValue;
  }

  private isReliable(reliability: "reliable" | "unreliable") {
    switch (reliability) {
      case "reliable":
        return true;
      case "unreliable":
        return false;
      default:
        throw new NeverError(reliability);
    }
  }
};
