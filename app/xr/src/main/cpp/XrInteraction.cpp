#include "XrInteraction.h"

namespace xri
{

// ---------------------------------------------------------------------------------
// maths
// ---------------------------------------------------------------------------------

Vec3 normalize(Vec3 a)
{
    const float len = length(a);
    return len > 1e-9f ? a * (1.0f / len) : Vec3{};
}

Quat mul(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 rotate(Quat q, Vec3 v)
{
    // Written out rather than going through a matrix.
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

Quat aboutX(float rad)
{
    return {std::sin(rad * 0.5f), 0.0f, 0.0f, std::cos(rad * 0.5f)};
}

Quat aboutY(float rad)
{
    return {0.0f, std::sin(rad * 0.5f), 0.0f, std::cos(rad * 0.5f)};
}

Quat fromBasis(Vec3 x, Vec3 y, Vec3 z)
{
    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f)
    {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (m21 - m12) * s;
        q.y = (m02 - m20) * s;
        q.z = (m10 - m01) * s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    }
    else
    {
        const float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 0.0f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}

Quat facingAlong(Vec3 n)
{
    const Vec3 z  = normalize(n);
    const Vec3 up = std::fabs(z.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 x  = normalize(cross(up, z));
    const Vec3 y  = cross(z, x);
    return fromBasis(x, y, z);
}

void yawPitchOf(Vec3 d, float* yawDeg, float* pitchDeg)
{
    const float len = length(d);
    if (len < 1e-6f)
    {
        *yawDeg   = 0.0f;
        *pitchDeg = 0.0f;
        return;
    }
    *yawDeg   = std::atan2(d.x, -d.z) * kRad2Deg;
    *pitchDeg = std::asin(clampf(d.y / len, -1.0f, 1.0f)) * kRad2Deg;
}

// ---------------------------------------------------------------------------------
// the arrangement
// ---------------------------------------------------------------------------------

void defaultLayout(Placement layers[LAYER_COUNT])
{
    for (int i = 0; i < LAYER_COUNT; ++i)
    {
        layers[i] = Placement{};
    }

    // Deliberately no offsets: horizon and reticle only mean anything if they sit exactly
    // where the image sits.
    Placement& sym  = layers[LAYER_SYMBOLOGY];
    sym.pixelWidth  = 2048;
    sym.pixelHeight = 1152;
    sym.grabbable   = false;

    // Curved, so it wraps around the pilot: the curve is itself a depth cue. 15 degrees of arc
    // is what the first version showed - it asked for 30, but put the cylinder's centre where
    // its surface belongs, which doubled the distance and halved the angle.
    Placement& dash      = layers[LAYER_DASHBOARD];
    dash.pixelWidth      = 1536;
    dash.pixelHeight     = 1152;
    dash.pitchDeg        = -30.0f;
    dash.distance        = 1.40f;
    dash.cylinder        = true;
    dash.centralAngleDeg = 15.0f;
    dash.aspect          = 0.75f;  // height / width - three rows, not two

    Placement& map  = layers[LAYER_MINIMAP];
    map.pixelWidth  = 640;
    map.pixelHeight = 640;
    map.yawDeg      = -25.0f;
    map.pitchDeg    = -30.0f;
    map.tiltDeg     = 26.0f;  // laid back, like a nav screen on a dash
    map.distance    = 1.25f;
    map.widthM      = 0.42f;

    Placement& chart  = layers[LAYER_CHART];
    chart.pixelWidth  = 896;
    chart.pixelHeight = 560;
    chart.yawDeg      = 25.0f;
    chart.pitchDeg    = -30.0f;
    chart.tiltDeg     = 26.0f;
    chart.distance    = 1.25f;
    chart.widthM      = 0.54f;
}

const char* layerName(int layer)
{
    switch (layer)
    {
        case LAYER_SYMBOLOGY:
            return "symbology";
        case LAYER_DASHBOARD:
            return "dashboard";
        case LAYER_MINIMAP:
            return "minimap";
        case LAYER_CHART:
            return "chart";
        case TARGET_VIDEO:
            return "video";
        default:
            return "nothing";
    }
}

Vec3 arrangementOrigin(const Anchor& anchor, const Video& video)
{
    // The facing is yaw only, so straight up in the arrangement is straight up here too.
    return anchor.eye + Vec3{0.0f, video.height, 0.0f};
}

Surface panelSurface(const Placement& p, const Anchor& anchor, const Video& video)
{
    Surface     s;
    const float dist = p.distance > 0.0f ? p.distance : video.distance;
    // Direction first, then the panel is laid back in its own frame - so the tilt changes how
    // it faces without moving where it is. Yaw negated: a positive turn about +Y goes left,
    // and the table reads better with positive to the right.
    const Quat facing =
        mul(mul(anchor.facing, aboutY(-p.yawDeg * kDeg2Rad)), aboutX(p.pitchDeg * kDeg2Rad));
    s.pose.orientation = mul(facing, aboutX(p.tiltDeg * kDeg2Rad));
    s.pose.position    = arrangementOrigin(anchor, video) + rotate(facing, Vec3{0.0f, 0.0f, -dist});
    s.cylinder         = p.cylinder;
    if (p.cylinder)
    {
        s.radius       = dist;
        s.centralAngle = p.centralAngleDeg * kDeg2Rad;
        s.width        = dist * s.centralAngle;
        // The cylinder layer's pose is the centre of the cylinder, with the visible arc one
        // radius out along its -Z - not the middle of the arc. Untilted, that is the origin.
        s.axisPose.orientation = s.pose.orientation;
        s.axisPose.position =
            s.pose.position + rotate(s.pose.orientation, Vec3{0.0f, 0.0f, dist});
    }
    else
    {
        s.width = p.widthM > 0.0f ? p.widthM : video.width;
    }
    const float aspect =
        p.aspect > 0.0f ? p.aspect
                        : (p.pixelWidth > 0 ? (float) p.pixelHeight / (float) p.pixelWidth : 1.0f);
    s.height = s.width * aspect;
    return s;
}

Vec3 surfacePoint(const Surface& s, float u, float v)
{
    if (!s.cylinder)
    {
        return s.pose.position + rotate(s.pose.orientation, Vec3{u, v, 0.0f});
    }
    const float a = s.radius > 0.0f ? u / s.radius : 0.0f;
    return s.axisPose.position +
           rotate(s.axisPose.orientation,
                  Vec3{s.radius * std::sin(a), v, -s.radius * std::cos(a)});
}

Surface videoSurface(const Anchor& anchor, const Video& video)
{
    Surface s;
    s.pose.orientation = anchor.facing;
    s.pose.position =
        arrangementOrigin(anchor, video) + rotate(anchor.facing, Vec3{0.0f, 0.0f, -video.distance});
    s.width  = video.width;
    s.height = video.width * video.aspect;
    return s;
}

// ---------------------------------------------------------------------------------
// hitting things
// ---------------------------------------------------------------------------------

bool intersect(const Surface& s, Vec3 o, Vec3 d, float pad, Hit* out)
{
    // Not the hand's own surroundings: a beam starts a few centimetres out.
    constexpr float kMinT = 0.02f;

    if (!s.cylinder)
    {
        // Against the plane rather than comparing angles: a laid-back panel subtends less than
        // its size suggests, and an angular test would let you grab empty air above the map.
        const Vec3  n     = rotate(s.pose.orientation, Vec3{0.0f, 0.0f, 1.0f});
        const float denom = dot(d, n);
        if (std::fabs(denom) < 1e-4f) return false;  // running along the panel
        const float t = dot(s.pose.position - o, n) / denom;
        if (t <= kMinT) return false;
        const Vec3 p     = o + d * t;
        const Vec3 local = rotate(conj(s.pose.orientation), p - s.pose.position);
        if (std::fabs(local.x) > s.width * 0.5f + pad || std::fabs(local.y) > s.height * 0.5f + pad)
        {
            return false;
        }
        out->t      = t;
        out->point  = p;
        out->normal = denom < 0.0f ? n : n * -1.0f;
        out->u      = local.x;
        out->v      = local.y;
        return true;
    }

    // A cylinder, in its own frame: the axis is Y through the origin and the visible arc is
    // centred on -Z, one radius out.
    const Quat  inv = conj(s.axisPose.orientation);
    const Vec3  lo  = rotate(inv, o - s.axisPose.position);
    const Vec3  ld  = rotate(inv, d);
    const float a   = ld.x * ld.x + ld.z * ld.z;
    if (a < 1e-8f || s.radius <= 0.0f) return false;  // along the axis
    const float b    = lo.x * ld.x + lo.z * ld.z;
    const float c    = lo.x * lo.x + lo.z * lo.z - s.radius * s.radius;
    const float disc = b * b - a * c;
    if (disc < 0.0f) return false;
    const float root  = std::sqrt(disc);
    const float ts[2] = {(-b - root) / a, (-b + root) / a};
    for (float t : ts)
    {
        if (t <= kMinT) continue;
        const Vec3  p     = lo + ld * t;
        const float angle = std::atan2(p.x, -p.z);
        if (std::fabs(angle) > s.centralAngle * 0.5f + pad / s.radius) continue;
        if (std::fabs(p.y) > s.height * 0.5f + pad) continue;
        Vec3 n = rotate(s.axisPose.orientation, normalize(Vec3{-p.x, 0.0f, -p.z}));
        if (dot(n, d) > 0.0f) n = n * -1.0f;  // facing back along the ray
        out->t      = t;
        out->point  = o + d * t;
        out->normal = n;
        out->u      = angle * s.radius;
        out->v      = p.y;
        return true;
    }
    return false;
}

Hit pick(Vec3 o, Vec3 d, const Placement* layers, int count, const Anchor& anchor,
         const Video& video, bool includeVideo, float pad)
{
    Hit   best;
    float bestT = 1e9f;
    for (int i = 0; i < count; ++i)
    {
        const Placement& p = layers[i];
        if (!p.present || !p.grabbable) continue;
        Hit h;
        if (intersect(panelSurface(p, anchor, video), o, d, pad, &h) && h.t < bestT)
        {
            best        = h;
            best.target = i;
            bestT       = h.t;
        }
    }
    if (best.target == TARGET_NONE && includeVideo)
    {
        Hit h;
        if (intersect(videoSurface(anchor, video), o, d, 0.0f, &h))
        {
            best        = h;
            best.target = TARGET_VIDEO;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------
// hands
// ---------------------------------------------------------------------------------

HandBlock handBlock(const HandPose& hand, const HandPose& other, const HeadPose& head)
{
    if (!hand.tracked) return HAND_NOT_TRACKED;
    const float rise = hand.position.y - head.position.y;
    if (rise < kHandMinRaise) return HAND_LOW;
    const float awayX = hand.position.x - head.position.x;
    const float awayZ = hand.position.z - head.position.z;
    if (rise > kHandMaxRaise || std::sqrt(awayX * awayX + awayZ * awayZ) < kHandMinFromHead)
    {
        return HAND_AT_HEAD;
    }
    if (other.tracked && length(hand.position - other.position) < kHandsMinApart)
    {
        return HAND_TOGETHER;
    }
    if (dot(normalize(hand.direction), normalize(head.forward)) <
        std::cos(kHandMaxAimDeg * kDeg2Rad))
    {
        return HAND_NOT_POINTING;
    }
    return HAND_FREE;
}

const char* handBlockName(HandBlock block)
{
    switch (block)
    {
        case HAND_FREE:
            return "free";
        case HAND_NOT_TRACKED:
            return "not tracked";
        case HAND_LOW:
            return "low";
        case HAND_AT_HEAD:
            return "at the headset";
        case HAND_TOGETHER:
            return "together";
        case HAND_NOT_POINTING:
            return "not pointing";
    }
    return "?";
}

// ---------------------------------------------------------------------------------
// the pointer
// ---------------------------------------------------------------------------------

namespace
{

Vec3 aimDirection(const Pose& aim)
{
    return normalize(rotate(aim.orientation, Vec3{0.0f, 0.0f, -1.0f}));
}

bool mayPoint(const HandInput& in, const Settings& settings)
{
    if (!in.present) return false;
    return in.isHand ? (settings.handDrag && in.eligible) : settings.pointing;
}

/** A quad along the beam, its face turned towards the eye as far as the beam allows. */
void placeBeam(PointerOut* p, Vec3 eye)
{
    const Vec3 along = p->to - p->from;
    p->beamLength    = length(along);
    const Vec3 y     = normalize(along);
    const Vec3 mid   = (p->from + p->to) * 0.5f;
    const Vec3 toEye = eye - mid;
    Vec3       z     = toEye - y * dot(toEye, y);
    if (length(z) < 1e-4f)
    {
        z = std::fabs(y.y) < 0.95f ? cross(y, Vec3{0.0f, 1.0f, 0.0f}) : cross(y, Vec3{1.0f, 0.0f, 0.0f});
    }
    z                         = normalize(z);
    const Vec3 x              = cross(y, z);
    p->beamPose.orientation   = fromBasis(x, y, z);
    p->beamPose.position      = mid;
}

/** About one and a half degrees across, wherever it is. */
float cursorSizeAt(Vec3 point, Vec3 eye)
{
    return clampf(length(point - eye) * 0.026f, 0.012f, 0.08f);
}

}  // namespace

void Interaction::reset()
{
    mHeld       = TARGET_NONE;
    mHeldBy     = -1;
    mHovered    = TARGET_NONE;
    mHoverOf[0] = TARGET_NONE;
    mHoverOf[1] = TARGET_NONE;
}

void Interaction::grab(int hand, const Hit& hit, const Anchor& anchor, const Video& video,
                       const Placement* layers, Events* events)
{
    mHeld   = hit.target;
    mHeldBy = hand;
    if (hit.target == TARGET_VIDEO)
    {
        const Vec3 local    = rotate(conj(anchor.facing), hit.point - anchor.eye);
        mVideoGrabV         = local.y - video.height;
        mVideoGrabDistance  = video.distance;
    }
    else
    {
        // The hit is already in the panel's own coordinates. It may lie in the grace margin
        // just outside the edge, which is fine: it only has to be a point that moves with it.
        (void) layers;
        mGrabU = hit.u;
        mGrabV = hit.v;
    }
    events->grabbed    = true;
    events->grabTarget = mHeld;
    events->grabHand   = hand;
    events->haptic[hand] = Haptic{0.5f, 25};
}

void Interaction::release(Events* events)
{
    events->released      = true;
    events->releaseTarget = mHeld;
    if (mHeldBy == 0 || mHeldBy == 1) events->haptic[mHeldBy] = Haptic{0.25f, 20};
    mHeld   = TARGET_NONE;
    mHeldBy = -1;
}

void Interaction::moveHeld(Vec3 o, Vec3 d, const Anchor& anchor, Placement* layers, Video* video,
                           Events* events)
{
    if (mHeld == TARGET_VIDEO)
    {
        // Up and down only. The screen stays square to the pilot, so the beam is crossed with
        // the plane it stands in, and the point taken hold of stays where the beam crosses.
        const Quat inv = conj(anchor.facing);
        const Vec3 lo  = rotate(inv, o - anchor.eye);
        const Vec3 ld  = rotate(inv, d);
        if (ld.z > -0.05f) return;  // pointing too far off the screen to say where on it
        const float t = (-video->distance - lo.z) / ld.z;
        if (t <= 0.0f) return;
        const float y = lo.y + ld.y * t;
        if (std::fabs(video->distance - mVideoGrabDistance) > 1e-5f)
        {
            // The stick moved the screen this frame. Take hold again where the beam crosses
            // the new plane, rather than letting the new crossing height jump it.
            mVideoGrabV        = y - video->height;
            mVideoGrabDistance = video->distance;
            return;
        }
        const float h = clampf(y - mVideoGrabV, -kMaxVideoHeight, kMaxVideoHeight);
        if (h != video->height)
        {
            video->height        = h;
            events->videoChanged = true;
        }
        return;
    }

    // A panel. Turning it about the origin keeps the grabbed point at the same distance from
    // the origin, so the point can only be on a sphere: cross the beam with that sphere, and
    // turn the panel until the point sits where the beam crosses. Not just the beam's angles -
    // the hand is half a metre from the eye, and following only its direction would let the
    // panel slide out from under the beam.
    Placement& p        = layers[mHeld];
    const Vec3 origin   = arrangementOrigin(anchor, *video);
    const Quat toLayout = conj(anchor.facing);
    const Vec3 grabbed  = surfacePoint(panelSurface(p, anchor, *video), mGrabU, mGrabV);
    const float radius  = length(grabbed - origin);
    const Vec3  oc      = o - origin;
    const float b       = dot(oc, d);
    const float c       = dot(oc, oc) - radius * radius;
    const float disc    = b * b - c;
    if (disc < 0.0f) return;  // the hand is outside the sphere and the beam misses it
    const float t = -b + std::sqrt(disc);
    if (t <= 0.0f) return;
    float wantYaw   = 0.0f;
    float wantPitch = 0.0f;
    yawPitchOf(rotate(toLayout, o + d * t - origin), &wantYaw, &wantPitch);

    // A few rounds, because a point off the panel's centre does not move by exactly the
    // panel's angles once the pitch changes. Each round closes most of what is left.
    for (int round = 0; round < 4; ++round)
    {
        float haveYaw   = 0.0f;
        float havePitch = 0.0f;
        const Vec3 now  = surfacePoint(panelSurface(p, anchor, *video), mGrabU, mGrabV);
        yawPitchOf(rotate(toLayout, now - origin), &haveYaw, &havePitch);
        float dYaw = wantYaw - haveYaw;
        if (dYaw > 180.0f) dYaw -= 360.0f;
        if (dYaw < -180.0f) dYaw += 360.0f;
        const float dPitch = wantPitch - havePitch;
        // Clamped rather than free: a panel dragged behind the pilot or into the floor is
        // gone, with nothing left to point at to get it back.
        p.yawDeg   = clampf(p.yawDeg + dYaw, kMinYawDeg, kMaxYawDeg);
        p.pitchDeg = clampf(p.pitchDeg + dPitch, kMinPitchDeg, kMaxPitchDeg);
        if (std::fabs(dYaw) < 0.01f && std::fabs(dPitch) < 0.01f) break;
    }
}

void Interaction::applyStick(float sx, float sy, float dt, const Anchor& anchor,
                             Placement* layers, Video* video, Events* events)
{
    const bool useY = std::fabs(sy) > kStickDeadzone;
    const bool useX = std::fabs(sx) > kStickDeadzone;
    if (!useX && !useY) return;

    if (mHeld == TARGET_NONE)
    {
        // Nothing held: the stick moves the screen up and down and sizes it, as it always has.
        if (useY)
        {
            video->height =
                clampf(video->height + sy * kHeightPerSec * dt, -kMaxVideoHeight, kMaxVideoHeight);
        }
        if (useX)
        {
            video->width =
                clampf(video->width + sx * kWidthPerSec * dt, kMinVideoWidth, kMaxVideoWidth);
        }
        events->videoChanged = true;
        return;
    }

    // Something held: the stick acts on it. Forward pushes it away and back pulls it in, the
    // way every pointer on the headset does it; right makes it bigger.
    if (mHeld == TARGET_VIDEO)
    {
        if (useY)
        {
            video->distance = clampf(video->distance + sy * kDistancePerSec * dt,
                                     kMinVideoDistance, kMaxVideoDistance);
        }
        if (useX)
        {
            video->width =
                clampf(video->width + sx * kWidthPerSec * dt, kMinVideoWidth, kMaxVideoWidth);
        }
        events->videoChanged = true;
        return;
    }

    Placement& p = layers[mHeld];
    if (useY)
    {
        const float before = p.distance > 0.0f ? p.distance : video->distance;
        p.distance = clampf(before + sy * kDistancePerSec * dt, kMinPanelDistance,
                            kMaxPanelDistance);
    }
    if (useX)
    {
        if (p.cylinder)
        {
            // A cylinder's size is the angle it wraps through, not a width.
            p.centralAngleDeg = clampf(p.centralAngleDeg + sx * kAnglePerSec * dt,
                                       kMinCentralAngleDeg, kMaxCentralAngleDeg);
        }
        else
        {
            const float now = panelSurface(p, anchor, *video).width;
            p.widthM = clampf(now + sx * kWidthPerSec * dt, kMinPanelWidth, kMaxPanelWidth);
        }
    }
}

void Interaction::update(const HandInput hands[2], float stickX, float stickY, float dt,
                         const Settings& settings, const Anchor& anchor, Vec3 eye,
                         Placement* layers, int count, Video* video, PointerOut pointers[2],
                         Events* events)
{
    *events = Events{};

    // Letting go: the select came up, or the hand stopped being tracked. A dropout has to let
    // go too, or the panel stays stuck to a pointer that no longer exists.
    if (mHeld != TARGET_NONE)
    {
        const HandInput& in = hands[mHeldBy];
        if (!in.present || !in.select) release(events);
    }

    // Taking hold: on the press, not while held - sweeping a held trigger across a panel picks
    // nothing up. One thing at a time, by whichever hand pressed first.
    for (int i = 0; i < 2; ++i)
    {
        const HandInput& in   = hands[i];
        const bool       edge = in.select && !mSelectWas[i];
        mSelectWas[i]         = in.select;
        if (!edge || mHeld != TARGET_NONE || !mayPoint(in, settings)) continue;
        const Hit hit = pick(in.aim.position, aimDirection(in.aim), layers, count, anchor, *video,
                             true, kGrabPad);
        // A press aimed at nothing does nothing. It used to pull the video nearer, which is
        // exactly what a near miss on a panel then did.
        if (hit.target != TARGET_NONE) grab(i, hit, anchor, *video, layers, events);
    }

    applyStick(stickX, stickY, dt, anchor, layers, video, events);

    if (mHeld != TARGET_NONE)
    {
        const HandInput& in = hands[mHeldBy];
        moveHeld(in.aim.position, aimDirection(in.aim), anchor, layers, video, events);
    }

    // The pointers, against the arrangement as it is after this frame's moves - so the cursor
    // is drawn on the panel where the panel is drawn.
    for (int i = 0; i < 2; ++i)
    {
        PointerOut& out = pointers[i];
        out             = PointerOut{};
        const HandInput& in = hands[i];
        if (!in.present)
        {
            mHoverOf[i] = TARGET_NONE;
            continue;
        }
        const bool holding = mHeld != TARGET_NONE && mHeldBy == i;
        // A controller points while a finger rests on it, so one lying on the table during a
        // flight draws nothing, outlines nothing and buzzes at nothing - head-locked, every
        // turn of the head sweeps its beam across the panels. A hand only gets this far once
        // it is up and free.
        const bool active = holding || (mayPoint(in, settings) &&
                                         (in.isHand || in.touching || in.select));
        const Vec3 o      = in.aim.position;
        const Vec3 d      = aimDirection(in.aim);

        Hit hit;
        if (holding)
        {
            // Wherever the beam crosses what it holds, with grace to spare: a panel stopped by
            // its clamps should still show the beam on it.
            const Surface s = mHeld == TARGET_VIDEO ? videoSurface(anchor, *video)
                                                    : panelSurface(layers[mHeld], anchor, *video);
            if (intersect(s, o, d, 1.0f, &hit)) hit.target = mHeld;
        }
        else if (active)
        {
            hit = pick(o, d, layers, count, anchor, *video, true, kGrabPad);
        }

        const int target = holding ? mHeld : (active ? hit.target : TARGET_NONE);
        // A tick when the beam slides onto something it could take hold of. Controllers only:
        // a hand has nothing to buzz.
        if (active && !holding && !in.isHand && target != TARGET_NONE && target != mHoverOf[i])
        {
            events->haptic[i] = Haptic{0.12f, 10};
        }
        mHoverOf[i] = target;
        out.target  = target;

        if (active)
        {
            out.beam = true;
            out.from = o + d * (in.isHand ? 0.06f : 0.03f);
            out.to   = hit.target != TARGET_NONE ? hit.point : o + d * kBeamMissLength;
            placeBeam(&out, eye);
            if (hit.target != TARGET_NONE)
            {
                out.cursor                  = true;
                out.cursorPose.position     = hit.point + hit.normal * 0.004f;
                out.cursorPose.orientation  = facingAlong(hit.normal);
                out.cursorSize              = cursorSizeAt(out.cursorPose.position, eye);
                out.cursorFrame             = holding ? CURSOR_HELD : CURSOR_HOVER;
            }
        }

        // A held hand gesture filling up: a ring in front of the hand, so a pilot can see that
        // the gesture was recognised - and let go - before it fires.
        if (in.isHand && in.holdProgress > 0.0f && !holding)
        {
            const Vec3 at              = o + d * 0.10f;
            out.cursor                 = true;
            out.cursorPose.position    = at;
            out.cursorPose.orientation = facingAlong(eye - at);
            out.cursorSize             = 0.035f;
            int step = (int) (clampf(in.holdProgress, 0.0f, 1.0f) * CURSOR_PROGRESS_STEPS);
            if (step >= CURSOR_PROGRESS_STEPS) step = CURSOR_PROGRESS_STEPS - 1;
            out.cursorFrame = CURSOR_PROGRESS_FIRST + step;
        }
    }

    // The panel to outline: the one held, else the one the right hand points at, else the left.
    int hovered = TARGET_NONE;
    if (mHeld >= 0)
    {
        hovered = mHeld;
    }
    else if (mHoverOf[1] >= 0)
    {
        hovered = mHoverOf[1];
    }
    else if (mHoverOf[0] >= 0)
    {
        hovered = mHoverOf[0];
    }
    if (hovered != mHovered)
    {
        mHovered               = hovered;
        events->hoverChanged   = true;
    }
    events->hovered = mHovered;
}

}  // namespace xri
