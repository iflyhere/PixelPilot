// Pointing at, hovering over and moving the instrument panels and the video.
//
// Geometry and state only - no OpenXR, no Android. XrGoggleSession feeds it poses and button
// states once a frame and applies what comes back; the host tests in tests/ feed it scripted
// controllers and hands instead, which is what makes the interaction testable on a desktop.
//
// Everything the compositor draws and everything the pointer can hit is worked out by the same
// functions here. Two copies of that would let a panel be grabbable somewhere other than where
// it is drawn - which is exactly what the first cylinder did.
//
// All poses are in the layout space, the space the layers are submitted in: VIEW when the
// arrangement is head-locked, LOCAL when it is world-locked. Right-handed, +Y up, -Z forward,
// metres. Angles are radians in the maths and degrees in the placement table, because the table
// is what a person reads and edits.

#ifndef PIXELPILOT_XRINTERACTION_H
#define PIXELPILOT_XRINTERACTION_H

#include <cmath>

namespace xri
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Pose
{
    Quat orientation;
    Vec3 position;
};

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** a scaled to unit length; a zero vector stays zero. */
Vec3 normalize(Vec3 a);
Quat mul(Quat a, Quat b);
inline Quat conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
/** v rotated by q. */
Vec3 rotate(Quat q, Vec3 v);
Quat aboutX(float rad);
Quat aboutY(float rad);
/** The rotation whose columns are x, y and z, which have to be orthonormal and right-handed. */
Quat fromBasis(Vec3 x, Vec3 y, Vec3 z);
/** Any rotation that turns +Z into n. */
Quat facingAlong(Vec3 n);

/**
 * The yaw and pitch of a direction, in the convention of the placement table: yaw positive to
 * the right, pitch positive up, both zero looking straight down -Z.
 */
void yawPitchOf(Vec3 dir, float* yawDeg, float* pitchDeg);

// ---------------------------------------------------------------------------------
// the arrangement
// ---------------------------------------------------------------------------------

/** One instrument layer's place, in the units of the layout table. */
struct Placement
{
    float yawDeg          = 0.0f;  // positive to the right
    float pitchDeg        = 0.0f;  // positive up
    float tiltDeg         = 0.0f;  // about the layer's own X, to lay it back
    float distance        = 0.0f;  // metres from the origin; 0 = the video's distance
    float widthM          = 0.0f;  // metres; 0 = the video's width
    float aspect          = 0.0f;  // height / width; 0 = from the pixel size
    bool  cylinder        = false;
    float centralAngleDeg = 0.0f;  // cylinder only: the arc it wraps through
    int   pixelWidth      = 1;
    int   pixelHeight     = 1;
    // False for the symbology: a horizon and a reticle only mean anything sitting exactly on
    // the image, so it is not up for rearranging.
    bool grabbable = true;
    // Has a swapchain and is shown. A layer that is not there cannot be hit either.
    bool present = true;
};

constexpr float kMinYawDeg            = -80.0f;
constexpr float kMaxYawDeg            = 80.0f;
constexpr float kMinPitchDeg          = -70.0f;
constexpr float kMaxPitchDeg          = 45.0f;
constexpr float kMinTiltDeg           = -80.0f;
constexpr float kMaxTiltDeg           = 80.0f;
constexpr float kMinPanelDistance     = 0.6f;
constexpr float kMaxPanelDistance     = 4.0f;
constexpr float kMinPanelWidth        = 0.15f;
constexpr float kMaxPanelWidth        = 1.6f;
constexpr float kMinCentralAngleDeg   = 10.0f;
constexpr float kMaxCentralAngleDeg   = 80.0f;

/** The video panel, which the instruments are arranged around. */
struct Video
{
    float distance = 1.6f;          // metres in front of the eye
    float width    = 2.2f;          // metres
    float aspect   = 9.0f / 16.0f;  // height / width, from the stream
    float height   = 0.0f;          // metres up (+) or down (-) from eye level
};

constexpr float kMinVideoDistance = 0.4f;
constexpr float kMaxVideoDistance = 8.0f;
constexpr float kMinVideoWidth    = 0.4f;
constexpr float kMaxVideoWidth    = 12.0f;
constexpr float kMaxVideoHeight   = 1.5f;

/**
 * What the arrangement hangs from: the eye it was laid out around, and which way it faces.
 * Head-locked that is the origin of VIEW looking down -Z; world-locked it is where the head was
 * at the last recenter, turned to where it was looking.
 */
struct Anchor
{
    Vec3 eye;
    Quat facing;  // yaw only, so nothing ends up rolled
};

