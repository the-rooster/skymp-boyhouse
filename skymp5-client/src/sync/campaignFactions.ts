export interface CampaignFactionRank { factionId: number; rank: number; isMember: boolean; }
export const campaignFactionsProperty = 'campaignFactions';

// A missing property is unmanaged; [] is an authoritative empty snapshot.
// Validate before replacing a model or dispatching any native work.
export function campaignFactionRanks(value: unknown): CampaignFactionRank[] {
    if (!Array.isArray(value) || value.length > 4096) throw Error('Invalid campaign faction snapshot');
    const result: CampaignFactionRank[] = [], seen = new Set<number>();
    for (let i = 0; i < value.length; ++i) {
        if (!Object.prototype.hasOwnProperty.call(value, i)) throw Error('Sparse campaign faction snapshot');
        const entry = value[i];
        if (!entry || typeof entry !== 'object' || Array.isArray(entry) ||
            Object.keys(entry).some(key => key !== 'factionId' && key !== 'rank' && key !== 'isMember'))
            throw Error('Invalid campaign faction entry');
        const { factionId, rank, isMember } = entry;
        if (!Number.isInteger(factionId) || factionId <= 0 || factionId > 0xffffffff ||
            !Number.isInteger(rank) || rank < -128 || rank > 127 || typeof isMember !== 'boolean' || seen.has(factionId))
            throw Error('Invalid or duplicate campaign faction rank');
        seen.add(factionId); result.push({ factionId, rank, isMember });
    }
    return result.sort((a, b) => a.factionId - b.factionId);
}

export function campaignFactionsFromBaseline(message: {
    customPropsJsonDumps?: { propName: string; propValueJsonDump: string }[];
}): CampaignFactionRank[] | undefined {
    const properties = message.customPropsJsonDumps?.filter(property => property.propName === campaignFactionsProperty) || [];
    if (!properties.length) return undefined;
    if (properties.length !== 1) throw Error('Duplicate authoritative campaign faction snapshot');
    return campaignFactionRanks(JSON.parse(properties[0].propValueJsonDump));
}
