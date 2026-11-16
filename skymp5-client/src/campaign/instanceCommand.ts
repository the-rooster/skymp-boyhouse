export function parseInstanceCommand(input: string): Record<string, string | number> {
  const [prefix, action, ...args] = input.trim().split(/\s+/);
  if (prefix !== "bh") throw new Error("Expected bh command");
  const id = (value: string) => {
    if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(value ?? "")) throw new Error("Use a decimal or 0x-prefixed server character ID");
    const n = Number(value);
    if (!Number.isInteger(n) || n <= 0 || n > 0xffffffff) throw new Error("Invalid character ID");
    return n;
  };
  const result: Record<string, string | number> = { customPacketType: "boyhouse.instance", action };
  switch (action) {
    case "status": case "leave": case "exit":
      if (args.length !== 0) throw new Error("Unexpected arguments");
      break;
    case "invite": case "kick": case "accept":
      if (args.length !== 1) throw new Error("Expected a server character ID");
      result[action === "accept" ? "leader" : "target"] = id(args[0]);
      break;
    case "enter":
      if (args.length < 1 || args.length > 2 || !/^[a-zA-Z0-9_-]{1,48}$/.test(args[0])) throw new Error("Use: bh enter AREA [OWNER_ID]");
      result.area = args[0];
      if (args[1]) result.owner = id(args[1]);
      break;
    default: throw new Error("Commands: bh status|invite ID|accept ID|kick ID|leave|enter AREA [OWNER_ID]|exit");
  }
  return result;
}
