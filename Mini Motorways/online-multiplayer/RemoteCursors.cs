using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace MMMultiplayer
{
    // Other players' mouse pointers, in map (world) space so they stay put regardless of each player's camera.
    public static class RemoteCursors
    {
        private const float SendInterval = 0.05f, KeepAlive = 1f, Timeout = 5f;

        private class Cursor
        {
            public Vector2 Target, Shown;
            public float LastSeen;
            public bool Placed;
        }

        private static readonly Dictionary<int, Cursor> Cursors = new Dictionary<int, Cursor>();
        private static readonly Color[] Palette =
        {
            new Color(0.95f, 0.35f, 0.35f), new Color(0.3f, 0.65f, 1f), new Color(0.4f, 0.85f, 0.4f),
            new Color(1f, 0.75f, 0.2f), new Color(0.8f, 0.45f, 1f), new Color(0.2f, 0.85f, 0.85f),
        };

        private static Vector2 _lastSent;
        private static float _lastSendTime;
        private static GUIStyle _arrow, _name;

        public static Color ColorOf(int peer) => Palette[peer % Palette.Length];

        private static GameCamera Camera => Session.Game?.Scope.Get<GameCamera>();

        public static void Update()
        {
            var cam = Camera;
            if (cam == null) return;
            Vector2 world = cam.GetWorldFromScreen(Input.mousePosition);
            float now = Time.realtimeSinceStartup;
            if (now - _lastSendTime < SendInterval) return;
            if ((world - _lastSent).sqrMagnitude < 0.0001f && now - _lastSendTime < KeepAlive) return;
            _lastSent = world;
            _lastSendTime = now;
            Session.SendCursor(world);
        }

        public static void OnCursor(int peer, Vector2 world)
        {
            if (peer == Session.LocalPeer) return;
            if (float.IsNaN(world.x)) { Cursors.Remove(peer); return; } // player left
            if (!Cursors.TryGetValue(peer, out var c)) Cursors[peer] = c = new Cursor();
            c.Target = world;
            c.LastSeen = Time.realtimeSinceStartup;
            if (!c.Placed) { c.Shown = world; c.Placed = true; }
        }

        public static void Remove(int peer) => Cursors.Remove(peer);
        public static void Clear() => Cursors.Clear();

        public static void Draw()
        {
            var cam = Camera;
            if (cam == null || Cursors.Count == 0) return;
            if (_arrow == null) CreateStyles();
            float now = Time.realtimeSinceStartup;
            float blend = 1f - Mathf.Exp(-20f * Time.unscaledDeltaTime);
            var oldColor = GUI.color;
            foreach (var kv in Cursors)
            {
                var c = kv.Value;
                if (now - c.LastSeen > Timeout) continue;
                if (Event.current.type == EventType.Repaint) c.Shown = Vector2.Lerp(c.Shown, c.Target, blend);
                Vector2 screen = cam.GetScreenFromWorld((Vector3)c.Shown);
                float x = screen.x, y = Screen.height - screen.y;
                GUI.color = ColorOf(kv.Key);
                GUI.Label(new Rect(x, y, 22, 33), "", _arrow);
                GUI.color = Color.white;
                var label = new GUIContent(Session.Peers.TryGetValue(kv.Key, out var p) ? p.Name : "?");
                _name.normal.textColor = ColorOf(kv.Key);
                Vector2 size = _name.CalcSize(label);
                GUI.Label(new Rect(x + 18, y + 26, size.x, size.y), label, _name);
            }
            GUI.color = oldColor;
        }

        // Arrow pointer: white fill with a dark outline, tinted per player through GUI.color.
        private static void CreateStyles()
        {
            const int w = 16, h = 24;
            var tex = new Texture2D(w, h);
            for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int row = h - 1 - y; // texture rows go bottom-up, the arrow tip is at the top-left
                bool inside = x <= row * 0.6f && row < 20 || row >= 20 && x <= 3;
                bool edge = inside && (x == 0 || x >= row * 0.6f - 1.2f || row >= 19 && x <= 3 && row == h - 1);
                tex.SetPixel(x, y, !inside ? Color.clear : edge ? new Color(0.1f, 0.1f, 0.1f) : Color.white);
            }
            tex.Apply();
            _arrow = new GUIStyle();
            _arrow.normal.background = tex;
            var pill = new Texture2D(1, 1);
            pill.SetPixel(0, 0, new Color(0.08f, 0.08f, 0.1f, 0.85f));
            pill.Apply();
            _name = new GUIStyle(GUI.skin.label) { fontSize = 15, padding = new RectOffset(6, 6, 2, 2) };
            _name.normal.background = pill;
        }
    }
}
