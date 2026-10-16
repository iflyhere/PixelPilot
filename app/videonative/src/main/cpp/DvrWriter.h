// Writes a recording: the video's NAL units and the camera's Opus audio, into one MP4.
//
// Kept apart from VideoPlayer so it can be exercised on a desktop - see tests/DvrWriter_test.cpp -
// and so the one place that knows about minimp4 is this file's implementation.
//
// Timing is the part that matters. A frame's duration used to be a constant 1/fps, so every frame
// the link lost made the recording shorter than the flight, and a soundtrack laid next to it would
// have drifted further with every one. Here a frame lasts until the next one arrived, and an audio
// packet lasts its own length plus whatever packets went missing after it, by RTP sequence number.
// Both tracks then add up to the time that actually passed.

#ifndef PIXELPILOT_DVRWRITER_H
#define PIXELPILOT_DVRWRITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

class DvrWriter
{
  public:
    using Clock = std::chrono::steady_clock;
    /** Same contract as minimp4's: write size bytes at offset, return non-zero on failure. */
    using WriteFn = int (*)(int64_t offset, const void* buffer, size_t size, void* token);

    DvrWriter(WriteFn write, void* token, bool fragmented);
    ~DvrWriter();

    DvrWriter(const DvrWriter&)            = delete;
    DvrWriter& operator=(const DvrWriter&) = delete;

    bool ok() const { return mMux != nullptr; }

    /**
     * One NAL unit in Annex B form, start code included. The tracks are set up with the first
     * one that comes with a known size: the video, and the audio too if withAudio says audio is
     * arriving - in a fragmented file every track has to exist before the first sample.
     */
    void video(const uint8_t* nal, size_t size, bool h265, int width, int height, float fps,
               bool withAudio, Clock::time_point at);

    /** One RTP packet carrying Opus, header included, as it came off the link. */
    void audio(const uint8_t* rtp, size_t size, Clock::time_point at);

    /** Writes what is still held back and finishes the file. Safe to call twice. */
    void close();

    // What went in, for logs and tests.
    int videoSamples() const { return mVideoSamples; }
    int audioSamples() const { return mAudioSamples; }
    bool hasAudioTrack() const { return mAudioTrack >= 0; }

    /** Samples of one Opus packet at 48 kHz, from its TOC byte (RFC 6716, 3.1); 0 if malformed. */
    static int opusSamples(const uint8_t* packet, size_t size);

    /**
     * The payload of an RTP packet: past CSRCs and an extension, short of padding. False if
     * the lengths do not add up.
     */
    static bool rtpPayload(const uint8_t* rtp, size_t size, size_t* offset, size_t* length);

  private:
    void startTracks(bool h265, int width, int height, bool withAudio);
    void writeHeldAudio(unsigned duration);

    struct Mux;  // minimp4's state, kept out of this header
    Mux*  mMux        = nullptr;
    bool  mFragmented = false;
    bool  mStarted    = false;
    bool  mVideoLive  = false;  // the first key frame is in; audio only starts after it
    int   mAudioTrack = -1;
    float mFps        = 0.0f;
    Clock::time_point mLastFrameAt{};
    bool  mHaveLastFrame = false;

    // The audio packet waiting for the next one, which tells how long it really lasted.
    std::vector<uint8_t> mHeldAudio;
    uint16_t             mHeldSeq     = 0;
    int                  mHeldSamples = 0;
    bool                 mHaveHeld    = false;

    int mVideoSamples = 0;
    int mAudioSamples = 0;
};

#endif  // PIXELPILOT_DVRWRITER_H
