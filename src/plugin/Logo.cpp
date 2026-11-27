// iHeart Radio - logo engine (see Logo.hpp). Kept step for step with logo-core.js so both pick the same styles.
#include "Logo.hpp"

#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include <stb_image.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <limits>

namespace Logo
{
namespace
{
double LumOf(double r, double g, double b) { return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255; }

int Dist2(const std::vector<uint8_t>& a, int i, const std::vector<uint8_t>& b, int j)
{
    const int dr = a[i] - b[j], dg = a[i + 1] - b[j + 1], db = a[i + 2] - b[j + 2];
    return dr * dr + dg * dg + db * db;
}

double Dist2(const std::vector<uint8_t>& a, int i, const std::array<double, 3>& c)
{
    const double dr = a[i] - c[0], dg = a[i + 1] - c[1], db = a[i + 2] - c[2];
    return dr * dr + dg * dg + db * db;
}

double Dist2(const std::array<double, 3>& a, const std::array<double, 3>& c)
{
    const double dr = a[0] - c[0], dg = a[1] - c[1], db = a[2] - c[2];
    return dr * dr + dg * dg + db * db;
}

double Smooth(double e0, double e1, double x)
{
    const double t = std::min(1.0, std::max(0.0, (x - e0) / (e1 - e0)));
    return t * t * (3 - 2 * t);
}

// JavaScript's Math.round for the positive numbers used here
int JsRound(double x) { return static_cast<int>(std::floor(x + 0.5)); }

// ------------------------------------------------------------------ background
// Transparent logos: the alpha channel. Opaque logos (a white / coloured / gradient square): grown from the image
// border through small colour steps, so smooth gradients go but the logo's hard edges stop it.
std::vector<float> Foreground(const std::vector<uint8_t>& rgba, int w, int h, std::string& aRemoved)
{
    const int n = w * h;
    std::vector<float> a(n);
    int transparent = 0;
    for (int i = 0; i < n; i++)
    {
        a[i] = static_cast<float>(rgba[i * 4 + 3] / 255.0);
        if (a[i] < 0.1)
            transparent++;
    }
    if (static_cast<double>(transparent) / n > 0.04)
    {
        aRemoved = "alpha";
        return a;
    }

    const int STEP = 14 * 14 * 3;    // max colour change between two neighbouring background pixels
    const int DRIFT = 90 * 90 * 3;   // max distance from the border colour it started from
    std::vector<uint8_t> bg(n);
    std::vector<int> origin(n, -1), queue(n);
    int qh = 0, qt = 0;
    auto seed = [&](int p) {
        if (!bg[p])
        {
            bg[p] = 1;
            origin[p] = p;
            queue[qt++] = p;
        }
    };
    for (int x = 0; x < w; x++)
    {
        seed(x);
        seed((h - 1) * w + x);
    }
    for (int y = 0; y < h; y++)
    {
        seed(y * w);
        seed(y * w + w - 1);
    }
    while (qh < qt)
    {
        const int p = queue[qh++], x = p % w, y = p / w, o = origin[p];
        auto visit = [&](int q) {
            if (bg[q])
                return;
            if (Dist2(rgba, q * 4, rgba, p * 4) > STEP)
                return;
            if (Dist2(rgba, q * 4, rgba, o * 4) > DRIFT)
                return;
            bg[q] = 1;
            origin[q] = o;
            queue[qt++] = q;
        };
        if (x > 0) visit(p - 1);
        if (x < w - 1) visit(p + 1);
        if (y > 0) visit(p - w);
        if (y < h - 1) visit(p + w);
    }
    if (static_cast<double>(qt) / n < 0.05)
    {
        aRemoved = "none";   // the logo fills the whole picture
        return a;
    }
    // soft edge: pixels next to the background fade by how close their colour is to it
    for (int p = 0; p < n; p++)
    {
        if (bg[p])
        {
            a[p] = 0;
            continue;
        }
        const int x = p % w, y = p / w;
        int near = -1;
        if (x > 0 && bg[p - 1]) near = p - 1;
        else if (x < w - 1 && bg[p + 1]) near = p + 1;
        else if (y > 0 && bg[p - w]) near = p - w;
        else if (y < h - 1 && bg[p + w]) near = p + w;
        if (near >= 0)
            a[p] = static_cast<float>(std::min(static_cast<double>(a[p]),
                                               std::min(1.0, std::sqrt(Dist2(rgba, p * 4, rgba, near * 4) / (3.0 * 70 * 70)))));
    }
    aRemoved = "color";
    return a;
}

// ------------------------------------------------------------------ colour clusters (k-means, merged when close)
void Clusters(Analysis& an, int kMax)
{
    const int w = an.w, h = an.h, n = w * h;
    const auto& rgba = an.rgba;
    std::vector<int> pts;
    for (int i = 0; i < n; i += 3)
        if (an.alpha[i] > 0.6)
            pts.push_back(i);
    an.labels.assign(n, -1);
    if (pts.empty())
        return;
    auto lumAt = [&](int p) { return LumOf(rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]); };
    // init: spread over luminance (stable, like Array.prototype.sort)
    std::stable_sort(pts.begin(), pts.end(), [&](int p, int q) { return lumAt(p) < lumAt(q); });
    const int len = static_cast<int>(pts.size());
    const int k = std::min(kMax, len);
    std::vector<std::array<double, 3>> centers;
    for (int c = 0; c < k; c++)
    {
        const int p = pts[std::min(len - 1, static_cast<int>(std::floor((c + 0.5) * len / k)))];
        centers.push_back({static_cast<double>(rgba[p * 4]), static_cast<double>(rgba[p * 4 + 1]), static_cast<double>(rgba[p * 4 + 2])});
    }
    // a saturated colour that luminance spreading may miss: add the most saturated point as a seed
    int satP = -1, satV = 0;
    for (int p : pts)
    {
        const int r = rgba[p * 4], g = rgba[p * 4 + 1], b = rgba[p * 4 + 2];
        const int s = std::max({r, g, b}) - std::min({r, g, b});
        if (s > satV)
        {
            satV = s;
            satP = p;
        }
    }
    if (satP >= 0 && satV > 80)
        centers[k / 2] = {static_cast<double>(rgba[satP * 4]), static_cast<double>(rgba[satP * 4 + 1]), static_cast<double>(rgba[satP * 4 + 2])};
    for (int it = 0; it < 12; it++)
    {
        std::vector<std::array<double, 4>> sum(centers.size(), {0, 0, 0, 0});
        for (int p : pts)
        {
            size_t bi = 0;
            double bd = std::numeric_limits<double>::infinity();
            for (size_t c = 0; c < centers.size(); c++)
            {
                const double d = Dist2(rgba, p * 4, centers[c]);
                if (d < bd)
                {
                    bd = d;
                    bi = c;
                }
            }
            auto& s = sum[bi];
            s[0] += rgba[p * 4];
            s[1] += rgba[p * 4 + 1];
            s[2] += rgba[p * 4 + 2];
            s[3]++;
        }
        std::vector<std::array<double, 3>> next;
        for (const auto& s : sum)
            if (s[3] > 0)
                next.push_back({s[0] / s[3], s[1] / s[3], s[2] / s[3]});
        centers = next;
    }
    // merge near-identical clusters
    std::vector<std::array<double, 3>> merged;
    for (const auto& c : centers)
    {
        bool close = false;
        for (const auto& m : merged)
            if (Dist2(m, c) < 55.0 * 55 * 3)
                close = true;
        if (!close)
            merged.push_back(c);
    }
    an.centers = merged;
    an.share.assign(merged.size(), 0.0f);
    std::vector<double> count(merged.size(), 0);
    int fg = 0;
    for (int i = 0; i < n; i++)
    {
        if (an.alpha[i] < 0.35)
            continue;
        int bi = 0;
        double bd = std::numeric_limits<double>::infinity();
        for (size_t c = 0; c < merged.size(); c++)
        {
            const double d = Dist2(rgba, i * 4, merged[c]);
            if (d < bd)
            {
                bd = d;
                bi = static_cast<int>(c);
            }
        }
        an.labels[i] = static_cast<int8_t>(bi);
        count[bi]++;
        fg++;
    }
    for (size_t c = 0; c < count.size(); c++)
        an.share[c] = static_cast<float>(count[c] / std::max(1, fg));
}

// ------------------------------------------------------------------ helpers on masks
std::vector<uint8_t> Dilate(const std::vector<uint8_t>& mask, int w, int h, int r)
{
    if (r <= 0)
        return mask;
    std::vector<uint8_t> tmp(w * h), out(w * h);
    for (int y = 0; y < h; y++)
    {
        double run = -1e9;
        for (int x = 0; x < w; x++)
        {
            if (mask[y * w + x]) run = x;
            if (x - run <= r) tmp[y * w + x] = 1;
        }
        run = 1e9;
        for (int x = w - 1; x >= 0; x--)
        {
            if (mask[y * w + x]) run = x;
            if (run - x <= r) tmp[y * w + x] = 1;
        }
    }
    for (int x = 0; x < w; x++)
    {
        double run = -1e9;
        for (int y = 0; y < h; y++)
        {
            if (tmp[y * w + x]) run = y;
            if (y - run <= r) out[y * w + x] = 1;
        }
        run = 1e9;
        for (int y = h - 1; y >= 0; y--)
        {
            if (tmp[y * w + x]) run = y;
            if (run - y <= r) out[y * w + x] = 1;
        }
    }
    return out;
}

// thin cuts where two kept colour regions touch, so shapes stay readable in one colour
std::vector<uint8_t> CutLines(const Analysis& an, const std::vector<bool>& keep, int gap)
{
    const int w = an.w, h = an.h;
    if (gap <= 0)
        return {};
    std::vector<uint8_t> edge(w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const int i = y * w + x, l = an.labels[i];
            if (l < 0 || !keep[l])
                continue;
            const int r = x + 1 < w ? an.labels[i + 1] : -1, d = y + 1 < h ? an.labels[i + w] : -1;
            if (r >= 0 && r != l && keep[r]) edge[i] = 1;
            if (d >= 0 && d != l && keep[d]) edge[i] = 1;
        }
    return Dilate(edge, w, h, std::max(1, JsRound(gap / 2.0)));
}

double Otsu(const Analysis& an)
{
    double hist[64]{};
    double tot = 0;
    for (int i = 0; i < an.w * an.h; i++)
        if (an.alpha[i] > 0.5)
        {
            hist[std::min(63, static_cast<int>(std::floor(an.lum[i] * 64.0)))]++;
            tot++;
        }
    double sum = 0;
    for (int i = 0; i < 64; i++) sum += i * hist[i];
    double sumB = 0, wB = 0, best = 0;
    int t = 32;
    for (int i = 0; i < 64; i++)
    {
        wB += hist[i];
        if (!wB) continue;
        const double wF = tot - wB;
        if (!wF) break;
        sumB += i * hist[i];
        const double mB = sumB / wB, mF = (sum - sumB) / wF, v = wB * wF * (mB - mF) * (mB - mF);
        if (v > best)
        {
            best = v;
            t = i;
        }
    }
    return (t + 0.5) / 64;
}

// 4-connected components of ink > 0.5: per-pixel label, areas, boxes
struct Components
{
    std::vector<int> lab;
    std::vector<int> areas;
    std::vector<std::array<int, 4>> boxes;
};

Components FindComponents(const std::vector<float>& ink, int w, int h)
{
    const int n = w * h;
    Components c;
    c.lab.assign(n, -1);
    std::vector<int> q(n);
    for (int s = 0; s < n; s++)
    {
        if (ink[s] <= 0.5 || c.lab[s] >= 0)
            continue;
        const int id = static_cast<int>(c.areas.size());
        int qh = 0, qt = 0, area = 0, x0 = w, y0 = h, x1 = 0, y1 = 0;
        c.lab[s] = id;
        q[qt++] = s;
        while (qh < qt)
        {
            const int p = q[qh++], x = p % w, y = p / w;
            area++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            const int nb[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1};
            for (int r : nb)
                if (r >= 0 && c.lab[r] < 0 && ink[r] > 0.5)
                {
                    c.lab[r] = id;
                    q[qt++] = r;
                }
        }
        c.areas.push_back(area);
        c.boxes.push_back({x0, y0, x1, y1});
    }
    return c;
}

// remove tiny marks away from the logo (watermark dots, stray pixels)
void Despeckle(std::vector<float>& ink, int w, int h)
{
    const Components c = FindComponents(ink, w, h);
    double total = 0;
    int biggest = 0;
    for (int a : c.areas)
    {
        total += a;
        biggest = std::max(biggest, a);
    }
    if (!total)
        return;
    // the logo's main body: its big pieces (or the biggest one)
    const double big = std::min(total * 0.05, static_cast<double>(biggest));
    int X0 = w, Y0 = h, X1 = -1, Y1 = -1;
    for (size_t i = 0; i < c.areas.size(); i++)
        if (c.areas[i] >= big)
        {
            const auto& b = c.boxes[i];
            X0 = std::min(X0, b[0]);
            Y0 = std::min(Y0, b[1]);
            X1 = std::max(X1, b[2]);
            Y1 = std::max(Y1, b[3]);
        }
    if (X1 < 0)
        return;
    const int pad = JsRound(std::max(w, h) * 0.04);
    X0 -= pad;
    Y0 -= pad;
    X1 += pad;
    Y1 += pad;
    const int edge = JsRound(std::max(w, h) * 0.03);
    std::vector<bool> drop(c.areas.size());
    for (size_t i = 0; i < c.areas.size(); i++)
    {
        const auto& b = c.boxes[i];
        const double a = c.areas[i];
        const bool outside = b[2] < X0 || b[0] > X1 || b[3] < Y0 || b[1] > Y1;
        // a small piece in a corner of the picture: the image service's watermark, not the logo
        const bool corner = (b[2] < edge * 2 || b[0] > w - edge * 2) && (b[3] < edge * 2 || b[1] > h - edge * 2);
        drop[i] = a < total * 0.0005 || (a < total * 0.03 && (outside || corner));
    }
    for (int i = 0; i < w * h; i++)
        if (c.lab[i] >= 0 && drop[c.lab[i]])
            ink[i] = 0;
    // soft edge pixels (<= 0.5) touching a dropped piece go too
    for (int i = 0; i < w * h; i++)
    {
        if (ink[i] > 0 && ink[i] <= 0.5)
        {
            const int x = i % w, y = i / w;
            const int nb[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
            for (int r : nb)
                if (r >= 0 && c.lab[r] >= 0 && drop[c.lab[r]])
                {
                    ink[i] = 0;
                    break;
                }
        }
    }
}

// mask with its holes filled: everything the outside can't reach without crossing the mask
std::vector<uint8_t> Filled(const std::vector<uint8_t>& mask, int w, int h)
{
    const int n = w * h;
    std::vector<uint8_t> out(n, 1);
    std::vector<int> q(n);
    int qh = 0, qt = 0;
    auto seed = [&](int p) {
        if (out[p] && !mask[p])
        {
            out[p] = 0;
            q[qt++] = p;
        }
    };
    for (int x = 0; x < w; x++)
    {
        seed(x);
        seed((h - 1) * w + x);
    }
    for (int y = 0; y < h; y++)
    {
        seed(y * w);
        seed(y * w + w - 1);
    }
    while (qh < qt)
    {
        const int p = q[qh++], x = p % w, y = p / w;
        if (x > 0) seed(p - 1);
        if (x < w - 1) seed(p + 1);
        if (y > 0) seed(p - w);
        if (y < h - 1) seed(p + w);
    }
    return out;
}

// 1 = what was left out of the logo is a box around the rest, or sits inside the rest: the logo still reads.
// 0 = whole letters went missing.
double Enclosure(const Analysis& an, const std::vector<float>& ink, int fgCount)
{
    const int w = an.w, h = an.h, n = w * h;
    std::vector<uint8_t> kept(n), removed(n);
    int nk = 0, nr = 0;
    for (int i = 0; i < n; i++)
    {
        if (ink[i] > 0.5)
        {
            kept[i] = 1;
            nk++;
        }
        else if (an.alpha[i] > 0.5)
        {
            removed[i] = 1;
            nr++;
        }
    }
    if (nr < fgCount * 0.04 || nk == 0)
        return 1;
    const auto fr = Filled(removed, w, h), fk = Filled(kept, w, h);
    double a = 0, b = 0;
    for (int i = 0; i < n; i++)
    {
        if (kept[i] && fr[i]) a++;
        if (removed[i] && fk[i]) b++;
    }
    return std::max(a / nk, b / nr);
}

bool Similar(const std::vector<float>& a, const std::vector<float>& b)
{
    int inter = 0, uni = 0;
    for (size_t i = 0; i < a.size(); i += 7)
    {
        const bool x = a[i] > 0.5, y = b[i] > 0.5;
        if (x && y) inter++;
        if (x || y) uni++;
    }
    return uni == 0 || static_cast<double>(inter) / uni > 0.96;
}

struct Stats
{
    double cov = 0, detail = 0;
    int on = 0;
    int box[4] = {0, 0, -1, -1};
};

// coverage inside the ink's bounding box, amount of outline (detail)
Stats GetStats(const std::vector<float>& ink, int w, int h)
{
    int x0 = w, y0 = h, x1 = -1, y1 = -1, on = 0, edges = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            if (!(ink[y * w + x] > 0.5))
                continue;
            on++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            if (x == 0 || x == w - 1 || y == 0 || y == h - 1 || ink[y * w + x - 1] <= 0.5 || ink[y * w + x + 1] <= 0.5 ||
                ink[(y - 1) * w + x] <= 0.5 || ink[(y + 1) * w + x] <= 0.5)
                edges++;
        }
    Stats s;
    const double box = std::max(1, (x1 - x0 + 1) * (y1 - y0 + 1));
    s.cov = on / box;
    s.detail = edges / std::sqrt(std::max(1.0, static_cast<double>(on)));
    s.on = on;
    s.box[0] = x0;
    s.box[1] = y0;
    s.box[2] = x1;
    s.box[3] = y1;
    return s;
}

double Score(const Stats& s, const std::string& key)
{
    // good icons: 20-45 % filled, lots of outline (lettering) but not dust
    const double c = s.cov;
    const double covScore = c < 0.08 ? c / 0.08 * 0.3
                            : c < 0.2 ? 0.3 + (c - 0.08) / 0.12 * 0.7
                            : c <= 0.45 ? 1
                            : c < 0.85 ? 1 - (c - 0.45) / 0.4 * 0.7
                                        : 0.3;
    const double detail = std::min(s.detail, 40.0) / 40;
    const double bias = key == "solid" ? 0.95 : 1;   // prefer a knocked-out / line version when they're as good
    return covScore * (0.35 + detail) * bias;
}
} // namespace

