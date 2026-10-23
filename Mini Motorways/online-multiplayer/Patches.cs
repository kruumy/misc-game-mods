using HarmonyLib;
using Motorways;
using Motorways.Models;
using Server;

namespace MMMultiplayer
{
    [HarmonyPatch(typeof(MotorwaysGame), nameof(MotorwaysGame.Start))]
    internal static class GameStartPatch
    {
        private static void Prefix(MotorwaysGame __instance, GameMode mode, MapChallenge mapChallenge)
        {
            bool resumed = __instance.Simulation.GetModel<CityModel>() != null;
            if (!Session.ShouldAttach(mode, mapChallenge, resumed))
            {
                // The menu's background city and the movie/cinematic viewers aren't real games.
                if (Session.Active && mode != GameMode.Background && mode != GameMode.Movie && mode != GameMode.Cinematic)
                    Session.RefuseLocalGame(resumed ? "Saved cities can't be played in multiplayer. Start a new city instead."
                        : "Only Classic, Expert and Endless cities can be played in multiplayer.");
                return;
            }
            Session.AttachGame(__instance);
            Session.StartingMode = mode;
            Session.InGameStart = true;
        }

        private static void Postfix() => Session.InGameStart = false;
    }

    [HarmonyPatch(typeof(Random), nameof(Random.NextSimulationSeed))]
    internal static class SeedPatch
    {
        private static void Postfix(ref uint __result)
        {
            if (Session.InGameStart) __result = (uint)Session.OnSeedRolled(__result);
        }
    }

    [HarmonyPatch(typeof(Simulation), nameof(Simulation.ScheduleCommand))]
    internal static class ScheduleCommandPatch
    {
        private static bool Prefix(Simulation __instance, Command command, ref bool __result)
        {
            __result = true;
            return Session.OnScheduleCommand(__instance, command);
        }
    }

    [HarmonyPatch(typeof(Game), nameof(Game.Tick))]
    internal static class GameTickPatch
    {
        private static readonly AccessTools.FieldRef<Game, TimeInterval> Interval = AccessTools.FieldRefAccess<Game, TimeInterval>("_timeInterval");
        private static readonly AccessTools.FieldRef<Game, Client.IClient> View = AccessTools.FieldRefAccess<Game, Client.IClient>("_view");
        private static readonly System.Action<Game, TimeInterval> Adjust =
            AccessTools.MethodDelegate<System.Action<Game, TimeInterval>>(AccessTools.Method(typeof(Game), "AdjustTimeInterval"));

        private static bool Prefix(Game __instance, float frameTime)
        {
            if (__instance != Session.Game) return true;
            var interval = Interval(__instance);
            interval.UnsyncedDelta = frameTime;
            interval.Delta = frameTime;
            Adjust(__instance, interval);
            Session.Tick(__instance, interval, View(__instance));
            return false;
        }
    }

    [HarmonyPatch(typeof(Motorways.Views.GameUIScreen))]
    internal static class ClockButtonsPatch
    {
        [HarmonyPostfix]
        [HarmonyPatch(nameof(Motorways.Views.GameUIScreen.OnPausePressed))]
        [HarmonyPatch(nameof(Motorways.Views.GameUIScreen.OnPlayPressed))]
        [HarmonyPatch(nameof(Motorways.Views.GameUIScreen.OnFastForwardPressed))]
        [HarmonyPatch(nameof(Motorways.Views.GameUIScreen.OnExtraFastForwardPressed))]
        private static void Postfix() => Session.OnLocalClockChanged();
    }

    // A shared city must not overwrite the player's single-player save slot.
    [HarmonyPatch(typeof(MotorwaysGame), nameof(MotorwaysGame.TrySave))]
    internal static class NoSavePatch
    {
        private static bool Prefix(MotorwaysGame __instance, ref bool __result)
        {
            if (__instance != Session.Game) return true;
            __result = false;
            return false;
        }
    }

    [HarmonyPatch(typeof(MotorwaysGame), nameof(MotorwaysGame.OnGameEnd))]
    internal static class GameEndPatch
    {
        private static void Postfix(MotorwaysGame __instance)
        {
            if (__instance == Session.Game) Session.OnGameEnded();
        }
    }

    [HarmonyPatch(typeof(Game), nameof(Game.OnReleasedFromScope))]
    internal static class GameReleasedPatch
    {
        private static void Prefix(Game __instance)
        {
            if (__instance == Session.Game) Session.DetachGame();
        }
    }
}
