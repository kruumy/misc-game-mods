using System.Collections.Generic;
using Factory;
using HarmonyLib;
using UnityEngine;

namespace MMMultiplayer
{
    // Rewired reads raw input and never sees a press that activates the window, so the whole click is lost
    // (constant when switching between game windows). Unity's own input does see it: replay such presses to the game.
    [HarmonyPatch(typeof(RuntimeAppCommandSource), nameof(RuntimeAppCommandSource.GetFrameCommands))]
    internal static class FocusClickFix
    {
        private static readonly AccessTools.FieldRef<RuntimeAppCommandSource, Rewired.Player> Player =
            AccessTools.FieldRefAccess<RuntimeAppCommandSource, Rewired.Player>("_rewiredPlayer");
        private static readonly AccessTools.FieldRef<RuntimeAppCommandSource, Scope> ScopeRef =
            AccessTools.FieldRefAccess<RuntimeAppCommandSource, Scope>("_scope");

        private static readonly (int action, int button)[] Buttons = { (19, 0), (20, 1) }; // LeftMouse, RightMouse
        private static readonly int[] UnseenFrames = new int[2];
        private static readonly bool[] Injected = new bool[2];

        private static void Postfix(RuntimeAppCommandSource __instance, IEnumerable<IAppCommand> __result)
        {
            var player = Player(__instance);
            if (player == null || !(__result is List<IAppCommand> commands) || commands.Count == 0) return;
            var mouse = player.controllers.Mouse;
            for (int i = 0; i < Buttons.Length; i++)
            {
                var (action, button) = Buttons[i];
                bool unityHeld = Input.GetMouseButton(button);
                bool rewiredHeld = mouse.GetButton(button);
                if (Injected[i])
                {
                    if (rewiredHeld) Injected[i] = false; // Rewired caught up and will report the release itself
                    else if (!unityHeld)
                    {
                        Inject(__instance, commands, action, InputEventButtonState.JustUp, mouse.screenPosition);
                        Injected[i] = false;
                    }
                    continue;
                }
                // Wait a frame so a press Rewired merely reports late isn't doubled.
                UnseenFrames[i] = unityHeld && !rewiredHeld ? UnseenFrames[i] + 1 : 0;
                if (UnseenFrames[i] == 2)
                {
                    Inject(__instance, commands, action, InputEventButtonState.JustDown, mouse.screenPosition);
                    Injected[i] = true;
                }
            }
        }

        private static void Inject(RuntimeAppCommandSource source, List<IAppCommand> commands, int action, InputEventButtonState state, Vector2 position)
        {
            var scope = ScopeRef(source);
            var cmd = scope.Get<ProcessInputEventCommand>();
            cmd.Configure(commands[commands.Count - 1].Timestamp, InputEvent.CreateMouseEvent(scope, action, state, position));
            commands.Insert(commands.Count - 1, cmd); // before the frame's TickAppCommand, like real input
        }
    }
}
