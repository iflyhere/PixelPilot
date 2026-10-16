#include "Snapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xri
{

namespace
{

const Rgb kBackground{18, 20, 24};
const Rgb kVideoFill{44, 52, 66};
const Rgb kVideoEdge{130, 140, 160};
const Rgb kAccent{70, 225, 205};
const Rgb kInk{240, 246, 252};
const Rgb kDim{150, 163, 180};

Rgb layerColour(int layer)
{
    switch (layer)
    {
        case LAYER_DASHBOARD:
            return {80, 150, 215};
        case LAYER_MINIMAP:
            return {95, 180, 115};
        case LAYER_CHART:
            return {215, 160, 75};
        default:
            return {170, 170, 170};
    }
}

// 5x7, one byte per row, low five bits, leftmost column in bit 4.
struct Glyph
{
    char    c;
    uint8_t rows[7];
};

const Glyph kFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'.', {0, 0, 0, 0, 0, 12, 12}},
    {':', {0, 12, 12, 0, 12, 12, 0}},    {'/', {1, 1, 2, 4, 8, 16, 16}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},       {'=', {0, 0, 31, 0, 31, 0, 0}},
    {'%', {24, 25, 2, 4, 8, 19, 3}},     {',', {0, 0, 0, 0, 12, 4, 8}},
};

const uint8_t* glyphRows(char c)
{
    if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
    for (const Glyph& g : kFont)
    {
        if (g.c == c) return g.rows;
    }
    return nullptr;  // a space, or something not in the font
}

// --- PNG, uncompressed: no zlib needed, and these are test artefacts, not deliverables ---

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static bool     ready = false;
    if (!ready)
    {
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back((uint8_t) (v >> 24));
    out.push_back((uint8_t) (v >> 16));
    out.push_back((uint8_t) (v >> 8));
    out.push_back((uint8_t) v);
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    put32(out, (uint32_t) data.size());
    std::vector<uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), data.begin(), data.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put32(out, crc32(typed.data(), typed.size(), 0xFFFFFFFFu) ^ 0xFFFFFFFFu);
}

}  // namespace

Snapshot::Snapshot(Pose eye, int width, int height, float hfovDeg, float vfovDeg)
    : mEye(eye),
      mWidth(width),
      mHeight(height),
      mTanH(std::tan(hfovDeg * 0.5f * kDeg2Rad)),
      mTanV(std::tan(vfovDeg * 0.5f * kDeg2Rad)),
      mRgb((size_t) width * height * 3)
{
    for (size_t i = 0; i < mRgb.size(); i += 3)
    {
        mRgb[i]     = kBackground.r;
        mRgb[i + 1] = kBackground.g;
        mRgb[i + 2] = kBackground.b;
    }
    // Five-degree grid, so angles can be read off the picture.
    for (int deg = -45; deg <= 45; deg += 5)
    {
        const float t   = std::tan(deg * kDeg2Rad);
        const float px  = (t / mTanH + 1.0f) * 0.5f * (float) mWidth;
        const float py  = (1.0f - t / mTanV) * 0.5f * (float) mHeight;
        const float a   = deg == 0 ? 0.35f : (deg % 15 == 0 ? 0.18f : 0.08f);
        line({px, 0.0f}, {px, (float) mHeight}, kDim, 1.0f, a);
        line({0.0f, py}, {(float) mWidth, py}, kDim, 1.0f, a);
    }
}

bool Snapshot::project(Vec3 world, P2* out) const
{
    const Vec3 rel = rotate(conj(mEye.orientation), world - mEye.position);
    if (rel.z > -0.02f) return false;
    const float x = rel.x / -rel.z / mTanH;
    const float y = rel.y / -rel.z / mTanV;
    out->x        = (x + 1.0f) * 0.5f * (float) mWidth;
    out->y        = (1.0f - y) * 0.5f * (float) mHeight;
    return true;
}

std::vector<Snapshot::P2> Snapshot::projectPolygon(const std::vector<Vec3>& world) const
{
    constexpr float kNear = -0.02f;
    std::vector<Vec3> cam;
    for (const Vec3& w : world) cam.push_back(rotate(conj(mEye.orientation), w - mEye.position));
    // Sutherland-Hodgman against z = kNear, keeping what is in front.
    std::vector<Vec3> clipped;
    for (size_t i = 0; i < cam.size(); ++i)
    {
        const Vec3 a  = cam[i];
        const Vec3 b  = cam[(i + 1) % cam.size()];
        const bool ai = a.z <= kNear;
        const bool bi = b.z <= kNear;
        if (ai) clipped.push_back(a);
        if (ai != bi)
        {
            const float t = (kNear - a.z) / (b.z - a.z);
            clipped.push_back(a + (b - a) * t);
        }
    }
    std::vector<P2> out;
    for (const Vec3& c : clipped)
    {
        out.push_back({(c.x / -c.z / mTanH + 1.0f) * 0.5f * (float) mWidth,
                       (1.0f - c.y / -c.z / mTanV) * 0.5f * (float) mHeight});
    }
    return out;
}

