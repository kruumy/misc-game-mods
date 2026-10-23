using HarmonyLib;
using Motorways.Views;

namespace MMMultiplayer
{
    // Buttons that would start, restart or re-rule a city on one machine only. Disabled while a shared city runs.
    [HarmonyPatch]
    internal static class Lockdown
    {
        [HarmonyPrefix]
        [HarmonyPatch(typeof(PauseScreen), nameof(PauseScreen.OnRestart))]
        [HarmonyPatch(typeof(PauseScreen), nameof(PauseScreen.OnMovieButtonPressed))]
        [HarmonyPatch(typeof(PauseScreen), nameof(PauseScreen.OnCinematicModeButtonPressed))]
        [HarmonyPatch(typeof(GameOverScreen), nameof(GameOverScreen.OnRestart))]
        [HarmonyPatch(typeof(GameOverScreen), nameof(GameOverScreen.OnContinueInEndlessMode))]
        [HarmonyPatch(typeof(GameOverScreen), nameof(GameOverScreen.OnContinueInCreativeMode))]
        [HarmonyPatch(typeof(GameOverScreen), nameof(GameOverScreen.OnChallengeButtonsPressed))]
        private static bool BlockInMultiplayer() => Session.Game == null;
    }
}
