// Install only in a dedicated diagnostic PluginFolders directory, never in a
// release client. An explicit settings flag may load a generated test character.
(() => {
  const sp = skyrimPlatform;
  const log = (name, value) => sp.writeLogs("boyhouseRuntimeSmoke", name, JSON.stringify(value) ?? "null");
  const fail = (name, error) => log(name, String(error?.stack || error));
  log("loaded", { platform: sp.getPlatformVersion(), codec: sp.mpClientPlugin.getVersion() });
  let ticks = 0;
  sp.on("tick", () => { if ([1, 300, 1800].includes(++ticks)) log("ticks", ticks); });
  sp.on("consoleMessage", event => {
    if (event.message.includes("BOYHOUSE_NATIVE_SMOKE")) log("console-hook", event.message);
  });
  sp.on("postLoadGame", event => log("postLoadGame", event));
  sp.on("loadGame", event => log("loadGame", event));
  sp.once("update", () => {
    try {
      const world = sp.Game.getForm(0x3c), player = sp.Game.getPlayer();
      const result = {
        mods: Array.from({ length: sp.Game.getModCount() }, (_, i) => sp.Game.getModName(i)),
        worldId: world?.getFormID(), playerId: player?.getFormID(),
        position: player && [player.getPositionX(), player.getPositionY(), player.getPositionZ()],
        skseVersion: ["GetVersion", "GetVersionMinor", "GetVersionBeta"].map(name => sp.callNative("SKSE", name, null)),
        stringLength: sp.callNative("StringUtil", "GetLength", null, "boyhouse"),
        power: sp.callNative("Math", "Pow", null, 2, 10),
        formFromFile: sp.Game.getFormFromFile(0x3c, "Skyrim.esm")?.getFormID(),
      };
      log("native-read", result);
      if (result.worldId !== 0x3c || result.playerId !== 0x14 || result.formFromFile !== 0x3c ||
          result.stringLength !== 8 || result.power !== 1024)
        throw Error("Native identity or argument marshalling mismatch");
      log("native-read-passed", true);
      sp.printConsole("BOYHOUSE_NATIVE_SMOKE");
      Promise.resolve(sp.Debug.getVersionNumber()).then(
        version => log("native-string-result", version), error => fail("native-string-error", error));
    } catch (error) { fail("native-error", error); }
  });
  if (sp.settings["boyhouse-runtime-smoke"]?.loadTemporaryWorld === true &&
      !globalThis.__boyhouseSmokeWorldLoadStarted) {
    globalThis.__boyhouseSmokeWorldLoadStarted = true;
    sp.once("tick", () => {
      try {
        log("temporary-world-load", { position: [133857, -61130, 14662], world: 60 });
        sp.loadGame([133857, -61130, 14662], [0, 0, 72], 0x3c, { name: "Boyhouse Runtime Test" },
          ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]);
        log("temporary-world-requested", true);
      } catch (error) { fail("load-error", error); }
    });
  }
})();
