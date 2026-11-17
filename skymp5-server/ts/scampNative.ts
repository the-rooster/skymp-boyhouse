const scampNativeNode = require(process.cwd() + "/scam_native.node");

export declare interface Bot {
  destroy(): void;
  send(msg: Record<string, unknown>): void;
  pollCustomPackets(): string[];
}

export type SendChatMessageFn = (
  formId: number,
  message: Record<string, unknown>
) => void;

export interface CampaignResumeState {
  attempt: string;
  status: "Pending" | "ProjectionReady" | "Failed";
  actor?: number;
  error?: string;
}

export interface CampaignResumeArea {
  id: string;
  worldOrCell: number;
  center: number[];
  radius: number;
}

export interface WorldBaseline {
  schemaVersion: 1;
  actor: number;
  packetEpoch: string;
  messages: Record<string, unknown>[];
}

export interface ScampServer {
  on(event: "connect", handler: (userId: number) => void): void;
  on(event: "disconnect", handler: (userId: number) => void): void;
  on(
    event: "customPacket",
    handler: (userId: number, content: string) => void
  ): void;
  attachSaveStorage(): void;
  tick(): void;

  createActor(
    formId: number,
    pos: number[],
    angleZ: number,
    cellOrWorld: number,
    userProfileId?: number
  ): number;

  destroyActor(formId: number): void;
  setUserActor(userId: number, actorFormId: number): void;
  getUserActor(userId: number): number;
  getUserGuid(userId: number): string;
  isConnected(userId: number): boolean;
  getActorName(actorId: number): string;
  getActorPos(actorId: number): number[];
  getActorCellOrWorld(actorId: number): number;
  setRaceMenuOpen(formId: number, open: boolean): void;
  sendCustomPacket(userId: number, jsonContent: string): void;
  setEnabled(actorId: number, enabled: boolean): void;
  getActorsByProfileId(profileId: number): number[];
  createBot(captureCustomPackets?: boolean): Bot;
  getUserByActor(formId: number): number;
  getUserIp(userId: number): string;
  kick(userId: number): void;
  setGameplayBlocked(userId: number, blocked: boolean): void;
  getGameplayBlocked(userId: number): boolean;
  beginPacketEpoch(userId: number): string;
  getPacketEpoch(userId: number): string;
  getWorldBaseline(userId: number, packetEpoch: string): WorldBaseline;
  resumeCampaignActor(userId: number, actorId: number, expectedPhase: string,
    area: CampaignResumeArea): CampaignResumeState | null;
  getCampaignResume(userId: number): CampaignResumeState | null;
  captureCampaignResumeProjection(userId: number, packetEpoch: string): {
    attempt: string; proof: string; manifest: Record<string, unknown>; baseline: WorldBaseline;
  };
  completeCampaignResume(userId: number, attempt: string, packetEpoch: string,
    proof: string, publishReady: () => void): boolean;
  detachCampaignActor(userId: number): boolean;
  setCampaignVisibilityCallbacks(
    before: ((owner: number, users: number[]) => boolean) | null,
    after: ((owner: number, users: number[], synchronized: boolean) => boolean) | null
  ): void;

  executeJavaScriptOnChakra(src: string): void;
  clear(): void;
  writeLogs(logLevel: string, message: string): void;
  getPrometheusMetrics(): string;
}

export const createScampServer = (serverSettings: Record<string, unknown>) => {
  const res = new scampNativeNode.ScampServer(JSON.stringify(serverSettings));
  res._setSelf(res);
  return res;
}

export const getScampNative = () => {
  return scampNativeNode;
}