void Snapshot::blend(int x, int y, Rgb c, float a)
{
    if (x < 0 || y < 0 || x >= mWidth || y >= mHeight || a <= 0.0f) return;
    a                 = a > 1.0f ? 1.0f : a;
    uint8_t*      px  = &mRgb[((size_t) y * mWidth + x) * 3];
    const uint8_t src[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) px[i] = (uint8_t) std::lround(px[i] + (src[i] - px[i]) * a);
}

void Snapshot::fillPolygon(const std::vector<P2>& pts, Rgb colour, float alpha)
{
    if (pts.size() < 3) return;
    float minY = pts[0].y;
    float maxY = pts[0].y;
    for (const P2& p : pts)
    {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const int y0 = std::max(0, (int) std::floor(minY));
    const int y1 = std::min(mHeight - 1, (int) std::ceil(maxY));
    std::vector<float> xs;
    for (int y = y0; y <= y1; ++y)
    {
        const float sy = (float) y + 0.5f;
        xs.clear();
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const P2 a = pts[i];
            const P2 b = pts[(i + 1) % pts.size()];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy))
            {
                xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
        {
            const int x0 = std::max(0, (int) std::ceil(xs[i] - 0.5f));
            const int x1 = std::min(mWidth - 1, (int) std::floor(xs[i + 1] - 0.5f));
            for (int x = x0; x <= x1; ++x) blend(x, y, colour, alpha);
        }
    }
}

void Snapshot::line(P2 a, P2 b, Rgb colour, float thickness, float alpha)
{
    const float r  = thickness * 0.5f;
    const int   x0 = std::max(0, (int) std::floor(std::min(a.x, b.x) - r - 1));
    const int   x1 = std::min(mWidth - 1, (int) std::ceil(std::max(a.x, b.x) + r + 1));
    const int   y0 = std::max(0, (int) std::floor(std::min(a.y, b.y) - r - 1));
    const int   y1 = std::min(mHeight - 1, (int) std::ceil(std::max(a.y, b.y) + r + 1));
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float ll = dx * dx + dy * dy;
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const float px = (float) x + 0.5f;
            const float py = (float) y + 0.5f;
            float       t  = ll > 0.0f ? ((px - a.x) * dx + (py - a.y) * dy) / ll : 0.0f;
            t              = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float ex = a.x + dx * t - px;
            const float ey = a.y + dy * t - py;
            const float d  = std::sqrt(ex * ex + ey * ey);
            const float cover = r + 0.5f - d;
            if (cover > 0.0f) blend(x, y, colour, alpha * (cover > 1.0f ? 1.0f : cover));
        }
    }
}

void Snapshot::outline(const std::vector<P2>& pts, Rgb colour, float thickness)
{
    for (size_t i = 0; i < pts.size(); ++i) line(pts[i], pts[(i + 1) % pts.size()], colour, thickness);
}

void Snapshot::disc(P2 c, float radius, Rgb colour, float alpha)
{
    const int x0 = std::max(0, (int) std::floor(c.x - radius - 1));
    const int x1 = std::min(mWidth - 1, (int) std::ceil(c.x + radius + 1));
    const int y0 = std::max(0, (int) std::floor(c.y - radius - 1));
    const int y1 = std::min(mHeight - 1, (int) std::ceil(c.y + radius + 1));
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const float dx    = (float) x + 0.5f - c.x;
            const float dy    = (float) y + 0.5f - c.y;
            const float cover = radius + 0.5f - std::sqrt(dx * dx + dy * dy);
            if (cover > 0.0f) blend(x, y, colour, alpha * (cover > 1.0f ? 1.0f : cover));
        }
    }
}

