// The XR pointer without a headset: scripted controllers and hands against the real geometry.
//
// Each test is a situation from the goggles - a near miss on the map, a drag from a hand held
// at chest height, both hands on a transmitter - and checks what the pilot would see happen.
// The snapshot tests write PNGs of the eye's view to $XRI_SNAPSHOT_DIR (default: the working
// directory), which is the quickest way to see where everything actually sits.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../XrInteraction.h"
#include "Snapshot.h"

using namespace xri;

namespace
{

constexpr float kDt = 1.0f / 72.0f;

// A Touch controller held naturally: chest height, right of centre, a little forward.
const Vec3 kRightHand{0.18f, -0.38f, -0.25f};
const Vec3 kLeftHand{-0.18f, -0.38f, -0.25f};

/** The video as it was set up on the headset on 2026-10-12. */
Video hisVideo()
{
    Video v;
    v.distance = 1.8377725f;
    v.width    = 2.2f;
    v.aspect   = 1080.0f / 1920.0f;
    v.height   = -0.30477035f;
    return v;
}

/** Looking from `from` towards `at`: -Z of the pose points at it. */
Pose aimAt(Vec3 from, Vec3 at)
{
    Pose p;
    p.position    = from;
    p.orientation = facingAlong(from - at);
    return p;
}

Pose aimAlong(Vec3 from, Vec3 dir)
{
    return aimAt(from, from + dir);
}

HandInput controller(Pose aim, bool select, bool touching = true)
{
    HandInput in;
    in.present  = true;
    in.isHand   = false;
    in.aim      = aim;
    in.select   = select;
    in.touching = touching;
    return in;
}

Vec3 directionOf(const Pose& p)
{
    return rotate(p.orientation, Vec3{0.0f, 0.0f, -1.0f});
}

/** One session: the default arrangement, head-locked, and whatever the hands do. */
struct Session
{
    Placement   layers[LAYER_COUNT];
    Video       video;
    Anchor      anchor;
    Settings    settings;
    Interaction ix;
    PointerOut  pointers[2];
    Events      events;

    Session() { defaultLayout(layers); }

    void frame(const HandInput& left, const HandInput& right, float sx = 0.0f, float sy = 0.0f)
    {
        const HandInput hands[2] = {left, right};
        ix.update(hands, sx, sy, kDt, settings, anchor, anchor.eye, layers, LAYER_COUNT, &video,
                  pointers, &events);
    }

    Surface surface(int layer) const { return panelSurface(layers[layer], anchor, video); }
};

std::string snapshotPath(const std::string& name)
{
    const char* dir = std::getenv("XRI_SNAPSHOT_DIR");
    return std::string(dir != nullptr ? dir : ".") + "/" + name;
}

void saveSnapshot(const Session& s, const std::string& name,
                  std::initializer_list<std::string> captions)
{
    Pose eye;
    eye.position    = s.anchor.eye;
    eye.orientation = s.anchor.facing;
    Snapshot shot(eye);
    shot.scene(s.layers, LAYER_COUNT, s.anchor, s.video, s.ix.hovered(), s.ix.held());
    shot.pointer(s.pointers[0]);
    shot.pointer(s.pointers[1]);
    for (const std::string& c : captions) shot.caption(c);
    ASSERT_TRUE(shot.save(snapshotPath(name))) << "could not write " << snapshotPath(name);
}

float pitchFromEye(Vec3 p)
{
    float yaw   = 0.0f;
    float pitch = 0.0f;
    yawPitchOf(p, &yaw, &pitch);
    return pitch;
}

}  // namespace

// ---------------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------------