// ------------------------------------------------------------------ public
bool Decode(const std::string& aBytes, Image& aOut, int aMaxSide)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(aBytes.data()), static_cast<int>(aBytes.size()),
                                              &w, &h, &comp, 4);
    if (!px || w < 1 || h < 1)
    {
        if (px)
            stbi_image_free(px);
        return false;
    }
    const double s = std::min(1.0, static_cast<double>(aMaxSide) / std::max(w, h));
    const int ow = std::max(1, JsRound(w * s)), oh = std::max(1, JsRound(h * s));
    aOut.w = ow;
    aOut.h = oh;
    aOut.rgba.assign(static_cast<size_t>(ow) * oh * 4, 0);
    if (ow == w && oh == h)
        aOut.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    else
    {
        // area average with premultiplied alpha (what a browser canvas does when it scales down)
        const double sx = static_cast<double>(w) / ow, sy = static_cast<double>(h) / oh;
        for (int y = 0; y < oh; y++)
        {
            const double y0 = y * sy, y1 = (y + 1) * sy;
            for (int x = 0; x < ow; x++)
            {
                const double x0 = x * sx, x1 = (x + 1) * sx;
                double acc[4]{}, wsum = 0;
                for (int yy = static_cast<int>(y0); yy < std::min(h, static_cast<int>(std::ceil(y1))); yy++)
                {
                    const double wy = std::min(yy + 1.0, y1) - std::max(static_cast<double>(yy), y0);
                    for (int xx = static_cast<int>(x0); xx < std::min(w, static_cast<int>(std::ceil(x1))); xx++)
                    {
                        const double wx = std::min(xx + 1.0, x1) - std::max(static_cast<double>(xx), x0);
                        const unsigned char* p = px + (static_cast<size_t>(yy) * w + xx) * 4;
                        const double a = p[3] / 255.0, k = wx * wy;
                        acc[0] += p[0] * a * k;
                        acc[1] += p[1] * a * k;
                        acc[2] += p[2] * a * k;
                        acc[3] += a * k;
                        wsum += k;
                    }
                }
                uint8_t* o = &aOut.rgba[(static_cast<size_t>(y) * ow + x) * 4];
                if (wsum > 0 && acc[3] > 0)
                {
                    o[0] = static_cast<uint8_t>(std::min(255.0, acc[0] / acc[3] + 0.5));
                    o[1] = static_cast<uint8_t>(std::min(255.0, acc[1] / acc[3] + 0.5));
                    o[2] = static_cast<uint8_t>(std::min(255.0, acc[2] / acc[3] + 0.5));
                    o[3] = static_cast<uint8_t>(std::min(255.0, acc[3] / wsum * 255 + 0.5));
                }
            }
        }
    }
    stbi_image_free(px);
    return true;
}