void Snapshot::text(const std::string& s, int x, int y, int scale, Rgb colour)
{
    int cx = x;
    for (char ch : s)
    {
        const uint8_t* rows = glyphRows(ch);
        if (rows != nullptr)
        {
            for (int r = 0; r < 7; ++r)
            {
                for (int col = 0; col < 5; ++col)
                {
                    if ((rows[r] >> (4 - col)) & 1)
                    {
                        for (int yy = 0; yy < scale; ++yy)
                        {
                            for (int xx = 0; xx < scale; ++xx)
                            {
                                blend(cx + col * scale + xx, y + r * scale + yy, colour, 1.0f);
                            }
                        }
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

std::vector<Vec3> Snapshot::outlineOf(const Surface& s) const
{
    std::vector<Vec3> pts;
    if (!s.cylinder)
    {
        const Vec3 x = rotate(s.pose.orientation, Vec3{s.width * 0.5f, 0.0f, 0.0f});
        const Vec3 y = rotate(s.pose.orientation, Vec3{0.0f, s.height * 0.5f, 0.0f});
        const Vec3 c = s.pose.position;
        pts          = {c - x + y, c + x + y, c + x - y, c - x - y};
        return pts;
    }
    constexpr int kSteps = 24;
    for (int i = 0; i <= kSteps; ++i)  // along the top, left to right
    {
        const float a = -s.centralAngle * 0.5f + s.centralAngle * (float) i / kSteps;
        pts.push_back(s.axisPose.position +
                      rotate(s.axisPose.orientation,
                             Vec3{s.radius * std::sin(a), s.height * 0.5f, -s.radius * std::cos(a)}));
    }
    for (int i = kSteps; i >= 0; --i)  // and back along the bottom
    {
        const float a = -s.centralAngle * 0.5f + s.centralAngle * (float) i / kSteps;
        pts.push_back(s.axisPose.position +
                      rotate(s.axisPose.orientation,
                             Vec3{s.radius * std::sin(a), -s.height * 0.5f, -s.radius * std::cos(a)}));
    }
    return pts;
}

void Snapshot::scene(const Placement* layers, int count, const Anchor& anchor, const Video& video,
                     int hovered, int held)
{
    // The video first, as the compositor does: everything else is drawn over it.
    const Surface vs = videoSurface(anchor, video);
    const auto    vp = projectPolygon(outlineOf(vs));
    fillPolygon(vp, kVideoFill, 1.0f);
    outline(vp, held == TARGET_VIDEO ? kAccent : kVideoEdge, held == TARGET_VIDEO ? 5.0f : 2.0f);
    P2 label;
    if (project(vs.pose.position, &label)) text("VIDEO", (int) label.x - 45, (int) label.y - 7, 3, kDim);

    for (int i = 0; i < count; ++i)
    {
        const Placement& p = layers[i];
        if (!p.present) continue;
        const Surface s   = panelSurface(p, anchor, video);
        const auto    pts = projectPolygon(outlineOf(s));
        if (i == LAYER_SYMBOLOGY)
        {
            // Sits on the video: an outline only, so the video edge stays readable under it.
            continue;
        }
        const Rgb c = layerColour(i);
        fillPolygon(pts, c, 0.35f);
        if (i == held)
        {
            outline(pts, kAccent, 5.0f);
        }
        else if (i == hovered)
        {
            outline(pts, kAccent, 3.0f);
        }
        else
        {
            outline(pts, c, 2.0f);
        }
        if (project(s.pose.position, &label))
        {
            const std::string name = layerName(i);
            text(name, (int) label.x - (int) name.size() * 6, (int) label.y - 7, 2, kInk);
        }
    }
}

void Snapshot::pointer(const PointerOut& p)
{
    if (p.beam)
    {
        const auto pts = projectPolygon({p.from, p.to});
        if (pts.size() >= 2) line(pts[0], pts[1], kInk, 3.0f, 0.85f);
    }
    if (p.cursor)
    {
        P2 c;
        P2 edge;
        const Vec3 side = rotate(p.cursorPose.orientation, Vec3{p.cursorSize * 0.5f, 0.0f, 0.0f});
        if (project(p.cursorPose.position, &c) && project(p.cursorPose.position + side, &edge))
        {
            const float r = std::max(3.0f, std::hypot(edge.x - c.x, edge.y - c.y));
            if (p.cursorFrame >= CURSOR_PROGRESS_FIRST)
            {
                // A ring filling up clockwise from the top.
                const int   step = p.cursorFrame - CURSOR_PROGRESS_FIRST + 1;
                const float sweep = 2.0f * kPi * (float) step / CURSOR_PROGRESS_STEPS;
                for (int k = 0; k < 64; ++k)
                {
                    const float a0 = 2.0f * kPi * (float) k / 64.0f;
                    if (a0 > sweep) break;
                    const float a1 = 2.0f * kPi * (float) (k + 1) / 64.0f;
                    line({c.x + r * std::sin(a0), c.y - r * std::cos(a0)},
                         {c.x + r * std::sin(a1), c.y - r * std::cos(a1)}, kAccent, 3.0f);
                }
                disc(c, r * 0.3f, kInk);
            }
            else
            {
                disc(c, r, {0, 0, 0}, 0.6f);
                disc(c, r * 0.75f, p.cursorFrame == CURSOR_IDLE ? kInk : kAccent);
                if (p.cursorFrame == CURSOR_HELD) disc(c, r * 0.35f, kInk);
            }
        }
    }
}

void Snapshot::caption(const std::string& s)
{
    text(s, 16, 16 + mCaptionLine * 26, 3, kInk);
    ++mCaptionLine;
}

bool Snapshot::save(const std::string& path) const
{
    // Raw rows with a filter byte each, in stored (uncompressed) deflate blocks.
    std::vector<uint8_t> raw;
    raw.reserve((size_t) mHeight * (mWidth * 3 + 1));
    for (int y = 0; y < mHeight; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), mRgb.begin() + (size_t) y * mWidth * 3,
                   mRgb.begin() + (size_t) (y + 1) * mWidth * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();)
    {
        const size_t   n     = std::min<size_t>(65535, raw.size() - pos);
        const bool     last  = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t) (n & 0xFF));
        z.push_back((uint8_t) (n >> 8));
        z.push_back((uint8_t) (~n & 0xFF));
        z.push_back((uint8_t) ((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t) mWidth);
    put32(ihdr, (uint32_t) mHeight);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bit, RGB, deflate, no filter, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

}  // namespace xri