TEST(Geometry, CylinderSurfaceSitsAtItsDistanceAndItsAxisThroughTheEye)
{
    Session       s;
    const Surface dash   = s.surface(LAYER_DASHBOARD);
    const Vec3    origin = arrangementOrigin(s.anchor, s.video);
    EXPECT_NEAR(length(dash.pose.position - origin), 1.40f, 1e-4f);
    // Untilted, the cylinder wraps around the pilot: its centre is the arrangement's origin.
    EXPECT_NEAR(length(dash.axisPose.position - origin), 0.0f, 1e-4f);
    EXPECT_NEAR(dash.width, 1.40f * 15.0f * kDeg2Rad, 1e-4f);
}

TEST(Geometry, SurfacePointsLieOnTheirSurface)
{
    Session s;
    for (int layer : {LAYER_DASHBOARD, LAYER_MINIMAP, LAYER_CHART})
    {
        const Surface surf = s.surface(layer);
        const Vec3    p    = surfacePoint(surf, surf.width * 0.3f, -surf.height * 0.2f);
        // A ray from the eye through that point has to hit it at that spot.
        Hit hit;
        ASSERT_TRUE(intersect(surf, s.anchor.eye, normalize(p - s.anchor.eye), 0.0f, &hit))
            << layerName(layer);
        EXPECT_NEAR(length(hit.point - p), 0.0f, 1e-3f) << layerName(layer);
        EXPECT_NEAR(hit.u, surf.width * 0.3f, 1e-3f) << layerName(layer);
        EXPECT_NEAR(hit.v, -surf.height * 0.2f, 1e-3f) << layerName(layer);
    }
}

TEST(Geometry, WithHisSettingsThePanelsSitFortyDegreesDown)
{
    // Head-locked, with the video lowered 30 cm: the arrangement goes down with it, and the
    // instruments end up at the bottom edge of the lenses - where they are hard to read and
    // harder to point at. This pins the number so a layout change has to face it.
    Session s;
    s.video = hisVideo();
    EXPECT_NEAR(pitchFromEye(s.surface(LAYER_DASHBOARD).pose.position - s.anchor.eye), -39.7f,
                0.2f);
    EXPECT_NEAR(pitchFromEye(s.surface(LAYER_MINIMAP).pose.position - s.anchor.eye), -40.7f,
                0.2f);
}

TEST(Geometry, WorldLockedPanelsOrbitTheEyeNotTheScreen)
{
    Session s;
    s.anchor.eye    = {0.3f, 1.6f, -0.2f};
    s.anchor.facing = aboutY(30.0f * kDeg2Rad);
    s.video.distance = 2.5f;
    const Vec3 origin = arrangementOrigin(s.anchor, s.video);
    const Vec3 before = s.surface(LAYER_MINIMAP).pose.position;
    EXPECT_NEAR(length(before - origin), 1.25f, 1e-4f);
    // Pushing the screen away does not take the instruments with it.
    s.video.distance = 3.5f;
    EXPECT_NEAR(length(s.surface(LAYER_MINIMAP).pose.position - before), 0.0f, 1e-5f);
}

// ---------------------------------------------------------------------------------
// picking
// ---------------------------------------------------------------------------------

TEST(Pick, RaysFromTheEyeHitWhatTheyAimAt)
{
    Session s;
    for (int layer : {LAYER_DASHBOARD, LAYER_MINIMAP, LAYER_CHART})
    {
        const Vec3 at  = s.surface(layer).pose.position;
        const Hit  hit = pick(s.anchor.eye, normalize(at - s.anchor.eye), s.layers, LAYER_COUNT,
                              s.anchor, s.video, true, kGrabPad);
        EXPECT_EQ(hit.target, layer) << layerName(layer);
    }
    // The middle of the picture is the video - the symbology on it is not a separate target.
    const Hit centre = pick(s.anchor.eye, Vec3{0.0f, 0.0f, -1.0f}, s.layers, LAYER_COUNT,
                            s.anchor, s.video, true, kGrabPad);
    EXPECT_EQ(centre.target, TARGET_VIDEO);
    const Hit up = pick(s.anchor.eye, Vec3{0.0f, 1.0f, -0.2f}, s.layers, LAYER_COUNT, s.anchor,
                        s.video, true, kGrabPad);
    EXPECT_EQ(up.target, TARGET_NONE);
}

