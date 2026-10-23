using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace MMMultiplayer.Net
{
    public interface ITransport
    {
        bool IsHost { get; }
        IEnumerable<int> Peers { get; }
        void Send(int peer, byte[] data);
        void Broadcast(byte[] data);
        // Drains received events on the main thread. data == null means the peer disconnected.
        void Poll(Action<int, byte[]> onMessage);
        void Close();
    }

    // Reliable, ordered, length-prefixed frames over TCP. Host is peer 0.
    // shortcut: direct IP only, used for local two-instance testing and LAN; Steam P2P is the user-facing transport.
    public class TcpTransport : ITransport
    {
        private readonly TcpListener _listener;
        private readonly Dictionary<int, NetworkStream> _streams = new Dictionary<int, NetworkStream>();
        private readonly ConcurrentQueue<KeyValuePair<int, byte[]>> _inbox = new ConcurrentQueue<KeyValuePair<int, byte[]>>();
        private int _nextPeer = 1;
        private volatile bool _closed;

        public bool IsHost { get; }

        public IEnumerable<int> Peers
        {
            get { lock (_streams) return new List<int>(_streams.Keys); }
        }

        public static TcpTransport Host(int port) => new TcpTransport(port);

        public static TcpTransport Connect(string address, int port)
        {
            var client = new TcpClient { NoDelay = true };
            try
            {
                if (!client.ConnectAsync(address, port).Wait(5000)) throw new TimeoutException("No answer from " + address);
            }
            catch (AggregateException e)
            {
                client.Close();
                throw e.InnerException ?? e;
            }
            catch
            {
                client.Close();
                throw;
            }
            var t = new TcpTransport(null);
            t.AddPeer(0, client);
            return t;
        }

        private TcpTransport(int port)
        {
            IsHost = true;
            _listener = new TcpListener(IPAddress.Any, port);
            _listener.Start();
            new Thread(AcceptLoop) { IsBackground = true }.Start();
        }

        private TcpTransport(object _) { IsHost = false; }

        private void AcceptLoop()
        {
            while (!_closed)
            {
                TcpClient client;
                try { client = _listener.AcceptTcpClient(); }
                catch { return; }
                client.NoDelay = true;
                AddPeer(Interlocked.Increment(ref _nextPeer) - 1, client);
            }
        }

        private void AddPeer(int peer, TcpClient client)
        {
            var stream = client.GetStream();
            lock (_streams) _streams[peer] = stream;
            new Thread(() => ReadLoop(peer, stream)) { IsBackground = true }.Start();
        }

        private void ReadLoop(int peer, NetworkStream stream)
        {
            var reader = new BinaryReader(stream);
            try
            {
                while (!_closed)
                {
                    int length = reader.ReadInt32();
                    _inbox.Enqueue(new KeyValuePair<int, byte[]>(peer, reader.ReadBytes(length)));
                }
            }
            catch (Exception) { }
            lock (_streams) _streams.Remove(peer);
            _inbox.Enqueue(new KeyValuePair<int, byte[]>(peer, null));
        }

        public void Send(int peer, byte[] data)
        {
            NetworkStream stream;
            lock (_streams)
                if (!_streams.TryGetValue(peer, out stream)) return;
            try
            {
                lock (stream)
                {
                    stream.Write(BitConverter.GetBytes(data.Length), 0, 4);
                    stream.Write(data, 0, data.Length);
                }
            }
            catch (Exception) { stream.Close(); }
        }

        public void Broadcast(byte[] data)
        {
            foreach (int peer in Peers) Send(peer, data);
        }

        public void Poll(Action<int, byte[]> onMessage)
        {
            while (_inbox.TryDequeue(out var msg)) onMessage(msg.Key, msg.Value);
        }

        public void Close()
        {
            _closed = true;
            _listener?.Stop();
            lock (_streams)
            {
                foreach (var s in _streams.Values) s.Close();
                _streams.Clear();
            }
        }
    }
}
