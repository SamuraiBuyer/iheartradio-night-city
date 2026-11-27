// iHeart Radio Station Manager - everything the UI asks for (called through Program.cs's message bridge).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace IHeartRadioManager
{
    public class Backend
    {
        public const string Version = "2.1.0";
        const int BridgePort = 8925;
        static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };

        public string GameDir { get; private set; }
        readonly string settingsFile;

        public Backend(string gameDirOverride)
        {
            settingsFile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "iHeartRadioManager", "settings.json");
            GameDir = gameDirOverride ?? FindGameDir();
        }

        // ---------------------------------------------------------------- paths
        string ModsDir { get { return Path.Combine(GameDir, @"bin\x64\plugins\cyber_engine_tweaks\mods"); } }
        string RadiosDir { get { return Path.Combine(ModsDir, @"radioExt\radios"); } }
        string DataDir { get { return Path.Combine(ModsDir, @"iHeartRadio\data"); } }
        string LogosDir { get { return Path.Combine(DataDir, "logos"); } }
        string PendingFlag { get { return Path.Combine(DataDir, "logos_pending.txt"); } }
        // which logo-file format the archive was last written with; older ones are rebuilt (2 = Kraken, 3 = uncompressed)
        const string LogoFormat = "3";
        string FormatStamp { get { return Path.Combine(DataDir, "logos_format.txt"); } }
        bool LogosOutdated()
        {
            if (!Directory.Exists(LogosDir) || Directory.GetFiles(LogosDir, "*.png").Length == 0) return false;
            try { return !File.Exists(FormatStamp) || File.ReadAllText(FormatStamp).Trim() != LogoFormat; } catch { return true; }
        }
        string ArchivePath { get { return Path.Combine(GameDir, @"archive\pc\mod\iHeartRadio_Stations.archive"); } }

        static bool IsGameDir(string d) { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, @"bin\x64\Cyberpunk2077.exe")); }

        string FindGameDir()
        {
            // 1. installed inside the game (bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio\StationManager)
            for (var d = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory); d != null; d = d.Parent)
                if (IsGameDir(d.FullName)) return d.FullName;
            // 2. chosen before
            try
            {
                var s = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(settingsFile));
                if (s.ContainsKey("gameDir") && IsGameDir(s["gameDir"] as string)) return (string)s["gameDir"];
            }
            catch { }
            // 3. Steam libraries, GOG, Epic
            var cands = new List<string>();
            try
            {
                var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
                if (steam != null)
                {
                    cands.Add(Path.Combine(steam, @"steamapps\common\Cyberpunk 2077"));
                    foreach (var vdf in new[] { @"config\libraryfolders.vdf", @"steamapps\libraryfolders.vdf" })
                    {
                        var f = Path.Combine(steam, vdf);
                        if (!File.Exists(f)) continue;
                        foreach (Match m in Regex.Matches(File.ReadAllText(f), "\"path\"\\s+\"([^\"]+)\""))
                            cands.Add(Path.Combine(m.Groups[1].Value.Replace(@"\\", @"\"), @"steamapps\common\Cyberpunk 2077"));
                    }
                }
            }
            catch { }
            foreach (var k in new[] { @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\GOG.com\Games\1423049311", @"HKEY_LOCAL_MACHINE\SOFTWARE\GOG.com\Games\1423049311" })
            {
                try { var p = Registry.GetValue(k, "path", null) as string; if (p != null) cands.Add(p); } catch { }
            }
            cands.Add(@"C:\Program Files\Epic Games\Cyberpunk2077");
            foreach (var c in cands) { try { if (IsGameDir(c)) return c; } catch { } }
            return null;
        }

        public object SetGameDir(string dir)
        {
            if (!IsGameDir(dir)) throw new Exception("That folder doesn't contain bin\\x64\\Cyberpunk2077.exe");
            GameDir = dir;
            Directory.CreateDirectory(Path.GetDirectoryName(settingsFile));
            File.WriteAllText(settingsFile, Json.Serialize(new Dictionary<string, object> { { "gameDir", dir } }));
            return Status();
        }

        // ---------------------------------------------------------------- status
        public object Status()
        {
            bool found = IsGameDir(GameDir);
            var st = new Dictionary<string, object>
            {
                { "version", Version },
                { "gameDir", GameDir },
                { "gameFound", found },
                { "gameRunning", Process.GetProcessesByName("Cyberpunk2077").Length > 0 },
            };
            if (found)
            {
                st["cet"] = File.Exists(Path.Combine(GameDir, @"bin\x64\plugins\cyber_engine_tweaks.asi")) || Directory.Exists(Path.Combine(GameDir, @"bin\x64\plugins\cyber_engine_tweaks"));
                st["red4ext"] = File.Exists(Path.Combine(GameDir, @"red4ext\RED4ext.dll"));
                st["radioExt"] = File.Exists(Path.Combine(ModsDir, @"radioExt\init.lua")) && File.Exists(Path.Combine(GameDir, @"red4ext\plugins\RadioExt\RadioExt.dll"));
                st["plugin"] = File.Exists(Path.Combine(GameDir, @"red4ext\plugins\iHeartRadio\iHeartRadio.dll"));
                st["pendingLogos"] = File.Exists(PendingFlag) || LogosOutdated();
            }
            return st;
        }

        // ---------------------------------------------------------------- iHeart
        static string Http(string url)
        {
            using (var wc = new WebClient())
            {
                wc.Encoding = Encoding.UTF8;
                wc.Headers[HttpRequestHeader.UserAgent] = "Mozilla/5.0";
                return wc.DownloadString(url);
            }
        }

        static string S(Dictionary<string, object> d, string k) { object v; return d != null && d.TryGetValue(k, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : ""; }
        static string BaseImage(string url) { int q = url.IndexOf('?'); return q >= 0 ? url.Substring(0, q) : url; }

        public object Search(string q)
        {
            if (string.IsNullOrWhiteSpace(q)) return new object[0];
            var url = "https://api.iheart.com/api/v3/search/all?keywords=" + Uri.EscapeDataString(q.Trim()) +
                      "&maxRows=24&station=true&artist=false&track=false&playlist=false&podcast=false&keyword=false&bundle=false";
            var r = Json.Deserialize<Dictionary<string, object>>(Http(url));
            var results = r.ContainsKey("results") ? r["results"] as Dictionary<string, object> : null;
            var stations = results != null && results.ContainsKey("stations") ? results["stations"] as object[] ?? (results["stations"] as System.Collections.ArrayList)?.ToArray() : null;
            var installed = new HashSet<string>(InstalledFolders().Select(f => f.Key));
            var list = new List<object>();
            foreach (Dictionary<string, object> s in stations ?? new object[0])
            {
                var img = S(s, "imageUrl");
                list.Add(new Dictionary<string, object>
                {
                    { "id", S(s, "id") }, { "name", S(s, "name") }, { "callLetters", S(s, "callLetters") },
                    { "frequency", S(s, "frequency") }, { "genre", S(s, "genre") }, { "description", S(s, "description") },
                    { "thumb", img.Length > 0 ? BaseImage(img) + "?ops=fit(160,160)" : "" },
                    { "installed", installed.Contains(S(s, "id")) },
                });
            }
            return list;
        }

        Dictionary<string, object> LiveStation(string id)
        {
            var r = Json.Deserialize<Dictionary<string, object>>(Http("https://api.iheart.com/api/v2/content/liveStations/" + Uri.EscapeDataString(id)));
            var hits = r.ContainsKey("hits") ? r["hits"] as System.Collections.ArrayList : null;
            if (hits == null || hits.Count == 0) throw new Exception("iHeart has no station with id " + id);
            return (Dictionary<string, object>)hits[0];
        }

        public object Station(string id)
        {
            var h = LiveStation(id);
            var streams = h.ContainsKey("streams") ? h["streams"] as Dictionary<string, object> : null;
            var markets = h.ContainsKey("markets") ? h["markets"] as System.Collections.ArrayList : null;
            var market = markets != null && markets.Count > 0 ? markets[0] as Dictionary<string, object> : null;
            string freq = S(h, "freq"), name = S(h, "name"), call = Regex.Replace(S(h, "callLetters"), "-(FM|AM)$", "");
            double fm; double.TryParse(Regex.Replace(freq, "[^0-9.]", ""), NumberStyles.Float, CultureInfo.InvariantCulture, out fm);
            var logo = S(h, "logo");
            return new Dictionary<string, object>
            {
                { "id", S(h, "id") }, { "name", name }, { "callLetters", call }, { "freq", freq }, { "band", S(h, "band") },
                { "description", S(h, "description") },
                { "city", market != null ? S(market, "city") + (S(market, "stateAbbreviation").Length > 0 ? ", " + S(market, "stateAbbreviation") : "") : "" },
                { "displayName", GameName(name, freq, call) }, { "fm", fm },
                { "hasStream", streams != null && streams.Count > 0 },
                { "logo", logo.Length > 0 ? LogoDataUrl(BaseImage(logo) + "?ops=fit(1024,1024),format(%22png%22)") : "" },
            };
        }

        // the game's layout: frequency first, like "89.3 Radio Vexelstrom" ("Power 105.1" -> "105.1 Power")
        static string GameName(string name, string freq, string call)
        {
            if (freq.Length == 0) return name;
            // (?<![\w.]): "Q104.3" stays whole instead of becoming "104.3 Q"
            var rest = Regex.Replace(name, @"(?<![\w.])" + Regex.Escape(freq) + @"(?![\d.])", "");
            rest = Regex.Replace(rest, @"\s+", " ").Trim(' ', '-', '/');
            return freq + " " + (rest.Length > 0 ? rest : call);
        }

        public object Logo(string url) { return LogoDataUrl(url); }

        static string LogoDataUrl(string url)
        {
            using (var wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.UserAgent] = "Mozilla/5.0";
                var bytes = wc.DownloadData(url);
                var type = wc.ResponseHeaders[HttpResponseHeader.ContentType] ?? "image/png";
                return "data:" + type + ";base64," + Convert.ToBase64String(bytes);
            }
        }

        // ---------------------------------------------------------------- installed stations
        // RadioExt station folders made by this mod: radioExt\radios\iheart_<id>_<name>
        IEnumerable<KeyValuePair<string, string>> InstalledFolders()
        {
            if (GameDir == null || !Directory.Exists(RadiosDir)) yield break;
            foreach (var d in Directory.GetDirectories(RadiosDir, "iheart_*"))
            {
                var m = Regex.Match(Path.GetFileName(d), @"^iheart_(\d+)");
                if (m.Success && File.Exists(Path.Combine(d, "metadata.json"))) yield return new KeyValuePair<string, string>(m.Groups[1].Value, d);
            }
        }

        public object List()
        {
            var list = new List<object>();
            foreach (var f in InstalledFolders())
            {
                Dictionary<string, object> meta;
                try { meta = Json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(f.Value, "metadata.json"))); }
                catch { continue; }
                var logo = Path.Combine(LogosDir, f.Key + ".png");
                var settings = Path.Combine(LogosDir, f.Key + ".json");
                double fm = 0; try { fm = Convert.ToDouble(meta["fm"], CultureInfo.InvariantCulture); } catch { }
                double vol = 1; try { vol = Convert.ToDouble(meta["volume"], CultureInfo.InvariantCulture); } catch { }
                list.Add(new Dictionary<string, object>
                {
                    { "id", f.Key }, { "displayName", S(meta, "displayName") }, { "fm", fm }, { "volume", vol },
                    { "logo", File.Exists(logo) ? "data:image/png;base64," + Convert.ToBase64String(File.ReadAllBytes(logo)) : "" },
                    { "logoSettings", File.Exists(settings) ? File.ReadAllText(settings) : "" },
                });
            }
            return list.OrderBy(x => (double)((Dictionary<string, object>)x)["fm"]).ToList();
        }

        public object Install(Dictionary<string, object> p)
        {
            RequireGame();
            string id = S(p, "id");
            if (!Regex.IsMatch(id, @"^\d{1,8}$")) throw new Exception("bad station id");
            string name = S(p, "displayName").Trim();
            if (name.Length == 0) throw new Exception("The station needs a name");
            double fm; double.TryParse(S(p, "fm"), NumberStyles.Float, CultureInfo.InvariantCulture, out fm);
            double vol; if (!double.TryParse(S(p, "volume"), NumberStyles.Float, CultureInfo.InvariantCulture, out vol)) vol = 1;
            vol = Math.Max(0, Math.Min(2, vol));
            string icon = S(p, "icon");                       // data:image/png;base64,... (512x512) or "" = keep / none
            bool removeIcon = S(p, "removeIcon") == "True";

            Directory.CreateDirectory(LogosDir);
            string logoFile = Path.Combine(LogosDir, id + ".png");
            if (icon.StartsWith("data:image/png;base64,"))
            {
                File.WriteAllBytes(logoFile, Convert.FromBase64String(icon.Substring("data:image/png;base64,".Length)));
                var settings = S(p, "logoSettings");
                if (settings.Length > 0) File.WriteAllText(Path.Combine(LogosDir, id + ".json"), settings);
            }
            else if (removeIcon && File.Exists(logoFile)) File.Delete(logoFile);

            var existing = InstalledFolders().FirstOrDefault(f => f.Key == id).Value;
            string folder = existing;
            if (folder == null)
            {
                var slug = Regex.Replace(S(p, "callLetters").Length > 0 ? S(p, "callLetters") : name, "[^A-Za-z0-9]+", "_").Trim('_').ToLowerInvariant();
                folder = Path.Combine(RadiosDir, "iheart_" + id + "_" + (slug.Length > 0 ? slug : "station"));
            }
            Directory.CreateDirectory(folder);
            bool hasLogo = File.Exists(logoFile);
            WriteMetadata(Path.Combine(folder, "metadata.json"), name, fm, vol, hasLogo ? long.Parse(id) : -1,
                          "http://127.0.0.1:" + BridgePort + "/zc" + id + ".ogg");   // the plugin sends Ogg Vorbis
            return ApplyLogos();
        }

        public object Remove(string id)
        {
            RequireGame();
            foreach (var f in InstalledFolders().Where(f => f.Key == id).ToList()) Directory.Delete(f.Value, true);
            foreach (var ext in new[] { ".png", ".json" })
            {
                var f = Path.Combine(LogosDir, id + ext);
                if (File.Exists(f)) File.Delete(f);
            }
            return ApplyLogos();
        }

        /// <summary>Every station this manager added, their logos and the logo archive (before uninstalling the mod).</summary>
        public object RemoveAll()
        {
            RequireGame();
            foreach (var f in InstalledFolders().ToList()) Directory.Delete(f.Value, true);
            if (Directory.Exists(LogosDir)) Directory.Delete(LogosDir, true);
            return ApplyLogos();
        }

        /// <summary>Rebuilds iHeartRadio_Stations.archive from the saved logos. Can't while the game runs (it keeps the
        /// file open): then it's flagged and done later.</summary>
        public object ApplyLogos()
        {
            RequireGame();
            var installed = InstalledFolders().Select(f => f.Key).ToList();
            var icons = installed.Where(id => File.Exists(Path.Combine(LogosDir, id + ".png")))
                                 .Select(id => new KeyValuePair<long, string>(long.Parse(id), Path.Combine(LogosDir, id + ".png"))).ToList();
            var result = new Dictionary<string, object> { { "stations", installed.Count }, { "logos", icons.Count } };
            try
            {
                IconArchive.Write(ArchivePath, GameDir, icons);
                if (File.Exists(PendingFlag)) File.Delete(PendingFlag);
                Directory.CreateDirectory(DataDir);
                File.WriteAllText(FormatStamp, LogoFormat);
                result["logosApplied"] = true;
            }
            catch (IOException)
            {
                Directory.CreateDirectory(DataDir);
                File.WriteAllText(PendingFlag, DateTime.Now.ToString("s"));
                result["logosApplied"] = false;
            }
            return result;
        }

        void RequireGame() { if (!IsGameDir(GameDir)) throw new Exception("Cyberpunk 2077 folder not set"); }

        static void WriteMetadata(string path, string name, double fm, double volume, long iconId, string url)
        {
            Func<string, string> js = s => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
            var inv = CultureInfo.InvariantCulture;
            var sb = new StringBuilder();
            sb.Append("{\n");
            sb.Append("    \"displayName\": ").Append(js(name)).Append(",\n");
            sb.Append("    \"fm\": ").Append(fm.ToString("0.0##", inv)).Append(",\n");
            sb.Append("    \"volume\": ").Append(volume.ToString("0.0##", inv)).Append(",\n");
            sb.Append("    \"icon\": \"default\",\n");
            sb.Append("    \"customIcon\": {\n");
            sb.Append("        \"inkAtlasPath\": ").Append(js(iconId >= 0 ? IconArchive.AtlasPath(iconId) : "")).Append(",\n");
            sb.Append("        \"inkAtlasPart\": ").Append(js(iconId >= 0 ? IconArchive.IconName(iconId) : "")).Append(",\n");
            sb.Append("        \"useCustom\": ").Append(iconId >= 0 ? "true" : "false").Append("\n");
            sb.Append("    },\n");
            sb.Append("    \"streamInfo\": {\n");
            sb.Append("        \"streamURL\": ").Append(js(url)).Append(",\n");
            sb.Append("        \"isStream\": true\n");
            sb.Append("    },\n");
            sb.Append("    \"order\": []\n");
            sb.Append("}\n");
            File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        }

        // ---------------------------------------------------------------- misc
        public object Open(string what)
        {
            string target = what == "game" ? GameDir
                          : what == "radios" ? RadiosDir
                          : what == "log" ? Path.Combine(GameDir ?? "", @"red4ext\plugins\iHeartRadio")
                          : what.StartsWith("https://") ? what : null;
            if (target == null) throw new Exception("nothing to open");
            Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
            return true;
        }
    }
}