TEST(Pick, APanelOverTheVideoWinsEvenWhenTheVideoIsNearer)
{
    Session s;
    s.video.distance              = 1.0f;
    s.layers[LAYER_CHART].yawDeg  = 0.0f;
    s.layers[LAYER_CHART].pitchDeg = 0.0f;
    const Hit hit = pick(s.anchor.eye, Vec3{0.0f, 0.0f, -1.0f}, s.layers, LAYER_COUNT, s.anchor,
                         s.video, true, kGrabPad);
    EXPECT_EQ(hit.target, LAYER_CHART);
}

TEST(Pick, TheDashboardIsHitOnItsCurveAndNotBeyondIt)
{
    Session       s;
    const Surface dash = s.surface(LAYER_DASHBOARD);
    const auto    onArc = [&](float deg) {
        const float a = deg * kDeg2Rad;
        return dash.axisPose.position +
               rotate(dash.axisPose.orientation,
                      Vec3{dash.radius * std::sin(a), 0.0f, -dash.radius * std::cos(a)});
    };
    Hit hit;
    EXPECT_TRUE(intersect(dash, s.anchor.eye, normalize(onArc(7.0f) - s.anchor.eye), 0.0f, &hit));
    EXPECT_NEAR(hit.t, 1.40f, 1e-3f);
    EXPECT_FALSE(intersect(dash, s.anchor.eye, normalize(onArc(10.0f) - s.anchor.eye), kGrabPad,
                           &hit));
}

// ---------------------------------------------------------------------------------
// dragging
// ---------------------------------------------------------------------------------

TEST(Drag, TheGrabbedPointStaysUnderTheBeamFromAHandAtChestHeight)
{
    Session s;
    s.video = hisVideo();
    // Off the panel's centre on purpose: that is where the old angle-only drag slid away.
    const Surface map0   = s.surface(LAYER_MINIMAP);
    const Vec3    target = surfacePoint(map0, 0.09f, 0.06f);
    const Vec3    dir0   = normalize(target - kRightHand);
    s.frame({}, controller(aimAt(kRightHand, target), true));
    ASSERT_EQ(s.ix.held(), LAYER_MINIMAP);
    ASSERT_TRUE(s.events.grabbed);

    // Up and to the right, in sixty frames: a sweep of 35 degrees across and 25 up.
    for (int k = 1; k <= 60; ++k)
    {
        const float f   = (float) k / 60.0f;
        const Vec3  dir = rotate(mul(aboutY(-35.0f * f * kDeg2Rad), aboutX(25.0f * f * kDeg2Rad)), dir0);
        s.frame({}, controller(aimAlong(kRightHand, dir), true));
        Hit hit;
        ASSERT_TRUE(intersect(s.surface(LAYER_MINIMAP), kRightHand, dir, 0.5f, &hit)) << k;
        EXPECT_NEAR(hit.u, 0.09f, 0.004f) << "frame " << k;
        EXPECT_NEAR(hit.v, 0.06f, 0.004f) << "frame " << k;
    }
    EXPECT_GT(s.layers[LAYER_MINIMAP].yawDeg, -15.0f);
    EXPECT_GT(s.layers[LAYER_MINIMAP].pitchDeg, -20.0f);
}

