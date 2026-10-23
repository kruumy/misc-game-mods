using System;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;
using BepInEx.Configuration;
using HarmonyLib;
using MMMultiplayer.Net;
using Motorways.Views;
using TMPro;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.UI;

namespace MMMultiplayer
{
    // The player-facing UI: a "Multiplayer" button on the main menu that opens a host/join flow and a lobby.
    // Built from clones of the game's own menu button and font so it looks native.
    public class MultiplayerMenu : MonoBehaviour
    {
        private enum Page { Root, Join, Lobby }

        private static MultiplayerMenu _instance;
        private static readonly AccessTools.FieldRef<MainMenuScreen, GameObject> OptionsButton =
            AccessTools.FieldRefAccess<MainMenuScreen, GameObject>("_optionsButton");
        private static readonly AccessTools.FieldRef<TouchButton, TouchButton.ButtonClickedEvent> OnClick =
            AccessTools.FieldRefAccess<TouchButton, TouchButton.ButtonClickedEvent>("_onClick");

        private ConfigEntry<string> _name, _address;
        private ConfigEntry<int> _port;

        private MainMenuScreen _mainMenu;
        private GameObject _buttonTemplate;
        private TMP_FontAsset _font;
        private Color _textColor = Color.white;
        private Canvas _canvas;
        private RectTransform _column;
        private TMP_Text _info;

        private bool _open;
        private bool _overScreen; // showing on top of a non-menu screen after a shared city ended
        private int _settledFrames;
        private Page _page;
        private string _message = "";
        private Task<TcpTransport> _connecting;

        public static void Create(ConfigEntry<string> name, ConfigEntry<string> address, ConfigEntry<int> port)
        {
            var go = new GameObject("MMMultiplayerMenu");
            DontDestroyOnLoad(go);
            go.hideFlags = HideFlags.HideAndDontSave;
            _instance = go.AddComponent<MultiplayerMenu>();
            _instance._name = name;
            _instance._address = address;
            _instance._port = port;
            Session.LocalName = name.Value;
        }

        // Adds our button to the main menu, right above Options.
        public static void AttachTo(MainMenuScreen menu)
        {
            var self = _instance;
            var options = OptionsButton(menu);
            if (self == null || options == null || options.transform.parent.Find("MP_Multiplayer") != null) return;
            self._mainMenu = menu;
            if (self._buttonTemplate == null) self.CaptureStyle(options);
            var button = self.MakeButton(options.transform.parent, "Multiplayer", () => self.Open(Session.Active ? Page.Lobby : Page.Root));
            button.name = "MP_Multiplayer";
            button.transform.SetSiblingIndex(options.transform.GetSiblingIndex());
        }

        private void CaptureStyle(GameObject options)
        {
            _buttonTemplate = Instantiate(options, transform, false);
            _buttonTemplate.SetActive(false);
            var label = options.GetComponentInChildren<TMP_Text>(true);
            if (label != null)
            {
                _font = label.font;
                _textColor = label.color;
            }
            BuildCanvas();
        }

