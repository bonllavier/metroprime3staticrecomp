# Compares two screenshots: global difference statistics, per-region mean colour, and an amplified diff image.
#   compare_images.ps1 first.png second.png diff.png [x,y,w,h ...]
param([string]$First, [string]$Second, [string]$Out, [string[]]$Regions)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
public static class ImageDiff {
  static byte[] Load(string path, out int w, out int h) {
    using (var bmp = new Bitmap(path)) {
      w = bmp.Width; h = bmp.Height;
      var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      var bytes = new byte[data.Stride * h];
      System.Runtime.InteropServices.Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
      bmp.UnlockBits(data);
      return bytes;
    }
  }
  public static string Run(string pa, string pb, string outPath, string[] regions) {
    int wa, ha, wb, hb;
    var a = Load(pa, out wa, out ha); var b = Load(pb, out wb, out hb);
    int w = Math.Min(wa, wb), h = Math.Min(ha, hb);
    var diff = new Bitmap(w, h, PixelFormat.Format32bppArgb);
    long big = 0; double sum = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      int ia = y * wa * 4 + x * 4, ib = y * wb * 4 + x * 4;
      int db = Math.Abs(a[ia] - b[ib]), dg = Math.Abs(a[ia + 1] - b[ib + 1]), dr = Math.Abs(a[ia + 2] - b[ib + 2]);
      if (Math.Max(dr, Math.Max(dg, db)) > 40) big++;
      sum += (dr + dg + db) / 3.0;
      diff.SetPixel(x, y, Color.FromArgb(255, Math.Min(255, dr * 4), Math.Min(255, dg * 4), Math.Min(255, db * 4)));
    }
    diff.Save(outPath);
    var text = string.Format("size={0}x{1} pixels_diff_gt40={2} ({3:F3} %) mean_abs_diff={4:F3}", w, h, big, 100.0 * big / (w * h), sum / (w * h));
    foreach (var r in regions ?? new string[0]) {
      var p = r.Split(',');
      int x0 = int.Parse(p[0]), y0 = int.Parse(p[1]), rw = int.Parse(p[2]), rh = int.Parse(p[3]);
      double[] sa = new double[3], sb = new double[3]; int n = 0;
      for (int y = y0; y < y0 + rh && y < h; y++) for (int x = x0; x < x0 + rw && x < w; x++) {
        int ia = y * wa * 4 + x * 4, ib = y * wb * 4 + x * 4;
        sa[0] += a[ia + 2]; sa[1] += a[ia + 1]; sa[2] += a[ia]; sb[0] += b[ib + 2]; sb[1] += b[ib + 1]; sb[2] += b[ib]; n++;
      }
      text += string.Format("\nregion {0}: first=({1:F0},{2:F0},{3:F0}) second=({4:F0},{5:F0},{6:F0})", r, sa[0] / n, sa[1] / n, sa[2] / n, sb[0] / n, sb[1] / n, sb[2] / n);
    }
    return text;
  }
}
"@
[ImageDiff]::Run((Resolve-Path $First).Path, (Resolve-Path $Second).Path, [IO.Path]::GetFullPath($Out), [string[]]$Regions)
