#include "DvrWriter.h"

#include <algorithm>
#include <cstring>

// The only translation unit with minimp4 in it: the header is also its implementation.
#include "minimp4.h"

struct DvrWriter::Mux
{
    MP4E_mux_t*       mux       = nullptr;
    mp4_h26x_writer_t video     = {};
    bool              videoInit = false;
};

DvrWriter::DvrWriter(WriteFn write, void* token, bool fragmented) : mFragmented(fragmented)
{
    MP4E_mux_t* mux = MP4E_open(0 /*sequential_mode*/, fragmented ? 1 : 0, token, write);
    if (mux == nullptr) return;
    mMux      = new Mux();
    mMux->mux = mux;
}

DvrWriter::~DvrWriter()
{
    close();
}

void DvrWriter::close()
{
    if (mMux == nullptr) return;
    // The last audio packet has nothing after it to measure against, so it gets its own length.
    if (mHaveHeld) writeHeldAudio((unsigned) mHeldSamples);
    MP4E_close(mMux->mux);
    if (mMux->videoInit) mp4_h26x_write_close(&mMux->video);
    delete mMux;
    mMux = nullptr;
}

void DvrWriter::startTracks(bool h265, int width, int height, bool withAudio)
{
    mp4_h26x_write_init(&mMux->video, mMux->mux, width, height, h265 ? 1 : 0);
    mMux->videoInit = true;
    if (withAudio)
    {
        MP4E_track_t tr;
        std::memset(&tr, 0, sizeof(tr));
        tr.track_media_kind       = e_audio;
        tr.language[0]            = 'u';
        tr.language[1]            = 'n';
        tr.language[2]            = 'd';
        tr.object_type_indication = MP4_OBJECT_TYPE_OPUS;
        tr.time_scale             = 48000;  // Opus in MP4 is always counted at 48 kHz
        // Mono: what the camera's microphone gives. A stereo stream still decodes, downmixed.
        tr.u.a.channelcount = 1;
        mAudioTrack         = MP4E_add_track(mMux->mux, &tr);
    }
    mStarted = true;
}

void DvrWriter::video(const uint8_t* nal, size_t size, bool h265, int width, int height, float fps,
                      bool withAudio, Clock::time_point at)
{
    if (mMux == nullptr || size < 5) return;
    if (!mStarted)
    {
        if (width <= 0 || height <= 0) return;  // not decoded far enough yet to know the size
        mFps = fps > 1.0f ? fps : 60.0f;
        startTracks(h265, width, height, withAudio);
    }

    const size_t start = (nal[0] == 0 && nal[1] == 0 && nal[2] == 1) ? 3 : 4;
    if (size <= start) return;
    const uint8_t head = nal[start];
    bool          frame;
    bool          parameters;
    if (h265)
    {
        const int type = (head >> 1) & 0x3f;
        frame          = type <= 31;
        parameters     = type >= 32 && type <= 34;
    }
    else
    {
        const int type = head & 0x1f;
        frame          = type >= 1 && type <= 5;
        parameters     = type == 7 || type == 8;
    }
    // Access unit delimiters, SEI and the like: nothing an MP4 needs, and minimp4 would make
    // every one of them a frame of its own, with a duration.
    if (!frame && !parameters) return;

    unsigned duration = 0;
    if (frame)
    {
        if (mHaveLastFrame)
        {
            // As long as it took for this frame to follow the last one: a frame the link lost
            // lengthens its neighbour instead of shortening the recording.
            const int64_t us =
                std::chrono::duration_cast<std::chrono::microseconds>(at - mLastFrameAt).count();
            duration = (unsigned) std::clamp<int64_t>(us * 9 / 100, 90000 / 240, 90000);
        }
        else
        {
            duration = (unsigned) (90000.0f / mFps);
        }
    }
    const int result = mp4_h26x_write_nal(&mMux->video, nal, (int) size, duration);
    // Frames before the first key frame are refused, and do not start the clock.
    if (frame && result == MP4E_STATUS_OK)
    {
        mLastFrameAt   = at;
        mHaveLastFrame = true;
        mVideoLive     = true;
        ++mVideoSamples;
    }
}