TEST(Drag, TheDashboardFollowsAlongItsCurve)
{
    Session       s;
    const Surface dash0  = s.surface(LAYER_DASHBOARD);
    const Vec3    target = surfacePoint(dash0, -0.10f, 0.05f);
    const Vec3    dir0   = normalize(target - kLeftHand);
    s.frame(controller(aimAt(kLeftHand, target), true), {});
    ASSERT_EQ(s.ix.held(), LAYER_DASHBOARD);
    for (int k = 1; k <= 40; ++k)
    {
        const float f   = (float) k / 40.0f;
        const Vec3  dir = rotate(aboutX(20.0f * f * kDeg2Rad), dir0);
        s.frame(controller(aimAlong(kLeftHand, dir), true), {});
        Hit hit;
        ASSERT_TRUE(intersect(s.surface(LAYER_DASHBOARD), kLeftHand, dir, 0.5f, &hit)) << k;
        EXPECT_NEAR(hit.u, -0.10f, 0.004f) << "frame " << k;
        EXPECT_NEAR(hit.v, 0.05f, 0.004f) << "frame " << k;
    }
}

TEST(Drag, ANearMissPicksNothingUpAndLeavesTheVideoAlone)
{
    Session s;
    // Six centimetres past the map's left edge, where nothing else is behind it either.
    const Surface map    = s.surface(LAYER_MINIMAP);
    const Vec3    beside = surfacePoint(map, -map.width * 0.5f - 0.06f, 0.0f);
    const Video   before = s.video;
    s.frame({}, controller(aimAt(kRightHand, beside), true));
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
    EXPECT_FALSE(s.events.grabbed);
    for (int k = 0; k < 72; ++k) s.frame({}, controller(aimAt(kRightHand, beside), true));
    EXPECT_FLOAT_EQ(s.video.distance, before.distance);
    EXPECT_FLOAT_EQ(s.video.height, before.height);
}

TEST(Drag, SweepingAHeldTriggerAcrossAPanelPicksNothingUp)
{
    Session    s;
    const Vec3 nowhere = kRightHand + Vec3{0.0f, 1.0f, -0.3f};
    s.frame({}, controller(aimAt(kRightHand, nowhere), true));
    s.frame({}, controller(aimAt(kRightHand, s.surface(LAYER_CHART).pose.position), true));
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
}

TEST(Drag, TheVideoFollowsTheBeamUpAndDown)
{
    Session       s;
    const Surface screen = videoSurface(s.anchor, s.video);
    const Vec3    target = surfacePoint(screen, 0.4f, -0.2f);
    const Vec3    dir0   = normalize(target - kRightHand);
    s.frame({}, controller(aimAt(kRightHand, target), true));
    ASSERT_EQ(s.ix.held(), TARGET_VIDEO);
    const float heightBefore = s.video.height;
    for (int k = 1; k <= 30; ++k)
    {
        const Vec3 dir = rotate(aboutX(8.0f * k / 30.0f * kDeg2Rad), dir0);
        s.frame({}, controller(aimAlong(kRightHand, dir), true));
        Hit hit;
        ASSERT_TRUE(intersect(videoSurface(s.anchor, s.video), kRightHand, dir, 0.5f, &hit));
        EXPECT_NEAR(hit.v, -0.2f, 0.003f) << "frame " << k;
    }
    EXPECT_GT(s.video.height, heightBefore + 0.15f);
    EXPECT_TRUE(s.events.videoChanged);
}

TEST(Drag, TheStickPushesWhatIsHeldAwayAndSizesIt)
{
    Session    s;
    const Vec3 at = s.surface(LAYER_CHART).pose.position;
    s.frame({}, controller(aimAt(kRightHand, at), true));
    ASSERT_EQ(s.ix.held(), LAYER_CHART);
    for (int k = 0; k < 36; ++k) s.frame({}, controller(aimAt(kRightHand, at), true), 0.0f, 1.0f);
    // Half a second of full forward stick at 0.9 m/s.
    EXPECT_NEAR(s.layers[LAYER_CHART].distance, 1.25f + 0.45f, 0.02f);
    for (int k = 0; k < 36; ++k) s.frame({}, controller(aimAt(kRightHand, at), true), 1.0f, 0.0f);
    EXPECT_NEAR(s.layers[LAYER_CHART].widthM, 0.54f + 0.70f, 0.02f);
    // The video was not touched by any of it.
    EXPECT_FLOAT_EQ(s.video.distance, Video{}.distance);
}

