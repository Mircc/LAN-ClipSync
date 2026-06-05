// 将 icon_NxN.png 合并为 MSVC 资源编译器兼容的多尺寸 .ico（BMP DIB 格式）
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

static class BuildIcon
{
    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: BuildIcon <icon_sizes_dir> <output.ico>");
            return 1;
        }

        var dir = args[0];
        var outPath = args[1];
        var order = new[] { 16, 24, 32, 48, 64, 128, 256 };
        var paths = new List<string>();
        foreach (var s in order)
        {
            var p = Path.Combine(dir, $"icon_{s}x{s}.png");
            if (File.Exists(p))
            {
                paths.Add(p);
            }
        }

        if (paths.Count == 0)
        {
            Console.Error.WriteLine("No icon_*.png files found.");
            return 1;
        }

        WriteIco(outPath, paths);
        Console.WriteLine($"Wrote {outPath} ({paths.Count} sizes: {string.Join(", ", order)})");
        return 0;
    }

    static void WriteIco(string path, List<string> pngPaths)
    {
        var images = new List<byte[]>();
        var widths = new List<int>();
        var heights = new List<int>();

        foreach (var p in pngPaths)
        {
            using var src = new Bitmap(p);
            var w = src.Width;
            var h = src.Height;
            widths.Add(w);
            heights.Add(h);
            images.Add(Encode32bppBmp(src));
        }

        using var fs = File.Open(path, FileMode.Create, FileAccess.Write);
        using var bw = new BinaryWriter(fs);
        bw.Write((ushort)0);
        bw.Write((ushort)1);
        bw.Write((ushort)images.Count);

        var offset = 6 + 16 * images.Count;
        for (var i = 0; i < images.Count; i++)
        {
            var w = widths[i];
            var h = heights[i];
            bw.Write((byte)(w >= 256 ? 0 : w));
            bw.Write((byte)(h >= 256 ? 0 : h));
            bw.Write((byte)0);
            bw.Write((byte)0);
            bw.Write((ushort)1);
            bw.Write((ushort)32);
            bw.Write((uint)images[i].Length);
            bw.Write((uint)offset);
            offset += images[i].Length;
        }

        foreach (var data in images)
        {
            bw.Write(data);
        }
    }

    static byte[] Encode32bppBmp(Bitmap src)
    {
        var w = src.Width;
        var h = src.Height;
        const int headerSize = 40;
        var xorSize = w * h * 4;
        var andRowBytes = ((w + 31) / 32) * 4;
        var andSize = andRowBytes * h;
        var total = headerSize + xorSize + andSize;

        using var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.Transparent);
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.DrawImage(src, 0, 0, w, h);
        }

        var buffer = new byte[total];
        WriteInt32(buffer, 0, headerSize);
        WriteInt32(buffer, 4, w);
        WriteInt32(buffer, 8, h * 2);
        WriteInt16(buffer, 12, 1);
        WriteInt16(buffer, 14, 32);
        WriteInt32(buffer, 16, 0);
        WriteInt32(buffer, 20, xorSize + andSize);

        var locked = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
            PixelFormat.Format32bppArgb);
        try
        {
            var stride = Math.Abs(locked.Stride);
            var row = new byte[stride];
            var xorOffset = headerSize;
            for (var y = h - 1; y >= 0; y--)
            {
                Marshal.Copy(locked.Scan0 + y * stride, row, 0, stride);
                for (var x = 0; x < w; x++)
                {
                    var si = x * 4;
                    var di = xorOffset + x * 4;
                    buffer[di + 0] = row[si + 0];
                    buffer[di + 1] = row[si + 1];
                    buffer[di + 2] = row[si + 2];
                    buffer[di + 3] = row[si + 3];
                }
                xorOffset += w * 4;
            }
        }
        finally
        {
            bmp.UnlockBits(locked);
        }

        return buffer;
    }

    static void WriteInt16(byte[] b, int o, short v)
    {
        b[o] = (byte)(v & 0xFF);
        b[o + 1] = (byte)((v >> 8) & 0xFF);
    }

    static void WriteInt32(byte[] b, int o, int v)
    {
        b[o] = (byte)(v & 0xFF);
        b[o + 1] = (byte)((v >> 8) & 0xFF);
        b[o + 2] = (byte)((v >> 16) & 0xFF);
        b[o + 3] = (byte)((v >> 24) & 0xFF);
    }
}