void DvrWriter::audio(const uint8_t* rtp, size_t size, Clock::time_point at)
{
    (void) at;
    // Not before the first frame is in: audio from before it would sit at the start of a
    // track whose picture only begins later, and play out of step for the whole recording.
    if (mMux == nullptr || mAudioTrack < 0 || !mVideoLive) return;
    size_t offset = 0;
    size_t length = 0;
    if (!rtpPayload(rtp, size, &offset, &length)) return;
    const int samples = opusSamples(rtp + offset, length);
    if (samples <= 0) return;
    const uint16_t seq = (uint16_t) ((rtp[2] << 8) | rtp[3]);

    if (mHaveHeld)
    {
        const uint16_t gap = (uint16_t) (seq - mHeldSeq);
        if (gap == 0 || gap > 0x8000) return;  // a duplicate, or late - its place is gone
        // The held packet lasted its own length plus every packet missing behind it - at most
        // fifteen seconds of them, so a restart of the camera's numbering cannot open a chasm.
        const unsigned missing =
            std::min<unsigned>(gap - 1u, 15u * 48000u / (unsigned) mHeldSamples);
        writeHeldAudio((unsigned) mHeldSamples * (1u + missing));
    }
    mHeldAudio.assign(rtp + offset, rtp + offset + length);
    mHeldSeq     = seq;
    mHeldSamples = samples;
    mHaveHeld    = true;
}

void DvrWriter::writeHeldAudio(unsigned duration)
{
    if (!mHaveHeld) return;
    mHaveHeld = false;
    if (MP4E_put_sample(mMux->mux, mAudioTrack, mHeldAudio.data(), (int) mHeldAudio.size(),
                        (int) duration, MP4E_SAMPLE_DEFAULT) == MP4E_STATUS_OK)
    {
        ++mAudioSamples;
    }
}

int DvrWriter::opusSamples(const uint8_t* p, size_t size)
{
    if (p == nullptr || size < 1) return 0;
    const int config = p[0] >> 3;
    int       frame;  // samples at 48 kHz
    if (config < 12)
    {
        static const int silk[4] = {480, 960, 1920, 2880};  // 10, 20, 40, 60 ms
        frame                    = silk[config & 3];
    }
    else if (config < 16)
    {
        frame = (config & 1) ? 960 : 480;  // hybrid: 10 or 20 ms
    }
    else
    {
        static const int celt[4] = {120, 240, 480, 960};  // 2.5, 5, 10, 20 ms
        frame                    = celt[config & 3];
    }
    int frames;
    switch (p[0] & 3)
    {
        case 0:
            frames = 1;
            break;
        case 1:
        case 2:
            frames = 2;
            break;
        default:
            if (size < 2) return 0;
            frames = p[1] & 0x3f;
            break;
    }
    const int total = frame * frames;
    return (total > 0 && total <= 5760) ? total : 0;  // a packet holds 120 ms at most
}

bool DvrWriter::rtpPayload(const uint8_t* d, size_t size, size_t* offset, size_t* length)
{
    if (d == nullptr || size < 12 || (d[0] >> 6) != 2) return false;
    size_t header = 12 + 4 * (size_t) (d[0] & 0x0F);
    if (d[0] & 0x10)
    {
        if (size < header + 4) return false;
        header += 4 + 4 * (((size_t) d[header + 2] << 8) | d[header + 3]);
    }
    if (size <= header) return false;
    size_t payload = size - header;
    if (d[0] & 0x20)
    {
        const uint8_t padding = d[size - 1];
        if (padding >= payload) return false;
        payload -= padding;
    }
    *offset = header;
    *length = payload;
    return true;
}
