using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using BepInEx.Logging;
using FixMath;
using Motorways;
using Motorways.Commands;
using Motorways.Models;
using MMMultiplayer.Net;
using Server;
using UnityEngine;

namespace MMMultiplayer
{
    public enum MpMode : byte { Coop, Pvp }

    internal enum Msg : byte { Hello = 1, Welcome, StartGame, Submit, Turn, Ack, Clock, Cursor, Lobby, EndGame, Reject }

    public class PeerState
    {
        public string Name = "?";
        public int Frame = -1; // -1: not in the current game yet
        public int Score;
        public bool Ended;
        public float Rtt;
    }

    // Deterministic lockstep: the host decides up to which simulation frame everyone may step ("issued frame")
    // and stamps every player command with the frame it executes on. Coop shares commands; PvP only shares time.
    public static class Session
    {
        // Frames the host lets everyone run ahead of the slowest peer; sized from measured round trip time.
        public static int Lead = 6;
        private static float StepsPerSecond => 10f * (Game?.GetTimeScale().Scale ?? 1f);

        public static ManualLogSource Log;
        public static ITransport Transport;
        public static MpMode Mode;
        public static int LocalPeer;
        public static string LocalName = "Player";
        public static string Status = "Offline";
        public static readonly Dictionary<int, PeerState> Peers = new Dictionary<int, PeerState>();

        public static MotorwaysGame Game;
        public static ISimulation Sim;
        public static Server.Clock Clock;
        public static int IssuedFrame;
        public static Fix64 Accumulated;
        public static bool Injecting;
        public static bool InGameStart;
        public static GameMode StartingMode;

        private struct TurnCommand { public int Origin, LocalId, Frame; public byte[] Data; }

        private static readonly List<TurnCommand> HostQueue = new List<TurnCommand>();
        private static readonly Dictionary<int, Command> Awaiting = new Dictionary<int, Command>();
        private static int _nextLocalId;
        private static int _lastSentFrame = int.MinValue;
        private static ulong? _pendingSeed;
        private static GameStarter _starter;
        private static int _mapSelectFrames;
        private static float _lastHostTime;
        private static (bool paused, float scale) _lastSentClock;
        private static (bool paused, float scale)? _clockRequest;
        private static float _clockRequestTime;
        public static bool ApplyingClock;
        public const int ProtocolVersion = 1; // bump whenever the wire format or synced behaviour changes
        private const int MaxPlayers = 8, MaxNameLength = 20;
        public static string Notice = ""; // shown in the lobby, e.g. why a city was refused
        private static bool _refusedGamePending;
        public static bool ReturnToLobby; // set when the shared city ends; the menu reopens the lobby once back out

        private static readonly HarmonyLib.AccessTools.FieldRef<Game, TimeInterval> Interval =
            HarmonyLib.AccessTools.FieldRefAccess<Game, TimeInterval>("_timeInterval");

        private static (bool paused, float scale) ClockState() => (Interval(Game).IsPaused, Game.GetTimeScale().Scale);

        // Called after the local player pressed a clock button.
        public static void OnLocalClockChanged()
        {
            if (Game == null || ApplyingClock || IsHost) return; // the host's change rides out in the next turn
            var state = ClockState();
            _clockRequest = state;
            _clockRequestTime = Time.realtimeSinceStartup;
            Send(0, Msg.Clock, w => { w.Write(state.paused); w.Write(state.scale); });
        }

        private static void ApplyClock(bool paused, float scale)
        {
            ApplyingClock = true;
            try
            {
                var timeScale = ToTimeScale(scale);
                Game.SetPaused(paused, Plugins.Analytics.AnalyticsTypes.PauseReason.PauseButton);
                Game.SetTimeScale(timeScale);
                Game.Scope.Get<Motorways.Views.GameUIScreen>()?.SetVcrButtonState(paused, timeScale);
            }
            finally { ApplyingClock = false; }
        }

        public static bool Active => Transport != null;
        public static bool IsHost => Transport != null && Transport.IsHost;
        public static PeerState Local => Peers.TryGetValue(LocalPeer, out var p) ? p : null;

