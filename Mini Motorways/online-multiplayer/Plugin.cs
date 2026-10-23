using System;
using System.Linq;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

namespace MMMultiplayer
{
    [BepInPlugin("kruumy.minimotorways.multiplayer", "Mini Motorways Multiplayer", "0.1.0")]
    public class Plugin : BaseUnityPlugin
    {
        public static Plugin Instance;

        private ConfigEntry<KeyCode> _debugKey;
        private bool _showDebug;
        private Rect _window = new Rect(20, 20, 320, 200);

        private void Awake()
        {
            Instance = this;
            Session.Log = Logger;
            var name = Config.Bind("General", "PlayerName", Environment.UserName);
            var address = Config.Bind("Direct", "Address", "127.0.0.1");
            var port = Config.Bind("Direct", "Port", 27777);
            _debugKey = Config.Bind("Debug", "OverlayKey", KeyCode.F8, "Toggles the network debug overlay.");
            new Harmony("kruumy.minimotorways.multiplayer").PatchAll();
            MultiplayerMenu.Create(name, address, port);
            Logger.LogInfo("Multiplayer loaded");
        }

        private void Update()
        {
            if (Input.GetKeyDown(_debugKey.Value)) _showDebug = !_showDebug;
            try { Session.Update(this); }
            catch (Exception e) { Logger.LogError(e); }
        }

        private void OnApplicationQuit() => Session.Leave();

        private void OnGUI()
        {
            RemoteCursors.Draw();
            if (_showDebug) _window = GUI.Window(0x4D4D4D, _window, DrawDebug, "Multiplayer debug");
        }

        private void DrawDebug(int id)
        {
            GUILayout.Label(Session.Status);
            var stack = FindObjectOfType<AppRuntime>()?.App?.Scope.Get<ScreenStack>();
            if (stack != null) GUILayout.Label($"Screen: {stack.GetTopActiveScreenType()}  in game: {stack.IsInGame()}");
            foreach (var kv in Session.Peers.OrderBy(p => p.Key))
                GUILayout.Label($"{kv.Value.Name}{(kv.Key == Session.LocalPeer ? " (you)" : "")}: {kv.Value.Score}{(kv.Value.Ended ? " - done" : "")}{(kv.Value.Rtt > 0 ? $"  ({kv.Value.Rtt * 1000:F0} ms)" : "")}");
            if (Session.Game != null)
                GUILayout.Label($"Frame {Session.Clock.FrameCount} / {Session.IssuedFrame}  lead {Session.Lead}");
            GUI.DragWindow(new Rect(0, 0, 10000, 20));
        }
    }
}
