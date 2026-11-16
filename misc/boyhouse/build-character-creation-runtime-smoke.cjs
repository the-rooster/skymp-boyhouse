"use strict";
// Append to the actual built client: Skyrim Platform storage is per plugin.
// This generator never installs the bundle or launches/controls the desktop.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
if (process.argv.length < 3 || process.argv.length > 4 ||
    (process.argv[3] && process.argv[3] !== '--native-array-inputs'))
  throw Error('Usage: node build-character-creation-runtime-smoke.cjs <output.js> [--native-array-inputs]');
const nativeArrayInputs = process.argv[3] === '--native-array-inputs';
const output = path.resolve(process.argv[2]);
const source = path.join(root, 'build/dist/client/Data/Platform/Plugins/skymp5-client.js');
if (output === source) throw Error('Write a separate observer bundle, not the production client');
const client = fs.readFileSync(source, 'utf8');
const observer = String.raw`
;(() => {
  const sp = skyrimPlatform, started = Date.now(), desiredName = 'Boyhouse Creation Test';
  const nativeArrayInputs = ${nativeArrayInputs};
  const raceMenu = 'RaceSex Menu', panel = '_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance';
  const entry = panel + '.NameEntryInstance', input = entry + '.TextInputInstance';
  // Verified by reading the installed Interface.bsa's racesex_menu.gfx,
  // messagebox.swf and textentry.swf, including AS2 function bytecode:
  // panel.onDoneClicked -> ConfirmDone; button.onPress -> ClickCallback;
  // entry.onAccept -> nameChange -> ChangeName. Invoke their normal UI path.
  // Default to documented scalar InvokeInt for compatibility with the older
  // SP3 bridge. --native-array-inputs explicitly tests the repaired array path.
  // The zero-argument done/accept callbacks ignore the scalar compatibility 0.
  // Button.onPress takes controllerIdx first; keyboardOrMouse is unused and
  // MessageBox's press listener reads only event.target._name, supplied by
  // EventDispatcher. It does not require the omitted mouse-button argument.
  const messageBox = '_root.MessageMenu', yesButton = messageBox + '.Buttons.Button0';
  let phase = 'login', deadline = started + 60000, stopping = false, nextSample = 0;
  let editingSince = 0, editingSamples = 0, restoreControls, initialEpoch, initialVersion, selfIdx;
  let pendingAppearance, initialAppearanceChanges = 0, accepted = false, settledSamples = 0;
  let confirmationPressed = false, original, copies = [], enteredVersion = 0, leaveSteps = 0, waitUntil = 0;
  let arrayCasesComplete = false;
  const log = (event, data) => sp.writeLogs('boyhouseCharacterCreationRuntimeTest', event, JSON.stringify(data));
  const canonical = value => Array.isArray(value) ? value.map(canonical) : value && typeof value === 'object'
    ? Object.keys(value).sort().reduce((out, key) => { out[key] = canonical(value[key]); return out; }, {}) : value;
  const equal = (a, b) => JSON.stringify(canonical(a)) === JSON.stringify(canonical(b));
  const assert = (value, error) => { if (!value) throw Error(error); };
  const invoke = (menu, target, args) => nativeArrayInputs
    ? sp.Ui.invokeIntA(menu, target, args) : sp.Ui.invokeInt(menu, target, 0);
  const runArrayInputCases = () => {
    // Verified SKSE PapyrusUtility.cpp ResizeArray only copies/fills temporary
    // VM arrays. PapyrusArgs.h PackValue emits None for a zero-length result.
    // No quest, inventory, actor, controls, or service state is mutated here.
    const check = (name, actual, expected) => {
      assert(equal(actual, expected), name + ': ' + JSON.stringify({ actual, expected }));
      log('native-array-case', { name, result: actual });
    };
    for (const [kind, values, fill, expected] of [
      ['Int', [-2147483648, 0, 2147483647], 17, [-2147483648, 0, 2147483647]],
      ['Float', [0.1, -1.25, 2], 1.5, [Math.fround(0.1), -1.25, 2]],
      ['Bool', [true, false, true], true, [true, false, true]],
      ['String', ['', 'Boyhouse', 'array'], 'fill', ['', 'Boyhouse', 'array']],
    ]) {
      const resize = (...args) => sp.Utility['resize' + kind + 'Array'](...args);
      check(kind + '-roundtrip', resize(values, values.length, fill), expected);
      check(kind + '-empty-input', resize([], 1, fill), [fill]);
      check(kind + '-null-input', resize(null, 1, fill), [fill]);
      check(kind + '-empty-result', resize([], 0, fill), null);
    }
    check('Form-empty-input', sp.Utility.resizeFormArray([], 1, null), [null]);
    check('Form-null-input', sp.Utility.resizeFormArray(null, 1, null), [null]);
    check('Form-empty-result', sp.Utility.resizeFormArray([], 0, null), null);
    // Obtain the wrapper immediately before use, inside this native update.
    const freshPlayer = sp.Game.getPlayer();
    const playerId = freshPlayer.getFormID();
    const forms = sp.Utility.resizeFormArray([freshPlayer, null], 3, null);
    assert(Array.isArray(forms), 'Form roundtrip returned no array');
    check('Form-subclass-and-null', forms.map(form => form === null ? null : sp.Form.from(form).getFormID()),
      [playerId, null, null]);
    check('Alias-empty-input', sp.Utility.resizeAliasArray([], 1, null), [null]);
    check('Alias-null-input', sp.Utility.resizeAliasArray(null, 1, null), [null]);
    check('Alias-null-elements', sp.Utility.resizeAliasArray([null, null], 2, null), [null, null]);
    check('Alias-empty-result', sp.Utility.resizeAliasArray([], 0, null), null);
    const rejected = (name, call) => {
      let error;
      try { call(); } catch (caught) { error = String(caught); }
      assert(error, name + ' was accepted unexpectedly');
      log('native-array-rejection', { name, error });
    };
    rejected('fractional-int', () => sp.Utility.resizeIntArray([1.5], 1, 0));
    rejected('int-overflow', () => sp.Utility.resizeIntArray([2147483648], 1, 0));
    rejected('nonfinite-float', () => sp.Utility.resizeFloatArray([Infinity], 1, 0));
    rejected('heterogeneous-bool', () => sp.Utility.resizeBoolArray([true, 1], 1, false));
    rejected('undefined-element', () => sp.Utility.resizeFormArray([undefined], 1, null));
    rejected('nested-array', () => sp.Utility.resizeIntArray([[1]], 1, 0));
    rejected('sparse-array', () => sp.Utility.resizeIntArray(new Array(1), 1, 0));
    rejected('bridge-size-limit', () => sp.Utility.resizeIntArray(new Array(65537), 0, 0));
    arrayCasesComplete = true;
    log('native-array-cases-complete', { primitiveKinds: 4, formSubclass: true,
      aliasCoverage: 'empty/null only; no real alias binding claim' });
  };
  const controls = () => [sp.Game.isMovementControlsEnabled(), sp.Game.isFightingControlsEnabled(),
    sp.Game.isCamSwitchControlsEnabled(), sp.Game.isLookingControlsEnabled(), sp.Game.isSneakingControlsEnabled(),
    sp.Game.isMenuControlsEnabled(), sp.Game.isActivateControlsEnabled(), sp.Game.isJournalControlsEnabled()];
  const loadoutSettled = state => state && state.ready && state.initialApplied && state.creation === 'idle' &&
    !state.creationCompletionPending && !state.inventoryPending && !state.equipmentPending && !state.error;
  const gameplayHeld = flags => [0, 1, 4, 6, 7].every(i => !flags[i]);
  const setPhase = (next, timeout) => { phase = next; deadline = Date.now() + timeout; log('phase', { phase, deadline }); };
  const finish = (passed, detail) => {
    if (stopping) return;
    stopping = true;
    log('test-end', { passed, phase, elapsedMs: Date.now() - started, accepted,
      nativeArrayInputs, arrayCasesComplete,
      editingSamples, editingMs: editingSince && Date.now() - editingSince, detail });
    sp.win32.exitProcess();
  };
  const inspect = id => {
    const ref = sp.ObjectReference.from(sp.Game.getFormEx(id));
    if (!ref) return null;
    const actor = sp.Actor.from(ref);
    return { id, handle: sp.getReferenceHandle(id), baseId: ref.getBaseObject().getFormID(),
      disabled: ref.isDisabled(), blocked: ref.isActivationBlocked(), deleted: ref.isDeleted(), loaded: ref.is3DLoaded(),
      dead: actor && actor.isDead(), health: actor && actor.getActorValue('Health') };
  };
  const preview = player => {
    const worn = [];
    for (let bit = 0; bit < 32; ++bit) {
      const form = player.getWornForm((2 ** bit) >>> 0);
      if (form) worn.push({ slot: bit + 30, id: form.getFormID() });
    }
    return { worn, left: player.getEquippedWeapon(true)?.getFormID() || 0,
      right: player.getEquippedWeapon(false)?.getFormID() || 0 };
  };
  const nameEntryReady = () => sp.Ui.isMenuOpen(raceMenu) && !sp.Ui.isMenuOpen('MessageBoxMenu') &&
    sp.Ui.getInt(raceMenu, panel + '.Mode') === sp.Ui.getInt(raceMenu, panel + '.NAME_ENTRY') &&
    sp.Ui.getInt(raceMenu, input + '.maxChars') === 26 && sp.Ui.getFloat(raceMenu, entry + '._alpha') >= 99 &&
    sp.Ui.isTextInputEnabled();
  sp.on('consoleMessage', e => {
    if (/error|failed|spawn observed|logged in|race menu/i.test(e.message)) log('console', e.message);
  });
  sp.on('menuOpen', e => { if ([raceMenu, 'MessageBoxMenu'].includes(e.name)) log('menu-open', { name: e.name }); });
  sp.on('menuClose', e => { if ([raceMenu, 'MessageBoxMenu'].includes(e.name)) log('menu-close', { name: e.name }); });
  sp.on('tick', () => {
    if (!stopping && (Date.now() > deadline || Date.now() - started > 210000))
      finish(false, 'Observed creation acceptance deadline expired');
  });
  sp.on('update', () => {
    if (stopping) return;
    try {
      const coordinator = sp.storage.boyhouseProjectionCoordinator;
      const state = sp.storage.playerLoadoutState, model = sp.storage.worldModel, view = sp.storage.view;
      const self = model && model.forms[model.playerCharacterFormIdx], player = sp.Game.getPlayer();
      if (!coordinator || !player) return;
      const status = coordinator.status, projection = coordinator.replica.snapshot(), flags = controls();
      const menuOpen = sp.Ui.isMenuOpen(raceMenu);
      const lease = sp.storage.boyhouseProjectionControls?.enabled;
      if (!restoreControls && lease) restoreControls = lease.map((owned, i) => !!owned || flags[i]);
      if (Date.now() >= nextSample) {
        nextSample = Date.now() + 1000;
        const menus = { race: menuOpen, message: sp.Ui.isMenuOpen('MessageBoxMenu'), textInput: sp.Ui.isTextInputEnabled() };
        log('sample', { phase, status, menus, controls: flags, loadout: state, self: self && {
          idx: self.idx, appearance: self.appearance, numAppearanceChanges: self.numAppearanceChanges },
          menuState: menuOpen ? { mode: sp.Ui.getInt(raceMenu, panel + '.Mode'),
            nameEntryMode: sp.Ui.getInt(raceMenu, panel + '.NAME_ENTRY'), maxChars: sp.Ui.getInt(raceMenu, input + '.maxChars'),
            nameAlpha: sp.Ui.getFloat(raceMenu, entry + '._alpha'), nameText: sp.Ui.getString(raceMenu, input + '.text') } : undefined,
          position: [player.getPositionX(), player.getPositionY(), player.getPositionZ()],
          world: (player.getWorldSpace() || player.getParentCell())?.getFormID(), loaded: player.is3DLoaded(),
          preview: menuOpen ? preview(player) : undefined });
      }
      assert(status.phase !== 'failed', status.error || 'Projection failed');
      assert(!state?.error, state?.error || 'Loadout failed');
      if (phase !== 'login') assert(sp.mpClientPlugin.isConnected() && status.version > 0, 'Creation connection reset');
      if (state?.pendingAppearance && !pendingAppearance) {
        assert(phase === 'awaiting-close', 'Appearance submitted before the name UI completed');
        pendingAppearance = JSON.parse(JSON.stringify(state.pendingAppearance.data));
        assert(pendingAppearance.name === desiredName, 'Native appearance did not capture the entered name');
        assert(state.pendingAppearance.actorIdx === selfIdx, 'Appearance receipt changed actors');
        log('captured-native-appearance', { epoch: coordinator.replica.packetEpoch, appearance: pendingAppearance });
      }
      if (phase === 'login') {
        if (menuOpen) assert(status.phase === 'ready', 'Creator opened before projection release');
        if (!menuOpen || state?.creation !== 'editing' || status.phase !== 'ready') return;
        assert(projection && projection.instance === '' && self && !self.appearance,
          'Fixture must create a fresh character in the shared world');
        assert(restoreControls, 'Initial native control lease was not observed');
        if (nativeArrayInputs && !arrayCasesComplete) runArrayInputCases();
        selfIdx = self.idx; initialEpoch = coordinator.replica.packetEpoch; initialVersion = status.version;
        initialAppearanceChanges = self.numAppearanceChanges || 0; editingSince = Date.now();
        setPhase('editing', 60000);
      }
      if (phase === 'editing') {
        assert(menuOpen && state.creation === 'editing', 'Creator disappeared before acceptance');
        assert(status.phase === 'ready' && coordinator.replica.packetEpoch === initialEpoch, 'Editing restarted projection');
        assert(gameplayHeld(flags) && flags[5], 'Creator must retain gameplay gate and permit menu controls');
        const gear = preview(player);
        assert(!gear.worn.length && !gear.left && !gear.right, 'Preview gear was reapplied');
        ++editingSamples;
        if (Date.now() - editingSince < 45000) return; // Exceeds the old 30-second transfer deadline.
        log('editing-observed', { durationMs: Date.now() - editingSince, editingSamples });
        invoke(raceMenu, panel + '.onDoneClicked', []);
        setPhase('confirming-ui', 15000); return;
      }
      if (phase === 'confirming-ui') {
        assert(gameplayHeld(flags), 'Gameplay released during native confirmation');
        if (nameEntryReady()) {
          sp.Ui.setString(raceMenu, input + '.text', desiredName);
          setPhase('writing-name', 10000); return;
        }
        if (sp.Ui.isMenuOpen('MessageBoxMenu') && !confirmationPressed) {
          const prompt = sp.Ui.getString('MessageBoxMenu', messageBox + '.MessageText.text');
          const yes = sp.Ui.getString('MessageBoxMenu', yesButton + '.ButtonText.text');
          const no = sp.Ui.getString('MessageBoxMenu', messageBox + '.Buttons.Button1.ButtonText.text');
          log('native-confirmation', { prompt, yes, no });
          assert(/finish|name.*character/i.test(prompt) && /^(yes|ok)$/i.test(yes.trim()) && /^(no|cancel)$/i.test(no.trim()),
            'Unexpected confirmation menu; refusing to press a different dialog');
          invoke('MessageBoxMenu', yesButton + '.onPress', [0, 0, 0]);
          confirmationPressed = true;
        }
        return;
      }
      if (phase === 'writing-name') {
        assert(nameEntryReady() && gameplayHeld(flags), 'Name entry lost its native state');
        if (sp.Ui.getString(raceMenu, input + '.text') !== desiredName) return;
        log('native-name-observed', { value: desiredName });
        invoke(raceMenu, entry + '.onAccept', []);
        setPhase('awaiting-close', 45000); return;
      }
      if (phase === 'awaiting-close') {
        assert(coordinator.replica.packetEpoch === initialEpoch && status.version === initialVersion,
          'An unrelated baseline cannot substitute for the creation echo');
        if (menuOpen || !pendingAppearance || !self || self.idx !== selfIdx) return;
        if (!accepted && !state.pendingAppearance && state.creation === 'idle' &&
            (self.numAppearanceChanges || 0) > initialAppearanceChanges && equal(self.appearance, pendingAppearance)) {
          accepted = true;
          log('authoritative-self-echo-observed', { epoch: initialEpoch, idx: selfIdx, appearance: self.appearance });
        }
        if (!accepted || !loadoutSettled(state) || !player.is3DLoaded() || !equal(flags, restoreControls)) return;
        if (++settledSamples < 5) return;
        assert(status.phase === 'ready' && projection.instance === '', 'Unexpected creation world');
        original = inspect(1080569);
        assert(original && !original.disabled && !original.dead, 'Projection fixture original is unavailable');
        log('creation-settled', { controls: flags, state, original });
        // A normal player command after the real acknowledgement, never an ack
        // fabricated by the harness. The production coordinator handles entry.
        sp.mpClientPlugin.send(JSON.stringify({ t: 34, epoch: coordinator.replica.packetEpoch, message: {
          t: 1, contentJsonDump: JSON.stringify({ customPacketType: 'boyhouse.instance', action: 'enter', area: 'projection_test' })
        } }), true);
        setPhase('entering', 30000); return;
      }
      if (status.phase !== 'ready' || !projection || !loadoutSettled(state) || Date.now() < waitUntil) return;
      assert(!menuOpen, 'Completed creator reopened during the phase roundtrip');
      assert(equal(self?.appearance, pendingAppearance), 'Accepted appearance changed during the phase roundtrip');
      if (phase === 'entering' && projection.instance) {
        const mapping = projection.references.find(ref => ref.original === original.id);
        assert(mapping && mapping.baseId === original.baseId, 'Projection fixture did not map');
        const hidden = inspect(original.id);
        assert(hidden.disabled && hidden.blocked && hidden.dead === original.dead && hidden.health === original.health,
          'Original suppression changed native gameplay state');
        copies = projection.references.filter(ref => !ref.disabled && !ref.deleted).map(ref => {
          const local = view.getLocalRefrId(ref.copy), copy = inspect(local);
          assert(copy && copy.handle && copy.loaded && !copy.disabled && copy.baseId === ref.baseId,
            'Projected copy is not ready: ' + ref.original);
          return { ...ref, local, handle: copy.handle };
        });
        assert(copies.length && equal(flags, restoreControls), 'Entry did not restore the settled player');
        enteredVersion = status.version; log('entered', { copies, hidden, version: enteredVersion });
        setPhase('leaving', 45000); waitUntil = Date.now() + 1000;
      } else if (phase === 'leaving' && projection.instance) {
        assert(++leaveSteps <= 80, 'Player could not cross the phased boundary');
        sp.TESModPlatform.moveRefrToPosition(player, null, player.getWorldSpace(),
          player.getPositionX() + 64, player.getPositionY(), player.getPositionZ(), 0, 0, 72);
        waitUntil = Date.now() + 250;
      } else if (phase === 'leaving' && !projection.instance && status.version > enteredVersion) {
        const restored = inspect(original.id);
        assert(restored && restored.disabled === original.disabled && restored.blocked === original.blocked &&
          restored.dead === original.dead && restored.health === original.health, 'Original did not restore after exit');
        for (const copy of copies) {
          assert(sp.getReferenceHandle(copy.local) !== copy.handle, 'A private copy survived exit');
          assert(view.getProjectedLocalRefrId(copy.original) === undefined, 'A private alias survived exit');
        }
        assert(equal(flags, restoreControls), 'Controls did not restore after the roundtrip');
        finish(true, { name: self.appearance.name, epoch: coordinator.replica.packetEpoch,
          creationEpoch: initialEpoch, version: status.version, copiesRemoved: copies.length, restored, controls: flags });
      }
    } catch (error) { finish(false, String(error.stack || error)); }
  });
  log('loaded', { platform: sp.getPlatformVersion(), desiredName, profileId: 2026111650, nativeArrayInputs,
    menuDriver: 'observed Skyrim Scaleform callbacks; no desktop input' });
})();
`;
new vm.Script(observer, { filename: 'character-creation-observer.js' });
fs.writeFileSync(output, client + '\n' + observer);
console.log(output);