TEST(Drag, ReleasingOrLosingTrackingLetsGo)
{
    Session    s;
    const Vec3 at = s.surface(LAYER_CHART).pose.position;
    s.frame({}, controller(aimAt(kRightHand, at), true));
    ASSERT_EQ(s.ix.held(), LAYER_CHART);
    s.frame({}, controller(aimAt(kRightHand, at), false));
    EXPECT_TRUE(s.events.released);
    EXPECT_EQ(s.events.releaseTarget, LAYER_CHART);
    EXPECT_EQ(s.ix.held(), TARGET_NONE);

    s.frame({}, controller(aimAt(kRightHand, at), true));
    ASSERT_EQ(s.ix.held(), LAYER_CHART);
    HandInput gone;  // not present
    s.frame({}, gone);
    EXPECT_TRUE(s.events.released);
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
}

TEST(Drag, OffMeansOff)
{
    Session s;
    s.settings.pointing = false;
    s.frame({}, controller(aimAt(kRightHand, s.surface(LAYER_CHART).pose.position), true));
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
    EXPECT_FALSE(s.pointers[1].beam);
}

// ---------------------------------------------------------------------------------
// the pointer
// ---------------------------------------------------------------------------------

TEST(Pointer, AControllerLyingOnTheTableDrawsNothing)
{
    Session s;
    s.frame({}, controller(aimAt(kRightHand, s.surface(LAYER_CHART).pose.position), false, false));
    EXPECT_FALSE(s.pointers[1].beam);
    EXPECT_FALSE(s.pointers[1].cursor);
    EXPECT_EQ(s.events.haptic[1].ms, 0);
}

TEST(Pointer, TheCursorSitsOnThePanelItPointsAt)
{
    Session       s;
    const Surface map = s.surface(LAYER_MINIMAP);
    s.frame({}, controller(aimAt(kRightHand, map.pose.position), false));
    const PointerOut& p = s.pointers[1];
    ASSERT_TRUE(p.beam);
    ASSERT_TRUE(p.cursor);
    EXPECT_EQ(p.cursorFrame, CURSOR_HOVER);
    EXPECT_EQ(p.target, LAYER_MINIMAP);
    EXPECT_NEAR(length(p.cursorPose.position - map.pose.position), 0.004f, 1e-3f);
    EXPECT_EQ(s.events.hovered, LAYER_MINIMAP);
}

TEST(Pointer, OneTickWhenTheBeamSlidesOntoAPanel)
{
    Session       s;
    const Surface chart = s.surface(LAYER_CHART);
    // Up from below it, where there is nothing else to slide across first.
    const Vec3    from  = surfacePoint(chart, 0.0f, -chart.height * 0.5f - 0.15f);
    const Vec3    to    = surfacePoint(chart, 0.0f, 0.0f);
    int ticks = 0;
    for (int k = 0; k <= 40; ++k)
    {
        const float f = (float) k / 40.0f;
        s.frame({}, controller(aimAt(kRightHand, from + (to - from) * f), false));
        if (s.events.haptic[1].ms > 0) ++ticks;
    }
    EXPECT_EQ(ticks, 1);
}

TEST(Pointer, ABeamThatHitsNothingIsShortAndHasNoCursor)
{
    Session s;
    s.frame({}, controller(aimAlong(kRightHand, Vec3{0.0f, 1.0f, 0.0f}), false));
    ASSERT_TRUE(s.pointers[1].beam);
    EXPECT_FALSE(s.pointers[1].cursor);
    EXPECT_NEAR(length(s.pointers[1].to - kRightHand), kBeamMissLength, 1e-3f);
}

// ---------------------------------------------------------------------------------
// hands
// ---------------------------------------------------------------------------------