        private void BuildCanvas()
        {
            var go = new GameObject("MP_Canvas", typeof(RectTransform), typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            go.transform.SetParent(transform, false);
            _canvas = go.GetComponent<Canvas>();
            _canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            _canvas.sortingOrder = 500;
            var scaler = go.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(2560, 1440); // matches the main menu's own button size
            scaler.matchWidthOrHeight = 1f;

            var backdrop = new GameObject("Backdrop", typeof(RectTransform), typeof(Image));
            backdrop.transform.SetParent(go.transform, false);
            Stretch(backdrop.GetComponent<RectTransform>());
            backdrop.GetComponent<Image>().color = new Color(0.13f, 0.14f, 0.16f, 0.97f);

            var column = new GameObject("Column", typeof(RectTransform), typeof(VerticalLayoutGroup), typeof(ContentSizeFitter));
            column.transform.SetParent(go.transform, false);
            _column = column.GetComponent<RectTransform>();
            _column.anchorMin = _column.anchorMax = new Vector2(0.15f, 0.5f);
            _column.pivot = new Vector2(0f, 0.5f);
            var layout = column.GetComponent<VerticalLayoutGroup>();
            layout.childAlignment = TextAnchor.MiddleLeft;
            layout.spacing = 30; // room for the menu buttons' hover grow
            layout.childControlWidth = layout.childControlHeight = false;
            layout.childForceExpandWidth = layout.childForceExpandHeight = false;
            var fitter = column.GetComponent<ContentSizeFitter>();
            fitter.horizontalFit = fitter.verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            _canvas.enabled = false;
        }

        private void Open(Page page)
        {
            _open = true;
            _message = "";
            Show(page);
        }

        private void Show(Page page)
        {
            _page = page;
            foreach (Transform child in _column) Destroy(child.gameObject);
            _info = null;
            switch (page)
            {
                case Page.Root:
                    MakeText("Multiplayer", 110);
                    MakeField("Your name", _name.Value, v => Session.LocalName = _name.Value = v);
                    _info = MakeText(_message, 30);
                    MakeButton(_column, "Host co-op", Host);
                    MakeButton(_column, "Join", () => Show(Page.Join));
                    MakeButton(_column, "Back", () => _open = false);
                    break;
                case Page.Join:
                    MakeText("Join a game", 110);
                    MakeField("Host address", _address.Value, v => _address.Value = v);
                    _info = MakeText(_message, 30);
                    MakeButton(_column, "Connect", Join);
                    MakeButton(_column, "Back", () => Show(Page.Root));
                    break;
                case Page.Lobby:
                    MakeText("Lobby", 110);
                    _info = MakeText("", 34);
                    if (Session.IsHost) MakeButton(_column, "Choose city", ChooseCity);
                    MakeButton(_column, "Leave", () => { Session.Leave(); _message = ""; Show(Page.Root); });
                    break;
            }
        }

        private void ChooseCity()
        {
            var stack = FindObjectOfType<AppRuntime>()?.App?.Scope.Get<ScreenStack>();
            _overScreen = false;
            if (stack == null || stack.GetTopActiveScreenType() == ScreenStack.MotorwaysScreen.MapSelect) return; // already underneath
            stack.PushScreen(ScreenStack.MotorwaysScreen.MapSelect, (MapSelectScreen s) => s.PrepareScreen());
        }

        private void Host()
        {
            try
            {
                Session.Host(TcpTransport.Host(_port.Value), MpMode.Coop);
                Show(Page.Lobby);
            }
            catch (Exception e)
            {
                SetMessage("Couldn't host: " + e.Message);
            }
        }

        private void Join()
        {
            if (_connecting != null) return;
            string address = _address.Value.Trim();
            int port = _port.Value;
            int colon = address.LastIndexOf(':');
            if (colon > 0 && int.TryParse(address.Substring(colon + 1), out int parsed))
            {
                port = parsed;
                address = address.Substring(0, colon);
            }
            SetMessage("Connecting...");
            _connecting = Task.Run(() => TcpTransport.Connect(address, port));
        }

        private void SetMessage(string text)
        {
            _message = text;
            if (_info != null) _info.text = text;
        }

        private void Update()
        {
            if (_canvas == null) return;
            if (_connecting != null && _connecting.IsCompleted)
            {
                var task = _connecting;
                _connecting = null;
                if (task.IsFaulted) SetMessage("Couldn't connect: " + (task.Exception?.InnerException?.Message ?? "unknown error"));
                else
                {
                    Session.Join(task.Result);
                    Show(Page.Lobby);
                }
            }

            var stack = FindObjectOfType<AppRuntime>()?.App?.Scope.Get<ScreenStack>();
            bool onMenu = stack != null && stack.GetTopActiveScreenType() == ScreenStack.MotorwaysScreen.MainMenu;
            // The shared city ended: once out of the game, show the lobby over whatever screen the game landed on.
            bool settled = stack != null && !stack.IsInGame(); // the transition flag can stay stuck after an exit, so just wait out the flicker
            _settledFrames = settled ? _settledFrames + 1 : 0;
            if (Session.ReturnToLobby && _settledFrames > 60)
            {
                Session.ReturnToLobby = false;
                _overScreen = true;
                Open(Session.Active ? Page.Lobby : Page.Root);
                if (!Session.Active) SetMessage(Session.Status);
            }
            if (stack != null && stack.IsInGame()) _overScreen = false;
            // Back on the menu after a game while still connected: return straight to the lobby.
            if (onMenu && Session.Active && (!_open || _page != Page.Lobby)) Open(Page.Lobby);
            if (_open && _page == Page.Lobby && !Session.Active)
            {
                _message = Session.Status;
                Show(Page.Root);
                SetMessage(_message);
            }
            // A connected client has nowhere to go but the lobby until the host starts a city.
            bool clientWaiting = Session.Active && !Session.IsHost && stack != null && !stack.IsInGame();
            if (clientWaiting && (!_open || _page != Page.Lobby)) Open(Page.Lobby);
            _canvas.enabled = _open && (onMenu || _overScreen || clientWaiting);
            if (_canvas.enabled && _page == Page.Lobby && _info != null) _info.text = LobbyText();
        }

        private static string LobbyText()
        {
            var text = new System.Text.StringBuilder();
            if (Session.Notice.Length > 0) text.AppendLine($"<color=#E8A33D>{Session.Notice}</color>").AppendLine();
            if (Session.IsHost)
            {
                text.AppendLine("You're hosting co-op. Friends can join at:");
                foreach (var ip in LocalAddresses()) text.AppendLine($"    {ip}:{_instance._port.Value}");
            }
            else text.AppendLine("Connected. Waiting for the host to choose a city...");
            text.AppendLine();
            text.AppendLine("Players");
            foreach (var kv in Session.Peers.OrderBy(p => p.Key))
            {
                string color = ColorUtility.ToHtmlStringRGB(RemoteCursors.ColorOf(kv.Key));
                string role = kv.Key == 0 ? " (host)" : "";
                string you = kv.Key == Session.LocalPeer ? " - you" : "";
                text.AppendLine($"    <color=#{color}>{kv.Value.Name}</color>{role}{you}");
            }
            return text.ToString();
        }

        private static string[] _addresses;

        private static string[] LocalAddresses()
        {
            if (_addresses != null) return _addresses;
            try
            {
                _addresses = Dns.GetHostEntry(Dns.GetHostName()).AddressList
                    .Where(a => a.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(a))
                    .Select(a => a.ToString()).ToArray();
            }
            catch { _addresses = new string[0]; }
            return _addresses;
        }

        // ---- widgets ----

        private GameObject MakeButton(Transform parent, string label, UnityAction onClick)
        {
            var go = Instantiate(_buttonTemplate, parent, false);
            go.name = "MP_" + label;
            foreach (var localized in go.GetComponentsInChildren<LocalizedTextUI>(true)) DestroyImmediate(localized);
            var text = go.GetComponentInChildren<TMP_Text>(true);
            if (text != null) text.text = label;
            var button = go.GetComponentInChildren<TouchButton>(true);
            if (button != null)
            {
                OnClick(button) = new TouchButton.ButtonClickedEvent(); // drop the cloned Options handler
                button.AddOnClickedEvent(onClick);
                // Screens normally do this; without it hover-exit throws and there is no hover sound.
                var appScope = FindObjectOfType<AppRuntime>()?.App?.Scope;
                if (appScope != null) button.Initialize(appScope);
            }
            go.SetActive(true);
            return go;
        }

        private TMP_Text MakeText(string content, float size)
        {
            var go = new GameObject("Text", typeof(RectTransform), typeof(TextMeshProUGUI));
            go.transform.SetParent(_column, false);
            var text = go.GetComponent<TextMeshProUGUI>();
            if (_font != null) text.font = _font;
            text.fontSize = size;
            text.color = _textColor;
            text.richText = true;
            text.enableWordWrapping = false;
            text.text = content;
            go.GetComponent<RectTransform>().sizeDelta = new Vector2(1100, size * 1.3f);
            go.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            return text;
        }

        private void MakeField(string label, string value, Action<string> onChanged)
        {
            MakeText(label, 30).color = new Color(_textColor.r, _textColor.g, _textColor.b, 0.6f);
            var go = new GameObject("Field", typeof(RectTransform), typeof(Image));
            go.SetActive(false);
            go.transform.SetParent(_column, false);
            go.GetComponent<RectTransform>().sizeDelta = new Vector2(560, 70);
            go.GetComponent<Image>().color = new Color(1f, 1f, 1f, 0.1f);

            var area = new GameObject("Text Area", typeof(RectTransform));
            area.transform.SetParent(go.transform, false);
            var areaRect = area.GetComponent<RectTransform>();
            Stretch(areaRect);
            areaRect.offsetMin = new Vector2(20, 8);
            areaRect.offsetMax = new Vector2(-20, -8);

            var textGo = new GameObject("Text", typeof(RectTransform), typeof(TextMeshProUGUI));
            textGo.transform.SetParent(area.transform, false);
            Stretch(textGo.GetComponent<RectTransform>());
            var text = textGo.GetComponent<TextMeshProUGUI>();
            if (_font != null) text.font = _font;
            text.fontSize = 38;
            text.color = _textColor;
            text.enableWordWrapping = false;
            text.alignment = TextAlignmentOptions.MidlineLeft;

            var field = go.AddComponent<TMP_InputField>();
            field.textViewport = areaRect;
            field.textComponent = text;
            field.characterLimit = 20;
            field.caretColor = _textColor;
            field.customCaretColor = true;
            field.text = value;
            field.onValueChanged.AddListener(v => onChanged(v));
            go.SetActive(true);
        }

        private static void Stretch(RectTransform rect)
        {
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = rect.offsetMax = Vector2.zero;
        }
    }

    [HarmonyPatch(typeof(MainMenuScreen), nameof(MainMenuScreen.OnCreatedInScope))]
    internal static class MainMenuButtonPatch
    {
        private static void Postfix(MainMenuScreen __instance) => MultiplayerMenu.AttachTo(__instance);
    }
}