        public static void Host(ITransport transport, MpMode mode)
        {
            Transport = transport;
            Mode = mode;
            LocalPeer = 0;
            Peers.Clear();
            Peers[0] = new PeerState { Name = CleanName(LocalName) };
            Status = $"Hosting {mode}";
        }

        public static void Join(ITransport transport)
        {
            Transport = transport;
            Peers.Clear();
            Status = "Connecting...";
            Send(0, Msg.Hello, w => { w.Write(CleanName(LocalName)); w.Write(ProtocolVersion); });
        }

        public static void Leave()
        {
            Transport?.Close();
            Transport = null;
            Peers.Clear();
            DetachGame();
            Status = "Offline";
            Notice = "";
            _starter = null;
            _pendingSeed = null;
            _refusedGamePending = false;
        }

        public static void Update(MonoBehaviour host)
        {
            if (!Active) return;
            Transport.Poll(OnMessage);
            if (!Active) return;

            // There is no MainMenu -> InGame transition, so wait until map select is up like the real UI flow.
            if (_starter != null && _starter.CanStart)
            {
                var app = UnityEngine.Object.FindObjectOfType<AppRuntime>().App;
                var stack = app.Scope.Get<ScreenStack>();
                if (stack.GetTopActiveScreenType() != ScreenStack.MotorwaysScreen.MapSelect) _mapSelectFrames = 0;
                else if (++_mapSelectFrames > 60)
                {
                    var starter = _starter;
                    _starter = null;
                    starter.Start(stack, app.Scope);
                }
            }

            if (_refusedGamePending)
            {
                var stack = UnityEngine.Object.FindObjectOfType<AppRuntime>()?.App?.Scope.Get<ScreenStack>();
                if (stack != null && stack.IsInGame() && !stack.AreAnyScreensTransitioning)
                {
                    _refusedGamePending = false;
                    ExitToMenu();
                }
            }

            if (Game == null) return;
            var local = Local;
            if (local != null)
            {
                local.Frame = Clock.FrameCount;
                local.Score = Sim.GetModel<ScoreModel>()?.Score ?? 0;
            }
            RemoteCursors.Update();
            if (IsHost) HostIssue();
            else if (local != null && local.Frame != _lastSentFrame)
            {
                _lastSentFrame = local.Frame;
                Send(0, Msg.Ack, w => { w.Write(local.Frame); w.Write(local.Score); w.Write(local.Ended); w.Write(_lastHostTime); });
            }
        }

        private static void HostIssue()
        {
            var running = Peers.Values.Where(p => !p.Ended).ToList();
            if (running.Any(p => p.Frame < 0)) return; // someone is still loading the map
            // Enough frames in flight to cover the slowest round trip at the current speed, plus slack.
            float rtt = Peers.Values.Max(p => p.Rtt);
            Lead = Mathf.Clamp(Mathf.CeilToInt(rtt * StepsPerSecond) + 2, 2, 30);
            int target = running.Count == 0 ? IssuedFrame : running.Min(p => p.Frame) + Lead;
            bool clockChanged = ClockState() != _lastSentClock;
            if (target <= IssuedFrame && HostQueue.Count == 0 && !clockChanged) return;
            _lastSentClock = ClockState();

            for (int i = 0; i < HostQueue.Count; i++)
            {
                var c = HostQueue[i];
                c.Frame = IssuedFrame;
                HostQueue[i] = c;
            }
            IssuedFrame = Math.Max(IssuedFrame + (HostQueue.Count > 0 ? 1 : 0), Math.Max(target, IssuedFrame));
            var queued = HostQueue.ToList();
            HostQueue.Clear();
            foreach (var c in queued)
            {
                if (c.Origin == 0 && Awaiting.TryGetValue(c.LocalId, out var mine))
                {
                    Awaiting.Remove(c.LocalId);
                    Schedule(mine, c.Frame);
                }
                else Schedule(Deserialize(c.Data), c.Frame);
            }
            Transport.Broadcast(Pack(Msg.Turn, w =>
            {
                w.Write(IssuedFrame);
                w.Write(Lead);
                w.Write(Time.realtimeSinceStartup);
                w.Write(_lastSentClock.paused);
                w.Write(_lastSentClock.scale);
                w.Write(Peers.Count);
                foreach (var kv in Peers)
                {
                    w.Write(kv.Key); w.Write(kv.Value.Name); w.Write(kv.Value.Score); w.Write(kv.Value.Ended);
                }
                w.Write(queued.Count);
                foreach (var c in queued)
                {
                    w.Write(c.Origin); w.Write(c.LocalId); w.Write(c.Frame); w.Write(c.Data.Length); w.Write(c.Data);
                }
            }));
        }