TEST(Hands, BothHandsOnATransmitterAreNotInput)
{
    HeadPose head{{0.0f, 1.70f, 0.0f}, {0.0f, -0.2f, -1.0f}};
    // Thumbs on the gimbals: low and close together, pointing forward and down.
    HandPose left{true, {-0.09f, 1.22f, -0.30f}, {0.0f, -0.4f, -1.0f}};
    HandPose right{true, {0.09f, 1.22f, -0.30f}, {0.0f, -0.4f, -1.0f}};
    EXPECT_EQ(handBlock(left, right, head), HAND_LOW);
    EXPECT_EQ(handBlock(right, left, head), HAND_LOW);
    // Raised to the shoulders but still together.
    left.position.y  = 1.45f;
    right.position.y = 1.45f;
    EXPECT_EQ(handBlock(right, left, head), HAND_TOGETHER);
}

TEST(Hands, WhatTheHeadsetMeasuredIsBlocked)
{
    // 2026-10-12: a free left hand reported pinches resting 0.37 and 0.45 m below the eyes,
    // and at the headset 0.12 m above them.
    HeadPose head{{0.0f, 1.70f, 0.0f}, {0.0f, -0.2f, -1.0f}};
    HandPose none;
    HandPose resting{true, {-0.25f, 1.70f - 0.37f, -0.25f}, {0.0f, -0.5f, -1.0f}};
    EXPECT_EQ(handBlock(resting, none, head), HAND_LOW);
    resting.position.y = 1.70f - 0.45f;
    EXPECT_EQ(handBlock(resting, none, head), HAND_LOW);
    HandPose strap{true, {-0.12f, 1.70f + 0.12f, -0.05f}, {0.0f, 0.2f, -1.0f}};
    EXPECT_EQ(handBlock(strap, none, head), HAND_AT_HEAD);
    // Just in front of the face, at eye level less a little: still the headset.
    HandPose face{true, {0.05f, 1.62f, -0.12f}, {0.0f, 0.0f, -1.0f}};
    EXPECT_EQ(handBlock(face, none, head), HAND_AT_HEAD);
}

TEST(Hands, ARaisedFreeHandPointingAwayCounts)
{
    HeadPose head{{0.0f, 1.70f, 0.0f}, {0.0f, -0.2f, -1.0f}};
    // Raised to the shoulder and held out: the posture of someone who means it.
    HandPose right{true, {0.25f, 1.48f, -0.35f}, {0.1f, -0.5f, -1.0f}};
    HandPose left{true, {-0.20f, 1.05f, -0.10f}, {0.0f, -1.0f, 0.0f}};
    EXPECT_EQ(handBlock(right, left, head), HAND_FREE);
    // The palm turned back towards the face is the system gesture's posture.
    right.direction = {0.0f, 0.3f, 1.0f};
    EXPECT_EQ(handBlock(right, left, head), HAND_NOT_POINTING);
}

TEST(Hands, ABlockedHandCannotTakeHold)
{
    Session s;
    s.settings.handDrag = true;
    HandInput hand      = controller(aimAt(kRightHand, s.surface(LAYER_CHART).pose.position), true);
    hand.isHand         = true;
    hand.eligible       = false;
    s.frame({}, hand);
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
    EXPECT_FALSE(s.pointers[1].beam);
    // Free again - but the pinch has to start again too, not be carried over.
    hand.eligible = true;
    s.frame({}, hand);
    EXPECT_EQ(s.ix.held(), TARGET_NONE);
    hand.select = false;
    s.frame({}, hand);
    hand.select = true;
    s.frame({}, hand);
    EXPECT_EQ(s.ix.held(), LAYER_CHART);
}

