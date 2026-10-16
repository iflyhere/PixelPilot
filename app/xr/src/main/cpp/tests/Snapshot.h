// What the left eye would see, drawn on the CPU into a PNG: the video, the instrument panels,
// the beams and the cursors, as outlines and flat fills. Not a picture of the HUD - the HUD's
// own pixels come from the androidTest preview - but of where everything is, which is the part
// the interaction tests are about.

#ifndef PIXELPILOT_XRI_SNAPSHOT_H
#define PIXELPILOT_XRI_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

#include "../XrInteraction.h"

namespace xri
{

struct Rgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

class Snapshot
{
  public:
    /** A camera at eye, looking down its -Z, with a field of view about a Quest 3 eye's. */
    Snapshot(Pose eye, int width = 1000, int height = 960, float hfovDeg = 100.0f,
             float vfovDeg = 96.0f);

    void scene(const Placement* layers, int count, const Anchor& anchor, const Video& video,
               int hovered = TARGET_NONE, int held = TARGET_NONE);
    void pointer(const PointerOut& p);
    /** Text in the top left corner, one line per call. Upper case, digits and a few signs. */
    void caption(const std::string& text);

    bool save(const std::string& path) const;

  private:
    struct P2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    bool project(Vec3 world, P2* out) const;
    /** Clips against the near plane in camera space, then projects. */
    std::vector<P2> projectPolygon(const std::vector<Vec3>& world) const;
    void fillPolygon(const std::vector<P2>& pts, Rgb colour, float alpha);
    void outline(const std::vector<P2>& pts, Rgb colour, float thickness);
    void line(P2 a, P2 b, Rgb colour, float thickness, float alpha = 1.0f);
    void disc(P2 c, float radius, Rgb colour, float alpha = 1.0f);
    void blend(int x, int y, Rgb colour, float alpha);
    void text(const std::string& s, int x, int y, int scale, Rgb colour);
    std::vector<Vec3> outlineOf(const Surface& s) const;

    Pose                 mEye;
    int                  mWidth;
    int                  mHeight;
    float                mTanH;
    float                mTanV;
    std::vector<uint8_t> mRgb;
    int                  mCaptionLine = 0;
};

}  // namespace xri

#endif  // PIXELPILOT_XRI_SNAPSHOT_H
