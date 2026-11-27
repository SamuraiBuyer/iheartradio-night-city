// iHeart Radio Station Manager - builds the station logo .archive without WolvenKit.
//
// Each station logo becomes two game files, made from templates built once with WolvenKit (see src/manager/templates):
//   iheartradio\icons\iheart_<8-digit id>.xbm       512x512 BC7 texture (CBitmapTexture, same setup as the game's
//                                                    radio station icons)
//   iheartradio\icons\iheart_<8-digit id>.inkatlas  atlas with one full-size part named iheart_<8-digit id>
// The station name only changes 8 digits inside the templates, so every offset stays the same: only the checksums
// are recomputed. Data is stored uncompressed (segment size == stored size), which the game reads as is, like the
// audio files in its own archives. (No Kraken: that would need the game's Oodle library.)
//
// Format notes (checked against WolvenKit-built files):
//   archive (RDAR v12): file entries sorted by FNV-1a 64 of the depot path; index checksum CRC-64/XZ over the index
//     after the checksum field; SHA1 field unused (empty-string hash).
//   CR2W: table checksums CRC32; header checksum CRC32 of the 160-byte header with the field set to 0xDEADBEEF;
//     name hashes FNV-1a 64 folded to 32 bits; buffer checksum CRC32 of the stored buffer.
//   Textures are stored bottom row first.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace IHeartRadioManager
{
    public static class IconArchive
    {
        public const int IconSize = 512;

        public static string IconName(long stationId) { return "iheart_" + stationId.ToString("D8"); }
        public static string AtlasPath(long stationId) { return "iheartradio\\icons\\" + IconName(stationId) + ".inkatlas"; }
        static string TexturePath(long stationId) { return "iheartradio\\icons\\" + IconName(stationId) + ".xbm"; }

        // ---------------------------------------------------------------- public entry point
        /// <summary>Writes the archive with one icon per (station id, 512x512 PNG). Returns false if there was nothing
        /// to write (and deletes the old archive).</summary>
        public static bool Write(string archivePath, string gameDir, IList<KeyValuePair<long, string>> icons)
        {
            if (icons.Count == 0)
            {
                if (File.Exists(archivePath)) File.Delete(archivePath);
                return false;
            }
            byte[] xbmMain = Template("iheart_00000000.xbm.main");
            byte[] atlasTpl = Template("iheart_00000000.inkatlas");
            var files = new List<ArchiveFile>();
            foreach (var icon in icons)
            {
                byte[] alpha = LoadAlpha(icon.Value);
                byte[] raw = Bc7.EncodeGrey(alpha, IconSize, IconSize);
                byte[] main = PatchTexture(xbmMain, raw, raw.Length);
                files.Add(new ArchiveFile(TexturePath(icon.Key), new[] { main, raw }, new[] { main.Length, raw.Length }));
                byte[] atlas = PatchAtlas(atlasTpl, icon.Key);
                files.Add(new ArchiveFile(AtlasPath(icon.Key), new[] { atlas }, new[] { atlas.Length }));
            }
            byte[] data = BuildArchive(files);
            Directory.CreateDirectory(Path.GetDirectoryName(archivePath));
            string tmp = archivePath + ".tmp";
            if (File.Exists(tmp)) File.Delete(tmp);     // left over from a write the game blocked
            File.WriteAllBytes(tmp, data);
            if (File.Exists(archivePath)) File.Delete(archivePath);   // throws IOException while the game has it open
            File.Move(tmp, archivePath);
            return true;
        }

        // the CR2W templates ship next to the exe, in templates\
        static byte[] Template(string name)
        {
            var path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "templates", name);
            if (!File.Exists(path)) throw new InvalidOperationException("missing template " + path);
            return File.ReadAllBytes(path);
        }

        // alpha of a 512x512 icon PNG (white on transparent), bottom row first
        static byte[] LoadAlpha(string pngPath)
        {
            using (var src = new Bitmap(pngPath))
            using (var bmp = new Bitmap(IconSize, IconSize, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(bmp))
                {
                    g.Clear(Color.Transparent);
                    g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                    g.DrawImage(src, new Rectangle(0, 0, IconSize, IconSize));
                }
                var lockData = bmp.LockBits(new Rectangle(0, 0, IconSize, IconSize), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                var px = new byte[IconSize * lockData.Stride];
                Marshal.Copy(lockData.Scan0, px, 0, px.Length);
                bmp.UnlockBits(lockData);
                var a = new byte[IconSize * IconSize];
                for (int y = 0; y < IconSize; y++)
                    for (int x = 0; x < IconSize; x++)
                        a[(IconSize - 1 - y) * IconSize + x] = px[y * lockData.Stride + x * 4 + 3];
                return a;
            }
        }

        // ---------------------------------------------------------------- CR2W patching
        static uint U32(byte[] d, int o) { return BitConverter.ToUInt32(d, o); }
        static void Put(byte[] d, int o, uint v) { BitConverter.GetBytes(v).CopyTo(d, o); }
        static int TableOff(byte[] d, int t) { return (int)U32(d, 40 + 12 * t); }
        static int TableCount(byte[] d, int t) { return (int)U32(d, 40 + 12 * t + 4); }
        static void TableCrc(byte[] d, int t, int bytes) { Put(d, 40 + 12 * t + 8, Crc32.Compute(d, TableOff(d, t), bytes)); }
        static void HeaderCrc(byte[] d) { Put(d, 32, 0xDEADBEEF); Put(d, 32, Crc32.Compute(d, 0, 160)); }

        static byte[] PatchTexture(byte[] tpl, byte[] blob, int rawSize)
        {
            var d = (byte[])tpl.Clone();
            int b = TableOff(d, 5);                   // buffers: flags, index, offset, diskSize, memSize, crc32
            if (U32(d, b + 16) != rawSize) throw new InvalidOperationException("texture template size mismatch");
            Put(d, b + 8, (uint)d.Length);
            Put(d, b + 12, (uint)blob.Length);
            Put(d, b + 20, Crc32.Compute(blob, 0, blob.Length));
            TableCrc(d, 5, 24 * TableCount(d, 5));
            Put(d, 28, (uint)(d.Length + blob.Length));   // buffersEnd
            HeaderCrc(d);
            return d;
        }

        static byte[] PatchAtlas(byte[] tpl, long stationId)
        {
            var d = (byte[])tpl.Clone();
            int so = TableOff(d, 0), sc = TableCount(d, 0);
            byte[] from = Encoding.ASCII.GetBytes("iheart_00000000"), to = Encoding.ASCII.GetBytes(IconName(stationId));
            int hits = 0;
            for (int i = so; i <= so + sc - from.Length; i++)
            {
                bool m = true;
                for (int k = 0; k < from.Length && m; k++) m = d[i + k] == from[k];
                if (m) { Array.Copy(to, 0, d, i, to.Length); hits++; }
            }
            if (hits != 2) throw new InvalidOperationException("atlas template: expected 2 names, found " + hits);
            TableCrc(d, 0, sc);
            int no = TableOff(d, 1), nc = TableCount(d, 1);
            for (int k = 0; k < nc; k++)
            {
                int off = (int)U32(d, no + 8 * k);
                int end = Array.IndexOf<byte>(d, 0, so + off);
                int len = end - (so + off);
                Put(d, no + 8 * k + 4, len > 0 ? NameHash(d, so + off, len) : 0);
            }
            TableCrc(d, 1, 8 * nc);
            HeaderCrc(d);
            return d;
        }

        static ulong Fnv1a64(byte[] d, int o, int n)
        {
            ulong h = 0xcbf29ce484222325;
            for (int i = 0; i < n; i++) { h ^= d[o + i]; h *= 0x100000001b3; }
            return h;
        }
        static uint NameHash(byte[] d, int o, int n) { ulong h = Fnv1a64(d, o, n); return (uint)(h ^ (h >> 32)); }

        // ---------------------------------------------------------------- RDAR archive
        class ArchiveFile
        {
            public string Path; public byte[][] Segments; public int[] Sizes; public ulong Hash;
            public ArchiveFile(string path, byte[][] segments, int[] sizes)
            {
                Path = path; Segments = segments; Sizes = sizes;
                var b = Encoding.ASCII.GetBytes(path); Hash = Fnv1a64(b, 0, b.Length);
            }
        }

        static byte[] BuildArchive(List<ArchiveFile> files)
        {
            files = files.OrderBy(f => f.Hash).ToList();
            var ms = new MemoryStream();
            ms.Write(new byte[0xAC], 0, 0xAC);

            // file name list (optional, lets WolvenKit and other tools show names)
            var names = new MemoryStream();
            foreach (var f in files.OrderBy(f => f.Path, StringComparer.Ordinal))
            {
                var b = Encoding.ASCII.GetBytes(f.Path); names.Write(b, 0, b.Length); names.WriteByte(0);
            }
            var nb = names.ToArray();
            var lxrs = new MemoryStream();
            lxrs.Write(Encoding.ASCII.GetBytes("SRXL"), 0, 4);
            foreach (var v in new[] { 1, nb.Length, nb.Length, files.Count }) lxrs.Write(BitConverter.GetBytes(v), 0, 4);
            lxrs.Write(nb, 0, nb.Length);
            var lx = lxrs.ToArray();
            ms.Write(lx, 0, lx.Length);

            var segs = new List<long[]>();      // offset, zsize, size
            var entries = new List<int[]>();    // first segment, end segment
            foreach (var f in files)
            {
                int first = segs.Count;
                for (int i = 0; i < f.Segments.Length; i++)
                {
                    segs.Add(new long[] { ms.Position, f.Segments[i].Length, f.Sizes[i] });
                    ms.Write(f.Segments[i], 0, f.Segments[i].Length);
                }
                entries.Add(new[] { first, segs.Count });
            }
            long idxPos = Align(ms.Position);
            Pad(ms, idxPos);

            var body = new MemoryStream();
            var w = new BinaryWriter(body);
            w.Write(files.Count); w.Write(segs.Count); w.Write(0);
            long ts = DateTime.UtcNow.ToFileTimeUtc();
            byte[] emptySha1 = { 0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55, 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09 };
            for (int i = 0; i < files.Count; i++)
            {
                w.Write(files[i].Hash); w.Write(ts); w.Write(0); w.Write(entries[i][0]); w.Write(entries[i][1]); w.Write(0); w.Write(0);
                w.Write(emptySha1);
            }
            foreach (var s in segs) { w.Write(s[0]); w.Write((uint)s[1]); w.Write((uint)s[2]); }
            w.Flush();
            var bodyBytes = body.ToArray();

            var iw = new BinaryWriter(ms);
            iw.Write(8); iw.Write(8 + bodyBytes.Length); iw.Write(Crc64Xz.Compute(bodyBytes)); iw.Write(bodyBytes); iw.Flush();
            int idxSize = 16 + bodyBytes.Length;
            Pad(ms, Align(ms.Position));

            var data = ms.ToArray();
            var hw = new BinaryWriter(new MemoryStream(data));
            hw.Write(0x52414452u); hw.Write(12u); hw.Write((ulong)idxPos); hw.Write((uint)idxSize); hw.Write(0UL); hw.Write(0u); hw.Write((ulong)data.Length);
            hw.Write((uint)lx.Length);
            hw.Flush();
            return data;
        }

        static long Align(long p) { return (p + 4095) / 4096 * 4096; }

        // padding to the 4 KB boundaries, with the byte WolvenKit-built archives use
        static void Pad(MemoryStream ms, long to)
        {
            var pad = new byte[to - ms.Position];
            for (int i = 0; i < pad.Length; i++) pad[i] = 0xD9;
            ms.Write(pad, 0, pad.Length);
        }
    }

    // ---------------------------------------------------------------- BC7 (mode 6) for premultiplied white icons:
    // every pixel is (v, v, v, v), so the colours of a block lie on one line and the end points are exact.
    static class Bc7
    {
        static readonly int[] W = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

        public static byte[] EncodeGrey(byte[] v, int width, int height)
        {
            var output = new byte[width * height];
            var vals = new int[16];
            var pal = new int[16];
            var idx = new int[16];
            int o = 0;
            for (int by = 0; by < height; by += 4)
                for (int bx = 0; bx < width; bx += 4)
                {
                    int lo = 255, hi = 0;
                    for (int k = 0; k < 16; k++)
                    {
                        int p = v[(by + k / 4) * width + bx + k % 4];
                        vals[k] = p; if (p < lo) lo = p; if (p > hi) hi = p;
                    }
                    for (int k = 0; k < 16; k++) pal[k] = ((64 - W[k]) * lo + W[k] * hi + 32) >> 6;
                    for (int k = 0; k < 16; k++)
                    {
                        int best = 0, bd = int.MaxValue;
                        for (int j = 0; j < 16; j++) { int dd = Math.Abs(pal[j] - vals[k]); if (dd < bd) { bd = dd; best = j; } }
                        idx[k] = best;
                    }
                    int e0 = lo, e1 = hi;
                    if (idx[0] >= 8) { e0 = hi; e1 = lo; for (int k = 0; k < 16; k++) idx[k] = 15 - idx[k]; }
                    WriteBlock(output, o, e0, e1, idx);
                    o += 16;
                }
            return output;
        }

        static void WriteBlock(byte[] dst, int o, int e0, int e1, int[] idx)
        {
            ulong lo = 0, hi = 0; int pos = 0;
            Action<ulong, int> put = (val, n) =>
            {
                val &= (1UL << n) - 1;
                if (pos < 64) { lo |= val << pos; if (pos + n > 64) hi |= val >> (64 - pos); }
                else hi |= val << (pos - 64);
                pos += n;
            };
            put(1UL << 6, 7);                                     // mode 6
            for (int c = 0; c < 4; c++) { put((ulong)(e0 >> 1), 7); put((ulong)(e1 >> 1), 7); }
            put((ulong)(e0 & 1), 1); put((ulong)(e1 & 1), 1);    // p-bits
            put((ulong)idx[0], 3);
            for (int k = 1; k < 16; k++) put((ulong)idx[k], 4);
            BitConverter.GetBytes(lo).CopyTo(dst, o);
            BitConverter.GetBytes(hi).CopyTo(dst, o + 8);
        }
    }

    static class Crc32
    {
        static readonly uint[] T = Make();
        static uint[] Make()
        {
            var t = new uint[256];
            for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1; t[i] = c; }
            return t;
        }
        public static uint Compute(byte[] d, int o, int n)
        {
            uint c = 0xFFFFFFFF;
            for (int i = 0; i < n; i++) c = T[(c ^ d[o + i]) & 0xFF] ^ (c >> 8);
            return c ^ 0xFFFFFFFF;
        }
    }

    static class Crc64Xz
    {
        static readonly ulong[] T = Make();
        static ulong[] Make()
        {
            var t = new ulong[256];
            for (ulong i = 0; i < 256; i++) { ulong c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xC96C5795D7870F42 ^ (c >> 1) : c >> 1; t[i] = c; }
            return t;
        }
        public static ulong Compute(byte[] d)
        {
            ulong c = ulong.MaxValue;
            foreach (byte b in d) c = T[(c ^ b) & 0xFF] ^ (c >> 8);
            return c ^ ulong.MaxValue;
        }
    }
}