        // A malformed or unexpected message is dropped on its own; it must never take the session down.
        private static void OnMessage(int peer, byte[] data)
        {
            if (data != null && data.Length == 0) return;
            if (IsHost && data != null && (Msg)data[0] != Msg.Hello && !Peers.ContainsKey(peer)) return; // refused or unknown peer
            try { HandleMessage(peer, data); }
            catch (Exception e) { Log.LogError($"Bad message from peer {peer}: {e}"); }
        }

        private static string CleanName(string name)
        {
            name = new string((name ?? "").Where(c => !char.IsControl(c) && c != '<' && c != '>').ToArray()).Trim();
            if (name.Length > MaxNameLength) name = name.Substring(0, MaxNameLength).Trim();
            return name.Length == 0 ? "Player" : name;
        }

        // Called when this machine starts a city multiplayer can't share; send everyone back to the lobby instead.
        public static void RefuseLocalGame(string reason)
        {
            Notice = reason;
            _refusedGamePending = true;
        }

        private static void HandleMessage(int peer, byte[] data)
        {
            if (data == null)
            {
                OnDisconnect(peer);
                return;
            }
            var r = new BinaryReader(new MemoryStream(data));
            var type = (Msg)r.ReadByte();
            switch (type)
            {
                case Msg.Hello:
                    {
                        string name = CleanName(r.ReadString());
                        int version = r.BaseStream.Position < r.BaseStream.Length ? r.ReadInt32() : 0;
                        string refusal = version != ProtocolVersion ? "The host is running a different version of the multiplayer mod."
                            : Game != null ? "That game is already in progress. Join when the host is back in the lobby."
                            : Peers.Count >= MaxPlayers ? "The lobby is full."
                            : null;
                        if (refusal != null)
                        {
                            Send(peer, Msg.Reject, w => w.Write(refusal));
                            break;
                        }
                        Peers[peer] = new PeerState { Name = name };
                    }
                    Send(peer, Msg.Welcome, w => { w.Write(peer); w.Write((byte)Mode); });
                    Status = $"Hosting {Mode} - {Peers.Count} players";
                    BroadcastLobby();
                    break;
                case Msg.Welcome:
                    LocalPeer = r.ReadInt32();
                    Mode = (MpMode)r.ReadByte();
                    Peers[LocalPeer] = new PeerState { Name = CleanName(LocalName) };
                    Status = $"Joined {Mode} as player {LocalPeer}, waiting for host to start";
                    break;
                case Msg.Reject:
                    {
                        string reason = r.ReadString();
                        Leave();
                        Status = reason;
                    }
                    break;
                case Msg.EndGame:
                    ExitToMenu();
                    break;
                case Msg.Lobby:
                    {
                        int count = r.ReadInt32();
                        var names = new Dictionary<int, string>();
                        for (int i = 0; i < count; i++) names[r.ReadInt32()] = r.ReadString();
                        foreach (int id in Peers.Keys.Where(id => !names.ContainsKey(id)).ToList()) Peers.Remove(id);
                        foreach (var kv in names)
                        {
                            if (!Peers.TryGetValue(kv.Key, out var lobbyPeer)) Peers[kv.Key] = lobbyPeer = new PeerState();
                            lobbyPeer.Name = kv.Value;
                        }
                    }
                    break;
                case Msg.StartGame:
                    OnStartGame(r.ReadString(), (GameMode)r.ReadInt32(), r.ReadUInt64());
                    break;
                case Msg.Submit:
                    HostQueue.Add(new TurnCommand { Origin = peer, LocalId = r.ReadInt32(), Data = r.ReadBytes(r.ReadInt32()) });
                    break;
                case Msg.Ack:
                    if (Peers.TryGetValue(peer, out var ps))
                    {
                        ps.Frame = r.ReadInt32(); ps.Score = r.ReadInt32(); ps.Ended = r.ReadBoolean();
                        float echoed = r.ReadSingle();
                        if (echoed > 0) ps.Rtt = Mathf.Lerp(ps.Rtt, Time.realtimeSinceStartup - echoed, 0.2f);
                    }
                    break;
                case Msg.Cursor:
                    {
                        int id = r.ReadInt32();
                        var pos = new Vector2(r.ReadSingle(), r.ReadSingle());
                        if (IsHost) { id = peer; RelayExcept(peer, Pack(Msg.Cursor, w => { w.Write(id); w.Write(pos.x); w.Write(pos.y); })); }
                        RemoteCursors.OnCursor(id, pos);
                    }
                    break;
                case Msg.Clock:
                    if (Game != null) ApplyClock(r.ReadBoolean(), r.ReadSingle());
                    break;
                case Msg.Turn:
                    OnTurn(r);
                    break;
            }
        }