Analysis Analyze(const Image& aImage)
{
    Analysis an;
    an.w = aImage.w;
    an.h = aImage.h;
    an.rgba = aImage.rgba;
    an.alpha = Foreground(an.rgba, an.w, an.h, an.bgRemoved);
    Clusters(an, 5);
    const int n = an.w * an.h;
    an.lum.resize(n);
    for (int i = 0; i < n; i++)
        an.lum[i] = static_cast<float>(LumOf(an.rgba[i * 4], an.rgba[i * 4 + 1], an.rgba[i * 4 + 2]));
    return an;
}

std::vector<Candidate> Candidates(const Analysis& an, int gap)
{
    const int w = an.w, h = an.h, n = w * h;
    const int nC = static_cast<int>(an.centers.size());
    std::vector<Candidate> out;

    auto fromKeep = [&](const std::vector<bool>& keep, const std::string& key, const std::string& label) {
        Candidate c;
        c.key = key;
        c.label = label;
        c.ink.assign(n, 0.0f);
        const auto cut = CutLines(an, keep, gap);
        for (int i = 0; i < n; i++)
        {
            const int l = an.labels[i];
            if (l >= 0 && !keep[l])
                continue;
            if (!cut.empty() && cut[i])
                continue;
            c.ink[i] = an.alpha[i];
        }
        out.push_back(std::move(c));
    };

    // 1. whole logo, colours separated by thin cuts
    fromKeep(std::vector<bool>(nC, true), "solid", "Full shape");
    // 2. one colour knocked out (e.g. white lettering cut out of a coloured badge, or a dark box removed)
    for (int c = 0; c < nC; c++)
    {
        if (an.share[c] < 0.03 || nC < 2)
            continue;
        std::vector<bool> keep(nC, true);
        keep[c] = false;
        const double L = LumOf(an.centers[c][0], an.centers[c][1], an.centers[c][2]);
        const char* what = L > 0.75 ? "light" : L < 0.25 ? "dark" : "colour";
        fromKeep(keep, "knock" + std::to_string(c), std::string("Without ") + what + " parts");
    }
    // 3. by brightness
    const double t = Otsu(an);
    Candidate dark{"dark", "Dark lines", std::vector<float>(n), 0}, light{"light", "Light lines", std::vector<float>(n), 0};
    for (int i = 0; i < n; i++)
    {
        dark.ink[i] = static_cast<float>(an.alpha[i] * Smooth(t + 0.08, t - 0.08, an.lum[i]));
        light.ink[i] = static_cast<float>(an.alpha[i] * Smooth(t - 0.08, t + 0.08, an.lum[i]));
    }
    out.push_back(std::move(dark));
    out.push_back(std::move(light));

    // clean up, drop near-duplicates, score, sort
    int fgCount = 0;
    for (int i = 0; i < n; i++)
        if (an.alpha[i] > 0.5)
            fgCount++;
    std::vector<Candidate> uniq;
    std::vector<Stats> stats;
    for (auto& c : out)
    {
        Despeckle(c.ink, w, h);
        const Stats s = GetStats(c.ink, w, h);
        if (s.cov < 0.01)
            continue;
        bool dup = false;
        for (const auto& u : uniq)
            if (Similar(u.ink, c.ink))
                dup = true;
        if (dup)
            continue;
        uniq.push_back(std::move(c));
        stats.push_back(s);
    }
    for (size_t i = 0; i < uniq.size(); i++)
        uniq[i].score = Score(stats[i], uniq[i].key) * (0.25 + 0.75 * Enclosure(an, uniq[i].ink, fgCount));
    std::stable_sort(uniq.begin(), uniq.end(), [](const Candidate& p, const Candidate& q) { return q.score < p.score; });
    return uniq;
}