/** Where the instruments built into the app sit by default. Indices are the session's. */
enum Layer
{
    LAYER_SYMBOLOGY = 0,
    LAYER_DASHBOARD,
    LAYER_MINIMAP,
    LAYER_CHART,
    LAYER_COUNT
};
void defaultLayout(Placement layers[LAYER_COUNT]);
const char* layerName(int layer);

/** The point the panels orbit: the eye, raised or lowered with the video. */
Vec3 arrangementOrigin(const Anchor& anchor, const Video& video);

/** A layer's surface, as both the compositor and the hit test need it. */
struct Surface
{
    Pose  pose;            // centre of the visible surface; +Z faces back towards the viewer
    float width  = 0.0f;   // metres; for a cylinder the arc length
    float height = 0.0f;
    bool  cylinder     = false;
    float radius       = 0.0f;  // cylinder only
    float centralAngle = 0.0f;  // cylinder only, radians
    // Cylinder only: the centre of the cylinder - which is what XR_KHR_composition_layer_cylinder
    // takes as the layer's pose. Its surface is `radius` away along that pose's -Z.
    Pose axisPose;
};

Surface panelSurface(const Placement& p, const Anchor& anchor, const Video& video);
Surface videoSurface(const Anchor& anchor, const Video& video);

/** The point u metres across (along the arc, for a cylinder) and v metres up from the centre. */
Vec3 surfacePoint(const Surface& s, float u, float v);

// ---------------------------------------------------------------------------------
// hitting things
// ---------------------------------------------------------------------------------

/** Panels are 0..LAYER_COUNT-1; these are the rest. */
enum Target
{
    TARGET_NONE  = -1,
    TARGET_VIDEO = -2,
};

struct Hit
{
    int   target = TARGET_NONE;
    float t      = 0.0f;  // along the ray, metres
    Vec3  point;          // layout space
    Vec3  normal;         // facing back along the ray
    float u = 0.0f;       // on the surface, metres from its centre: along the width (or arc)
    float v = 0.0f;       // ... and up
};

/** One surface against a ray; pad is how much grace there is around the edge, in metres. */
bool intersect(const Surface& s, Vec3 origin, Vec3 dir, float pad, Hit* out);

/**
 * What a ray lands on: the first grabbable panel, else the video if asked for, else nothing.
 * Panels win over the video even when they are further away, because they are composited on
 * top of it - what the pointer can hit has to be what the pilot can see.
 */
Hit pick(Vec3 origin, Vec3 dir, const Placement* layers, int count, const Anchor& anchor,
         const Video& video, bool includeVideo, float pad);

// ---------------------------------------------------------------------------------
// hands
// ---------------------------------------------------------------------------------

/** A tracked hand in a gravity-aligned space (LOCAL), for deciding whether it is free. */
struct HandPose
{
    bool tracked = false;
    Vec3 position;
    Vec3 direction;  // where its aim ray points
};

struct HeadPose
{
    Vec3 position;
    Vec3 forward;
};

enum HandBlock
{
    HAND_FREE = 0,
    HAND_NOT_TRACKED,
    HAND_LOW,           // down where a transmitter is held, or where a hand rests
    HAND_AT_HEAD,       // at the headset: adjusting the strap, lifting the visor
    HAND_TOGETHER,      // close to the other hand, as on a transmitter
    HAND_NOT_POINTING,  // turned back towards the face, which is the system gesture
};

// Measured on the headset, 2026-10-12, with a controller in the right hand and the left hand
// free: in one minute the left hand reported 21 pinches nobody made. Eleven came from a
// resting hand 0.37 to 0.45 m below the eyes, five from a hand above eye level - at the
// headset. A hand that means it is raised to about shoulder height and held away from the
// face, which is where these bounds put the window.
constexpr float kHandMinRaise    = -0.30f;  // metres relative to the eyes: lower is resting
constexpr float kHandMaxRaise    = -0.05f;  // higher is at the headset
constexpr float kHandMinFromHead = 0.20f;   // metres, horizontally: closer is at the face
constexpr float kHandsMinApart   = 0.30f;   // metres between the two aim origins
constexpr float kHandMaxAimDeg   = 80.0f;   // between the aim ray and where the head looks

/** Whether a tracked hand is free to give input, rather than holding something. */
HandBlock handBlock(const HandPose& hand, const HandPose& other, const HeadPose& head);
const char* handBlockName(HandBlock block);

// ---------------------------------------------------------------------------------
// the pointer
// ---------------------------------------------------------------------------------

