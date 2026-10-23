using System;
using System.Collections.Generic;
using HarmonyLib;
using Motorways;
using Motorways.Commands;
using Motorways.Models;
using Motorways.Views;

namespace MMMultiplayer
{
    // Picking a weekly upgrade does UI work only on the picker's machine (bumps numChoicesMade, plays the fly-in that
    // moves the counts onto the bar, closes the screen). When another player's pick executes here, replay that UI.
    [HarmonyPatch(typeof(SelectUpgradeCommand), nameof(SelectUpgradeCommand.Execute))]
    internal static class UpgradeSync
    {
        public static readonly HashSet<SelectUpgradeCommand> LocalPicks = new HashSet<SelectUpgradeCommand>();
        public static bool Mirroring;

        private static readonly AccessTools.FieldRef<SelectUpgradeCommand, int> UpgradeIndex =
            AccessTools.FieldRefAccess<SelectUpgradeCommand, int>("_upgradeIndex");
        private static readonly AccessTools.FieldRef<GameUpgradeScreen, int[]> ButtonToUpgrade =
            AccessTools.FieldRefAccess<GameUpgradeScreen, int[]>("_buttonIndexToUpgradeIndex");

        private static void Prefix(SelectUpgradeCommand __instance, Server.ISimulation simulation, out UpgradePackageDefinition? __state)
        {
            __state = null;
            if (Session.Game == null || simulation != Session.Sim) return;
            var model = simulation.GetModel<UpgradeDatabaseModel>();
            bool valid = model.pendingUpgradeChoices.Count > 0;
            if (LocalPicks.Remove(__instance))
            {
                if (!valid) model.numChoicesMade--; // someone else claimed this week's pick first; undo our UI's increment
                return;
            }
            if (!valid) return;

            int index = UpgradeIndex(__instance);
            int before = model.numChoicesMade;
            var screen = Session.Game.Scope.Get<ScreenStack>().GetActiveScreen<GameUpgradeScreen>();
            int button = screen != null ? Array.IndexOf(ButtonToUpgrade(screen) ?? new int[0], index) : -1;
            if (button >= 0)
            {
                Mirroring = true;
                try { screen.OnUpgradeSelect(button); }
                catch (Exception e) { Session.Log.LogError(e); }
                finally { Mirroring = false; }
            }
            if (model.numChoicesMade != before) return; // the screen replayed the pick, animation included

            // Screen not up (or busy): balance the counter and fill the bar directly once the command has run.
            model.numChoicesMade++;
            var choices = model.pendingUpgradeChoices[0].choices;
            if (index >= 0 && index < choices.Count) __state = choices[index];
            screen?.CloseScreenAfterSelection();
        }

        private static void Postfix(UpgradePackageDefinition? __state)
        {
            if (!__state.HasValue) return;
            var bar = Session.Game?.Scope.Get<GameUIScreen>()?.UpgradeBar;
            if (bar == null) return;
            var package = __state.Value;
            bar.AddToUpgradeButtonStack(package.type, true, package.amount);
            if (package.additionalConcrete > 0) bar.AddToUpgradeButtonStack(UpgradeType.Concrete, true, package.additionalConcrete);
        }
    }
}
