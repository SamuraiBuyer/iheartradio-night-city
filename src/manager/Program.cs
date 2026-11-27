// iHeart Radio Station Manager - window: a WebView2 (Edge) showing ui\index.html, talking to Backend through messages.
//
// UI -> app:  chrome.webview.postMessage({ id, method, params })
// app -> UI:  { id, ok, result | error }
//
// Command line (testing): --game <dir>   use this game folder
// Built with DEVTOOLS defined (build.bat dev) only:
//                         --screenshot <file.png> [--script <js>] [--wait <ms>]   render, save a picture, quit
//                         --record <dir> --script <js>   run the script, save frames (f00000.png + times.txt) until it
//                                                        sets window.__demoDone, quit (for the demo GIF)
//                         --script @file.js              read the script from a file
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace IHeartRadioManager
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm(args));
        }
    }

    class MainForm : Form
    {
        readonly WebView2 web = new WebView2 { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.FromArgb(10, 8, 10) };
        readonly Backend backend;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
#if DEVTOOLS
        readonly string screenshot, script, record;
        readonly int wait = 2500;
#endif

        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        public MainForm(string[] args)
        {
            string game = null;
            for (int i = 0; i + 1 < args.Length; i++)
            {
                if (args[i] == "--game") game = args[i + 1];
#if DEVTOOLS
                if (args[i] == "--screenshot") screenshot = args[i + 1];
                if (args[i] == "--script") script = args[i + 1];
                if (args[i] == "--wait") int.TryParse(args[i + 1], out wait);
                if (args[i] == "--record") record = args[i + 1];
#endif
            }
#if DEVTOOLS
            if (script != null && script.StartsWith("@")) script = File.ReadAllText(script.Substring(1));
#endif
            backend = new Backend(game);
            Text = "iHeart Radio Station Manager";
            BackColor = Color.FromArgb(10, 8, 10);
            ClientSize = new Size(1360, 860);
            MinimumSize = new Size(1100, 720);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            Controls.Add(web);
            Load += async (s, e) => await Init();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            int on = 1;   // dark title bar
            try { DwmSetWindowAttribute(Handle, 20, ref on, 4); } catch { }
        }

        async Task Init()
        {
            try
            {
                var data = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "iHeartRadioManager", "WebView2");
                var env = await CoreWebView2Environment.CreateAsync(null, data);
                await web.EnsureCoreWebView2Async(env);
            }
            catch (WebView2RuntimeNotFoundException)
            {
                if (MessageBox.Show(this, "This app needs the Microsoft Edge WebView2 Runtime (part of Windows 11, free for Windows 10).\n\nOpen the download page?",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
                    System.Diagnostics.Process.Start("https://developer.microsoft.com/microsoft-edge/webview2/");
                Close();
                return;
            }
            var core = web.CoreWebView2;
            core.Settings.AreDevToolsEnabled = false;
            core.Settings.AreDefaultContextMenusEnabled = false;
            core.Settings.IsStatusBarEnabled = false;
            core.Settings.IsZoomControlEnabled = false;
            var ui = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ui");
            core.SetVirtualHostNameToFolderMapping("app.iheartradio.local", ui, CoreWebView2HostResourceAccessKind.Allow);
            core.WebMessageReceived += OnMessage;
            core.NewWindowRequested += (s, e) => { e.Handled = true; backend.Open(e.Uri); };
#if DEVTOOLS
            if (record != null) core.NavigationCompleted += async (s, e) => await Record();
            else if (screenshot != null) core.NavigationCompleted += async (s, e) => await Shoot();
#endif
            core.Navigate("https://app.iheartradio.local/index.html");
        }

#if DEVTOOLS
        async Task Shoot()
        {
            if (script != null) { await Task.Delay(800); await web.CoreWebView2.ExecuteScriptAsync(script); }
            await Task.Delay(wait);
            using (var fs = File.Create(screenshot))
                await web.CoreWebView2.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, fs);
            Close();
        }

        // demo recording: frames as fast as WebView2 renders them, with their times, until the script says it's done
        async Task Record()
        {
            Directory.CreateDirectory(record);
            await Task.Delay(1500);
            if (script != null) await web.CoreWebView2.ExecuteScriptAsync(script);
            var times = new List<string>();
            var clock = System.Diagnostics.Stopwatch.StartNew();
            for (int n = 0; clock.Elapsed.TotalSeconds < 120; n++)
            {
                long t = clock.ElapsedMilliseconds;
                using (var fs = File.Create(Path.Combine(record, "f" + n.ToString("00000") + ".png")))
                    await web.CoreWebView2.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, fs);
                times.Add(t.ToString());
                if (await web.CoreWebView2.ExecuteScriptAsync("window.__demoDone === true") == "true") break;
                long pause = 66 - (clock.ElapsedMilliseconds - t);
                if (pause > 0) await Task.Delay((int)pause);
            }
            File.WriteAllLines(Path.Combine(record, "times.txt"), times);
            Close();
        }
#endif

        async void OnMessage(object sender, CoreWebView2WebMessageReceivedEventArgs e)
        {
            object id = null, result = null;
            string error = null;
            try
            {
                var msg = json.Deserialize<Dictionary<string, object>>(e.WebMessageAsJson);
                id = msg["id"];
                var method = (string)msg["method"];
                var p = msg.ContainsKey("params") ? msg["params"] as Dictionary<string, object> ?? new Dictionary<string, object>() : new Dictionary<string, object>();
                if (method == "browseGame") result = BrowseGame();      // dialogs need the UI thread
                else result = await Task.Run(() => Call(method, p));
            }
            catch (Exception ex)
            {
                while (ex.InnerException != null) ex = ex.InnerException;
                error = ex is WebException ? "Can't reach iHeartRadio (" + ex.Message + ")" : ex.Message;
            }
            try
            {
                web.CoreWebView2.PostWebMessageAsJson(json.Serialize(new Dictionary<string, object>
                    { { "id", id }, { "ok", error == null }, { "result", result }, { "error", error } }));
            }
            catch { }
        }

        object Call(string method, Dictionary<string, object> p)
        {
            Func<string, string> s = k => p.ContainsKey(k) && p[k] != null ? Convert.ToString(p[k], System.Globalization.CultureInfo.InvariantCulture) : "";
            switch (method)
            {
                case "status": return backend.Status();
                case "search": return backend.Search(s("q"));
                case "station": return backend.Station(s("id"));
                case "logo": return backend.Logo(s("url"));
                case "list": return backend.List();
                case "install": return backend.Install(p);
                case "remove": return backend.Remove(s("id"));
                case "removeAll": return backend.RemoveAll();
                case "applyLogos": return backend.ApplyLogos();
                case "open": return backend.Open(s("what"));
                default: throw new Exception("unknown method " + method);
            }
        }

        object BrowseGame()
        {
            using (var dlg = new FolderBrowserDialog { Description = "Select your Cyberpunk 2077 folder (the one with bin, archive, r6...)", ShowNewFolderButton = false })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return backend.Status();
                return backend.SetGameDir(dlg.SelectedPath);
            }
        }
    }
}