struct HandInput
{
    bool present  = false;  // an aim pose, tracked this frame
    bool isHand   = false;  // a tracked hand rather than a controller
    Pose aim;               // layout space
    bool select   = false;  // trigger on a controller, aim-activate pinch on a hand
    bool touching = false;  // a finger resting on the controller - controllers only
    bool eligible = true;   // hands: handBlock() said HAND_FREE; controllers: always
    float holdProgress = 0.0f;  // 0..1 while a held hand gesture is building, else 0
};

struct Settings
{
    bool pointing = true;   // controllers may point and drag
    bool handDrag = false;  // tracked hands may too
};

/** Frames of the cursor atlas, left to right. Keep in sync with XrPointerArt.java. */
enum CursorFrame
{
    CURSOR_IDLE           = 0,  // pointing at nothing that can be taken hold of
    CURSOR_HOVER          = 1,  // on something the trigger would take hold of
    CURSOR_HELD           = 2,  // holding it
    CURSOR_PROGRESS_FIRST = 3,  // a held hand gesture filling up, in eighths
    CURSOR_PROGRESS_STEPS = 8,
    CURSOR_FRAME_COUNT    = CURSOR_PROGRESS_FIRST + CURSOR_PROGRESS_STEPS,
};

struct PointerOut
{
    bool  beam = false;
    Vec3  from;
    Vec3  to;
    Pose  beamPose;          // a quad along the beam, turned to face the eye
    float beamLength = 0.0f;
    bool  cursor = false;
    Pose  cursorPose;
    float cursorSize  = 0.0f;
    int   cursorFrame = CURSOR_IDLE;
    int   target      = TARGET_NONE;  // what this hand points at
};

constexpr float kBeamWidth       = 0.006f;
constexpr float kBeamMissLength  = 0.6f;
constexpr float kGrabPad         = 0.02f;
constexpr float kStickDeadzone   = 0.30f;
constexpr float kDistancePerSec  = 0.9f;   // metres per second at full stick
constexpr float kWidthPerSec     = 1.4f;
constexpr float kHeightPerSec    = 0.7f;
constexpr float kAnglePerSec     = 20.0f;  // degrees of cylinder arc per second at full stick

struct Haptic
{
    float amplitude = 0.0f;
    int   ms        = 0;
};

struct Events
{
    bool   hoverChanged = false;
    int    hovered      = TARGET_NONE;  // the panel being pointed at, for its outline
    bool   grabbed      = false;        // a hold began this frame
    int    grabTarget   = TARGET_NONE;
    int    grabHand     = -1;
    bool   released     = false;        // ...or ended
    int    releaseTarget = TARGET_NONE;
    bool   videoChanged = false;        // distance, width or height moved this frame
    Haptic haptic[2];
};

class Interaction
{
  public:
    /**
     * One frame: lets go or takes hold, moves what is held, and says where the pointers go.
     *
     * stickX/stickY are the thumbstick (either controller): while something is held they size
     * and distance it, otherwise they move and size the video. eye is the viewer in layout
     * space, used only to turn the beams and cursors towards it.
     */
    void update(const HandInput hands[2], float stickX, float stickY, float dt,
                const Settings& settings, const Anchor& anchor, Vec3 eye, Placement* layers,
                int count, Video* video, PointerOut pointers[2], Events* events);

    int held() const { return mHeld; }
    int heldBy() const { return mHeldBy; }
    int hovered() const { return mHovered; }
    int targetOf(int hand) const { return (hand == 0 || hand == 1) ? mHoverOf[hand] : TARGET_NONE; }

    /** Drops any hold without an event, for when the layout is reset underneath it. */
    void reset();

  private:
    void grab(int hand, const Hit& hit, const Anchor& anchor, const Video& video,
              const Placement* layers, Events* events);
    void release(Events* events);
    void moveHeld(Vec3 origin, Vec3 dir, const Anchor& anchor, Placement* layers, Video* video,
                  Events* events);
    void applyStick(float stickX, float stickY, float dt, const Anchor& anchor,
                    Placement* layers, Video* video, Events* events);

    int   mHeld   = TARGET_NONE;
    int   mHeldBy = -1;
    // Panels: the point taken hold of, on the panel's own surface - so that point stays under
    // the beam, rather than the panel's middle snapping to it.
    float mGrabU = 0.0f;
    float mGrabV = 0.0f;
    // The video: how far above its centre it was taken hold of, and at what distance.
    float mVideoGrabV        = 0.0f;
    float mVideoGrabDistance = 0.0f;
    bool  mSelectWas[2] = {false, false};
    int   mHoverOf[2]   = {TARGET_NONE, TARGET_NONE};
    int   mHovered      = TARGET_NONE;
};

}  // namespace xri

#endif  // PIXELPILOT_XRINTERACTION_H