        private static void OnTurn(BinaryReader r)
        {
            int issued = r.ReadInt32();
            Lead = r.ReadInt32();
            _lastHostTime = r.ReadSingle();
            var clock = (paused: r.ReadBoolean(), scale: r.ReadSingle());
            int peerCount = r.ReadInt32();
            for (int i = 0; i < peerCount; i++)
            {
                int id = r.ReadInt32();
                if (!Peers.TryGetValue(id, out var ps)) Peers[id] = ps = new PeerState();
                ps.Name = r.ReadString();
                int score = r.ReadInt32();
                bool ended = r.ReadBoolean();
                if (id != LocalPeer) { ps.Score = score; ps.Ended = ended; }
            }
            int count = r.ReadInt32();
            for (int i = 0; i < count; i++)
            {
                int origin = r.ReadInt32(), localId = r.ReadInt32(), frame = r.ReadInt32();
                byte[] bytes = r.ReadBytes(r.ReadInt32());
                if (Game == null) continue;
                if (origin == LocalPeer && Awaiting.TryGetValue(localId, out var mine))
                {
                    Awaiting.Remove(localId);
                    Schedule(mine, frame);
                }
                else Schedule(Deserialize(bytes), frame);
            }
            if (Game == null) return;
            IssuedFrame = Math.Max(IssuedFrame, issued);
            // Hold our own clock change until the host has adopted it, or the stale turn state would undo it.
            if (_clockRequest.HasValue)
            {
                if (clock == _clockRequest.Value || Time.realtimeSinceStartup - _clockRequestTime > 2f) _clockRequest = null;
                else return;
            }
            if (Mode == MpMode.Coop && clock != ClockState()) ApplyClock(clock.paused, clock.scale);
        }

        private static void OnDisconnect(int peer)
        {
            if (!IsHost)
            {
                Log.LogWarning("Lost connection to host");
                Leave();
                Status = "The host left the game";
                ExitToMenu();
                return;
            }
            Peers.Remove(peer);
            RemoteCursors.Remove(peer);
            Transport.Broadcast(Pack(Msg.Cursor, w => { w.Write(peer); w.Write(float.NaN); w.Write(float.NaN); }));
            Status = $"Hosting {Mode} - {Peers.Count} players";
            BroadcastLobby();
        }

        // ---- game lifecycle (called from patches) ----

        public static bool ShouldAttach(GameMode mode, MapChallenge challenge, bool resumed)
        {
            if (!Active || resumed || challenge != null) return false;
            if (mode != GameMode.Normal && mode != GameMode.Expert && mode != GameMode.Endless) return false;
            return IsHost || _pendingSeed.HasValue;
        }

        public static void AttachGame(MotorwaysGame game)
        {
            if (Game == game) return;
            Notice = "";
            Game = game;
            Sim = game.Simulation;
            Clock = Sim.Scope.Get<Server.Clock>();
            IssuedFrame = Lead; // lets InitCityCommand run everywhere while the host waits for peers to load
            Accumulated = Sim.Timestep;
            HostQueue.Clear();
            Awaiting.Clear();
            _lastSentFrame = int.MinValue;
            foreach (var p in Peers.Values) { p.Frame = -1; p.Score = 0; p.Ended = false; }
            Log.LogInfo($"Attached multiplayer game ({Mode}) to {game.MapDefinition?.cityName}");
        }