TEST(Hands, AHeldGestureShowsARingFillingUp)
{
    Session   s;
    HandInput hand    = controller(aimAlong(kRightHand, Vec3{0.0f, -0.3f, -1.0f}), false, false);
    hand.isHand       = true;
    hand.holdProgress = 0.55f;
    s.frame({}, hand);
    const PointerOut& p = s.pointers[1];
    EXPECT_FALSE(p.beam);  // hand dragging is off; the ring is feedback, not a pointer
    ASSERT_TRUE(p.cursor);
    EXPECT_EQ(p.cursorFrame, CURSOR_PROGRESS_FIRST + 4);
}

// ---------------------------------------------------------------------------------
// pictures
// ---------------------------------------------------------------------------------

TEST(Snapshots, HisViewAsItIsToday)
{
    Session s;
    s.video = hisVideo();
    saveSnapshot(s, "01-his-view.png",
                 {"HEAD-LOCKED, HIS VIDEO SETTINGS", "VIDEO 1.84 M AWAY, 2.20 M WIDE, 0.30 M DOWN",
                  "GRID: 5 DEG, BRIGHT LINES EVERY 15"});
}

TEST(Snapshots, TheDefaultView)
{
    Session s;
    saveSnapshot(s, "02-default-view.png", {"HEAD-LOCKED, DEFAULT VIDEO 1.60 M / 2.20 M"});
}

TEST(Snapshots, PointingAndDragging)
{
    Session       s;
    s.video             = hisVideo();
    const Surface map0  = s.surface(LAYER_MINIMAP);
    const Vec3    grab  = surfacePoint(map0, 0.05f, 0.05f);
    s.frame({}, controller(aimAt(kRightHand, grab), false));
    saveSnapshot(s, "03-pointing-at-map.png", {"POINTING AT THE MAP: BEAM, CURSOR, OUTLINE"});

    const Vec3 dir0 = normalize(grab - kRightHand);
    s.frame({}, controller(aimAlong(kRightHand, dir0), true));
    for (int k = 1; k <= 45; ++k)
    {
        const float f   = (float) k / 45.0f;
        const Vec3  dir = rotate(mul(aboutY(-10.0f * f * kDeg2Rad), aboutX(22.0f * f * kDeg2Rad)), dir0);
        s.frame({}, controller(aimAlong(kRightHand, dir), true));
    }
    char line[96];
    std::snprintf(line, sizeof(line), "MAP NOW AT YAW %.0f PITCH %.0f", s.layers[LAYER_MINIMAP].yawDeg,
                  s.layers[LAYER_MINIMAP].pitchDeg);
    saveSnapshot(s, "04-dragging-map.png", {"HOLDING THE MAP, DRAGGED UP AND RIGHT", line});
}

TEST(Snapshots, HoldingTheVideo)
{
    Session       s;
    s.video              = hisVideo();
    const Surface screen = videoSurface(s.anchor, s.video);
    const Vec3    grab   = surfacePoint(screen, 0.3f, -0.25f);
    const Vec3    dir0   = normalize(grab - kRightHand);
    s.frame({}, controller(aimAlong(kRightHand, dir0), true));
    for (int k = 1; k <= 30; ++k)
    {
        s.frame({}, controller(aimAlong(kRightHand, rotate(aboutX(6.0f * k / 30.0f * kDeg2Rad), dir0)), true));
    }
    char line[96];
    std::snprintf(line, sizeof(line), "VIDEO HEIGHT %.2f M, PANELS CAME ALONG", s.video.height);
    saveSnapshot(s, "05-holding-video.png", {"HOLDING THE VIDEO, MOVED UP", line});
}

TEST(Snapshots, AHandGestureFillingUp)
{
    Session   s;
    HandInput hand    = controller(aimAlong(Vec3{0.20f, -0.20f, -0.30f}, Vec3{0.0f, 0.1f, -1.0f}), false, false);
    hand.isHand       = true;
    hand.holdProgress = 0.6f;
    s.frame({}, hand);
    saveSnapshot(s, "06-hand-gesture-ring.png", {"RIGHT HAND PINCH HELD 60%: RING IN FRONT OF THE HAND"});
}