std::vector<uint8_t> Fit(const std::vector<float>& ink, int w, int h, int size, bool invert)
{
    std::vector<float> src(static_cast<size_t>(w) * h, 0.0f);
    if (!invert)
        src = ink;
    else
    {
        // inverted: fill the logo's bounding shape and cut the ink out of it
        const Stats st = GetStats(ink, w, h);
        for (int y = st.box[1]; y <= st.box[3]; y++)
            for (int x = st.box[0]; x <= st.box[2]; x++)
                src[y * w + x] = 1 - ink[y * w + x];
    }
    const Stats st = GetStats(src, w, h);
    std::vector<uint8_t> out(static_cast<size_t>(size) * size, 0);
    if (st.on == 0)
        return out;
    const int x0 = st.box[0], y0 = st.box[1], x1 = st.box[2], y1 = st.box[3];
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const int margin = JsRound(size * 0.03);
    const double scale = std::min(static_cast<double>(size - 2 * margin) / bw, static_cast<double>(size - 2 * margin) / bh);
    const double ow = bw * scale, oh = bh * scale;
    const double ox = (size - ow) / 2, oy = (size - oh) / 2;
    // area-averaged resampling (box filter), good for both up and down scaling
    for (int y = 0; y < size; y++)
    {
        const double sy0 = (y - oy) / scale + y0, sy1 = (y + 1 - oy) / scale + y0;
        if (sy1 <= y0 || sy0 >= y1 + 1)
            continue;
        for (int x = 0; x < size; x++)
        {
            const double sx0 = (x - ox) / scale + x0, sx1 = (x + 1 - ox) / scale + x0;
            if (sx1 <= x0 || sx0 >= x1 + 1)
                continue;
            double acc = 0, wsum = 0;
            for (int sy = static_cast<int>(std::floor(sy0)); sy < static_cast<int>(std::ceil(sy1)); sy++)
            {
                if (sy < 0 || sy >= h)
                    continue;
                const double wy = std::min(sy + 1.0, sy1) - std::max(static_cast<double>(sy), sy0);
                for (int sx = static_cast<int>(std::floor(sx0)); sx < static_cast<int>(std::ceil(sx1)); sx++)
                {
                    if (sx < 0 || sx >= w)
                        continue;
                    const double wx = std::min(sx + 1.0, sx1) - std::max(static_cast<double>(sx), sx0);
                    acc += src[sy * w + sx] * wx * wy;
                    wsum += wx * wy;
                }
            }
            if (wsum > 0)
                out[y * size + x] = static_cast<uint8_t>(JsRound(255 * std::min(1.0, acc / wsum)));
        }
    }
    return out;
}
} // namespace Logo