        public static void DetachGame()
        {
            if (Game == null) return;
            RemoteCursors.Clear();
            if (Local != null) Local.Ended = true;
            if (Active && !IsHost) Send(0, Msg.Ack, w => { w.Write(Local?.Frame ?? 0); w.Write(Local?.Score ?? 0); w.Write(true); w.Write(_lastHostTime); });
            if (IsHost)
            {
                Transport.Broadcast(Pack(Msg.EndGame, w => { })); // host left the city: everyone goes back to the lobby
                ReturnToLobby = true;
            }
            Game = null;
            Sim = null;
            Clock = null;
        }

        private static void ExitToMenu()
        {
            _starter = null;
            ReturnToLobby = true;
            var app = UnityEngine.Object.FindObjectOfType<AppRuntime>()?.App;
            var stack = app?.Scope.Get<ScreenStack>();
            if (stack == null || !stack.IsInGame()) return;
            // Same steps as the pause menu's "Main menu" button. (ScreenStack.ExitToMainMenu waits for the main menu
            // to be on top, which never happens from here, and leaves all input blocked.)
            var game = app.Game as MotorwaysGame;
            if (game != null && !game.HasGameEnded)
            {
                game.StopAudio();
                game.OnGameEnd(GameEndReason.Exit);
            }
            game?.Scope.Get<Motorways.Views.GameUIScreen>()?.SetUIVisible(false, false, true);
            if (stack.IsScreenInStack<Motorways.Views.MapSelectScreen>()) stack.PopToScreenOfType(ScreenStack.MotorwaysScreen.MapSelect);
            else stack.ReplaceScreens(ScreenStack.MotorwaysScreen.MapSelect, (Motorways.Views.MapSelectScreen s) => s.PrepareScreen(game), typeof(Motorways.Views.GameContainerScreen));
        }

        public static void OnGameEnded()
        {
            if (Local != null) Local.Ended = true;
            if (Active && !IsHost) Send(0, Msg.Ack, w => { w.Write(Local.Frame); w.Write(Local.Score); w.Write(true); w.Write(_lastHostTime); });
        }

        // Called with the seed the host's game just rolled; clients get theirs replaced with the host's.
        public static ulong OnSeedRolled(ulong seed)
        {
            if (!IsHost)
            {
                seed = _pendingSeed ?? seed;
                _pendingSeed = null;
                return seed;
            }
            Transport.Broadcast(Pack(Msg.StartGame, w =>
            {
                w.Write(Game.MapDefinition.cityName);
                w.Write((int)StartingMode);
                w.Write(seed);
            }));
            return seed;
        }

        private static void OnStartGame(string city, GameMode mode, ulong seed)
        {
            var app = UnityEngine.Object.FindObjectOfType<AppRuntime>().App;
            var map = app.Scope.Get<MapDatabase>().MapLibrary.GetMapByName(city);
            if (map == null)
            {
                Status = $"Host started unknown map '{city}'";
                return;
            }
            if (app.Game != null && app.Game != Game && ((MotorwaysGame)app.Game).StartedWithGameMode != GameMode.Background)
            {
                Status = "Host started a game - return to the main menu to join";
                return;
            }
            _pendingSeed = seed;
            _starter = new GameStarter(Plugin.Instance);
            if (!_starter.StartFromMapDefinition(map, mode)) _starter = null;
            var stack = app.Scope.Get<ScreenStack>();
            if (stack.GetTopActiveScreenType() != ScreenStack.MotorwaysScreen.MapSelect)
                stack.PushScreen(ScreenStack.MotorwaysScreen.MapSelect, (Motorways.Views.MapSelectScreen s) => s.PrepareScreen(null, false, true));
            Status = $"Joining {city}...";
        }

        // Returns true when the command should be scheduled locally right away (unsynced).
        public static bool OnScheduleCommand(Simulation sim, Command cmd)
        {
            if (Game == null || Injecting || sim != Sim) return true;
            if (cmd is InitCityCommand || cmd is SnapshotCommand) return true;
            if (cmd is SetPausedCommand && (Mode == MpMode.Pvp || !IsHost))
            {
                sim.Scope.Release(cmd); // only the coop host may pause the shared city
                return false;
            }
            if (Mode == MpMode.Pvp) return true; // own city: only time is shared
            if (cmd is SelectUpgradeCommand pick)
            {
                if (UpgradeSync.Mirroring)
                {
                    sim.Scope.Release(cmd); // replaying another player's pick in our UI; theirs is already scheduled
                    return false;
                }
                UpgradeSync.LocalPicks.Add(pick);
            }

            byte[] data = Serialize(cmd);
            if (IsHost)
            {
                HostQueue.Add(new TurnCommand { Origin = 0, LocalId = _nextLocalId, Data = data });
                Awaiting[_nextLocalId++] = cmd;
            }
            else
            {
                int id = _nextLocalId++;
                Awaiting[id] = cmd;
                Send(0, Msg.Submit, w => { w.Write(id); w.Write(data.Length); w.Write(data); });
            }
            return false;
        }

        // Replaces Game.Tick's fixed-step loop so the simulation never passes the issued frame.
        public static void Tick(Game game, TimeInterval interval, Client.IClient view)
        {
            // Time dilation: drift faster/slower to keep about half the lead buffered, so we rarely hit the wall and hitch.
            int behind = IssuedFrame - Clock.FrameCount;
            float rate = Mathf.Clamp(1f + 0.15f * (behind - Lead * 0.5f), 0.5f, 3f);
            Accumulated += (Fix64)(interval.UnpausedScaledDelta * rate);
            Fix64 step = Sim.Timestep;
            int budget = behind > Lead * 3 ? behind : 10; // catch up hard when far behind
            while (budget-- > 0 && Clock.FrameCount < IssuedFrame && (Accumulated >= step || behind > Lead * 3))
            {
                Sim.Step();
                Accumulated = Accumulated >= step ? Accumulated - step : Fix64.Zero;
            }
            if (Accumulated > step) Accumulated = step; // stalled waiting for the host: don't bank time
            view.Tick(interval, (float)(Accumulated / step));
        }

        private static void Schedule(Command cmd, int frame)
        {
            cmd.FrameIndex = frame;
            Injecting = true;
            try { Sim.ScheduleCommand(cmd); }
            finally { Injecting = false; }
        }

        private static byte[] Serialize(Command cmd)
        {
            var ms = new MemoryStream();
            var w = new BinaryWriter(ms);
            if (!Sim.Scope.Export(cmd, w)) Log.LogError($"Failed to serialize {cmd}");
            return ms.ToArray();
        }

        private static Command Deserialize(byte[] data) =>
            Sim.Scope.Import<Command>(new BinaryReader(new MemoryStream(data)));

        private static TimeScale ToTimeScale(float s) =>
            Mathf.Approximately(s, TimeScale.ExtraFast.Scale) ? TimeScale.ExtraFast : TimeScale.FromScale(s);

        private static byte[] Pack(Msg type, Action<BinaryWriter> body)
        {
            var ms = new MemoryStream();
            var w = new BinaryWriter(ms);
            w.Write((byte)type);
            body(w);
            return ms.ToArray();
        }

        private static void Send(int peer, Msg type, Action<BinaryWriter> body) => Transport?.Send(peer, Pack(type, body));

        // Host tells everyone who is in the session, so lobbies can list players before any game runs.
        private static void BroadcastLobby() => Transport.Broadcast(Pack(Msg.Lobby, w =>
        {
            w.Write(Peers.Count);
            foreach (var kv in Peers) { w.Write(kv.Key); w.Write(kv.Value.Name); }
        }));

        private static void RelayExcept(int skip, byte[] data)
        {
            foreach (int p in Transport.Peers)
                if (p != skip) Transport.Send(p, data);
        }

        public static void SendCursor(Vector2 world)
        {
            var data = Pack(Msg.Cursor, w => { w.Write(LocalPeer); w.Write(world.x); w.Write(world.y); });
            if (IsHost) Transport.Broadcast(data);
            else Transport.Send(0, data);
        }
    }
}
