// The recorder on a desktop: a scripted flight's worth of H.265 and Opus goes in, and the MP4
// that comes out is taken apart box by box - tracks, the Opus sample entry, and above all how
// long each track says it lasts, against how long the "flight" did.
//
// The parameter sets are the real ones from the camera this was developed with (from its RTSP
// description), so minimp4 builds its hvcC from what a camera actually sends.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../DvrWriter.h"

namespace
{

using Clock = DvrWriter::Clock;

std::vector<uint8_t> hex(const char* s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2)
    {
        out.push_back((uint8_t) std::stoi(std::string(s + i, 2), nullptr, 16));
    }
    return out;
}

std::vector<uint8_t> annexB(const std::vector<uint8_t>& nal)
{
    std::vector<uint8_t> out = {0, 0, 0, 1};
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
}

const std::vector<uint8_t> kVps = annexB(hex("40010c01ffff01600000030000030000030000030096ac09"));
const std::vector<uint8_t> kSps =
    annexB(hex("42010101600000030000030000030000030096a003c08010e58dae49326b96700800001f48000754e040"));
const std::vector<uint8_t> kPps = annexB(hex("4401e076b02640"));

/** A slice: IDR_W_RADL (19) or TRAIL_R (1), with some payload. */
std::vector<uint8_t> slice(bool idr, size_t bytes = 900)
{
    std::vector<uint8_t> nal = {(uint8_t) ((idr ? 19 : 1) << 1), 0x01};
    for (size_t i = 0; i < bytes; ++i) nal.push_back((uint8_t) (0x40 + (i % 97)));
    return annexB(nal);
}

std::vector<uint8_t> aud()
{
    return annexB({(uint8_t) (35 << 1), 0x01, 0x50});
}

/** An RTP packet with one 20 ms CELT fullband Opus frame (TOC config 31, one frame). */
std::vector<uint8_t> opusRtp(uint16_t seq, uint32_t ts)
{
    std::vector<uint8_t> p = {0x80, 98, (uint8_t) (seq >> 8), (uint8_t) seq,
                              (uint8_t) (ts >> 24), (uint8_t) (ts >> 16), (uint8_t) (ts >> 8), (uint8_t) ts,
                              0x12, 0x34, 0x56, 0x78};
    p.push_back((uint8_t) (31 << 3));  // TOC: config 31, mono, code 0
    for (int i = 0; i < 60; ++i) p.push_back((uint8_t) (i * 7));
    return p;
}

/** What gets written, at the offsets minimp4 asks for. */
struct Sink
{
    std::vector<uint8_t> bytes;
    static int write(int64_t offset, const void* buffer, size_t size, void* token)
    {
        auto* s = static_cast<Sink*>(token);
        if (s->bytes.size() < (size_t) offset + size) s->bytes.resize((size_t) offset + size);
        std::memcpy(s->bytes.data() + offset, buffer, size);
        return 0;
    }
};

// --- a small MP4 reader, only as much as these checks need -------------------------------

uint32_t be32(const uint8_t* p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }

struct Box
{
    std::string    type;
    const uint8_t* body;
    size_t         size;
};

std::vector<Box> children(const uint8_t* p, size_t size)
{
    std::vector<Box> out;
    size_t           pos = 0;
    while (pos + 8 <= size)
    {
        const uint32_t len = be32(p + pos);
        if (len < 8 || pos + len > size) break;
        out.push_back({std::string((const char*) p + pos + 4, 4), p + pos + 8, len - 8});
        pos += len;
    }
    return out;
}

const Box* find(const std::vector<Box>& boxes, const char* type)
{
    for (const Box& b : boxes)
    {
        if (b.type == type) return &b;
    }
    return nullptr;
}

struct TrackInfo
{
    uint32_t    timescale = 0;
    std::string sampleEntry;
    std::vector<uint8_t> dOps;
    uint64_t    ticks   = 0;  // total duration in the track's timescale
    uint32_t    samples = 0;
    double seconds() const { return timescale ? (double) ticks / timescale : 0.0; }
};

/** Track ID -> what the file says about it, from moov and, if fragmented, every moof. */
std::map<uint32_t, TrackInfo> readTracks(const std::vector<uint8_t>& file)
{
    std::map<uint32_t, TrackInfo> tracks;
    const auto top  = children(file.data(), file.size());
    const Box* moov = find(top, "moov");
    if (moov == nullptr) return tracks;
    for (const Box& trak : children(moov->body, moov->size))
    {
        if (trak.type != "trak") continue;
        const auto tk   = children(trak.body, trak.size);
        const Box* tkhd = find(tk, "tkhd");
        const Box* mdia = find(tk, "mdia");
        if (!tkhd || !mdia) continue;
        const uint32_t id = be32(tkhd->body + (tkhd->body[0] == 1 ? 20 : 12));
        TrackInfo&     t  = tracks[id];
        const auto     md = children(mdia->body, mdia->size);
        if (const Box* mdhd = find(md, "mdhd")) t.timescale = be32(mdhd->body + (mdhd->body[0] == 1 ? 20 : 12));
        const Box* minf = find(md, "minf");
        const Box* stbl = minf ? find(children(minf->body, minf->size), "stbl") : nullptr;
        if (!stbl) continue;
        const auto st = children(stbl->body, stbl->size);
        if (const Box* stsd = find(st, "stsd"))
        {
            // full box (4) + entry count (4), then the one entry
            const auto entries = children(stsd->body + 8, stsd->size - 8);
            if (!entries.empty())
            {
                t.sampleEntry = entries[0].type;
                if (t.sampleEntry == "Opus")
                {
                    // AudioSampleEntry is 28 bytes, then its child boxes
                    const auto inner = children(entries[0].body + 28, entries[0].size - 28);
                    if (const Box* dops = find(inner, "dOps")) t.dOps.assign(dops->body, dops->body + dops->size);
                }
            }
        }
        if (const Box* stts = find(st, "stts"))
        {
            const uint32_t n = be32(stts->body + 4);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t count = be32(stts->body + 8 + i * 8);
                const uint32_t delta = be32(stts->body + 12 + i * 8);
                t.ticks += (uint64_t) count * delta;
                t.samples += count;
            }
        }
    }
    for (const Box& moof : top)
    {
        if (moof.type != "moof") continue;
        for (const Box& traf : children(moof.body, moof.size))
        {
            if (traf.type != "traf") continue;
            const auto tf   = children(traf.body, traf.size);
            const Box* tfhd = find(tf, "tfhd");
            const Box* trun = find(tf, "trun");
            if (!tfhd || !trun) continue;
            const uint32_t tfFlags = be32(tfhd->body) & 0xFFFFFF;
            const uint32_t id      = be32(tfhd->body + 4);
            size_t         pos     = 8;
            if (tfFlags & 0x01) pos += 8;  // base data offset
            if (tfFlags & 0x02) pos += 4;  // sample description index
            uint32_t defaultDuration = (tfFlags & 0x08) ? be32(tfhd->body + pos) : 0;
            const uint32_t trFlags = be32(trun->body) & 0xFFFFFF;
            const uint32_t count   = be32(trun->body + 4);
            size_t         tp      = 8;
            if (trFlags & 0x001) tp += 4;
            if (trFlags & 0x004) tp += 4;
            TrackInfo& t = tracks[id];
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t duration = defaultDuration;
                if (trFlags & 0x100)
                {
                    duration = be32(trun->body + tp);
                    tp += 4;
                }
                if (trFlags & 0x200) tp += 4;
                if (trFlags & 0x400) tp += 4;
                if (trFlags & 0x800) tp += 4;
                t.ticks += duration;
                ++t.samples;
            }
        }
    }
    return tracks;
}

/**
 * A scripted session: `seconds` of 60 fps video and 20 ms audio, with some of both lost on the
 * way, and an access unit delimiter before every frame as majestic may send.
 */
std::vector<uint8_t> record(bool fragmented, double seconds, int dropEveryNthFrame, int dropAudioRun,
                            bool withAudio = true)
{
    Sink      sink;
    DvrWriter w(&Sink::write, &sink, fragmented);
    EXPECT_TRUE(w.ok());
    const Clock::time_point t0 = Clock::now();
    const int               frames = (int) (seconds * 60);
    uint16_t                seq    = 1000;
    uint32_t                ts     = 0;
    int                     audioAt = 0;  // in 20 ms steps
    for (int f = 0; f < frames; ++f)
    {
        const auto at = t0 + std::chrono::microseconds((int64_t) f * 1000000 / 60);
        // audio up to this frame's time
        while (audioAt * 20000 <= (int64_t) f * 1000000 / 60)
        {
            const auto aAt  = t0 + std::chrono::microseconds((int64_t) audioAt * 20000);
            const bool lost = dropAudioRun > 0 && audioAt % 50 >= 10 && audioAt % 50 < 10 + dropAudioRun;
            if (!lost)
            {
                const auto pkt = opusRtp(seq, ts);
                w.audio(pkt.data(), pkt.size(), aAt);
            }
            ++seq;
            ts += 960;
            ++audioAt;
        }
        const bool keyframe = f % 60 == 0;
        if (dropEveryNthFrame > 0 && !keyframe && f % dropEveryNthFrame == 0) continue;
        const auto a = aud();
        w.video(a.data(), a.size(), true, 1920, 1080, 60.0f, withAudio, at);
        if (keyframe)
        {
            for (const auto* ps : {&kVps, &kSps, &kPps}) w.video(ps->data(), ps->size(), true, 1920, 1080, 60.0f, withAudio, at);
        }
        const auto s = slice(keyframe);
        w.video(s.data(), s.size(), true, 1920, 1080, 60.0f, withAudio, at);
    }
    w.close();
    return sink.bytes;
}

}  // namespace

TEST(DvrWriter, OpusPacketLengthsFromTheTocByte)
{
    const uint8_t celt20[] = {31 << 3};
    const uint8_t celt20x2[] = {(31 << 3) | 1};
    const uint8_t silk60[] = {3 << 3};
    const uint8_t code3[] = {(31 << 3) | 3, 3};
    EXPECT_EQ(DvrWriter::opusSamples(celt20, 1), 960);
    EXPECT_EQ(DvrWriter::opusSamples(celt20x2, 1), 1920);
    EXPECT_EQ(DvrWriter::opusSamples(silk60, 1), 2880);
    EXPECT_EQ(DvrWriter::opusSamples(code3, 2), 2880);
    EXPECT_EQ(DvrWriter::opusSamples(code3, 1), 0);  // code 3 without its count byte
}

TEST(DvrWriter, RtpPayloadPastCsrcsExtensionAndPadding)
{
    // V=2, padding, extension, two CSRCs; an extension of one word; 3 bytes of padding
    std::vector<uint8_t> p = {0xB2, 98, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,  // fixed header
                              1, 1, 1, 1, 2, 2, 2, 2,                   // CSRCs
                              0xBE, 0xDE, 0, 1, 9, 9, 9, 9,             // extension
                              0xF8, 1, 2,                               // payload
                              0, 0, 3};                                 // padding
    size_t off = 0;
    size_t len = 0;
    ASSERT_TRUE(DvrWriter::rtpPayload(p.data(), p.size(), &off, &len));
    EXPECT_EQ(off, 28u);
    EXPECT_EQ(len, 3u);
    p[0] = 0x40;  // version 1
    EXPECT_FALSE(DvrWriter::rtpPayload(p.data(), p.size(), &off, &len));
}

TEST(DvrWriter, FragmentedRecordingHasBothTracksAndAnOpusEntry)
{
    const auto file   = record(true, 5.0, 0, 0);
    const auto tracks = readTracks(file);
    ASSERT_EQ(tracks.size(), 2u);
    const TrackInfo& video = tracks.at(1);
    const TrackInfo& sound = tracks.at(2);
    EXPECT_EQ(video.sampleEntry, "hvc1");
    EXPECT_EQ(video.timescale, 90000u);
    EXPECT_EQ(sound.sampleEntry, "Opus");
    EXPECT_EQ(sound.timescale, 48000u);
    // dOps: version 0, one channel, pre-skip 0, 48000 Hz, gain 0, mapping family 0
    ASSERT_EQ(sound.dOps.size(), 11u);
    EXPECT_EQ(sound.dOps[0], 0);
    EXPECT_EQ(sound.dOps[1], 1);
    EXPECT_EQ(be32(sound.dOps.data() + 4), 48000u);
    EXPECT_EQ(sound.dOps[10], 0);
    // 300 frames, none of them the AUDs
    EXPECT_EQ(video.samples, 300u);
}

TEST(DvrWriter, BothTracksLastAsLongAsTheFlightDespiteLosses)
{
    for (bool fragmented : {true, false})
    {
        // A frame in seven lost, and runs of five audio packets out of every fifty.
        const auto file   = record(fragmented, 20.0, 7, 5);
        const auto tracks = readTracks(file);
        ASSERT_EQ(tracks.size(), 2u) << (fragmented ? "fragmented" : "plain");
        const double videoS = tracks.at(1).seconds();
        const double soundS = tracks.at(2).seconds();
        // The old constant 1/fps would have made this 20 s * 6/7 = 17.1 s.
        EXPECT_NEAR(videoS, 20.0, 0.05) << (fragmented ? "fragmented" : "plain");
        EXPECT_NEAR(soundS, 20.0, 0.05) << (fragmented ? "fragmented" : "plain");
        EXPECT_NEAR(videoS, soundS, 0.04) << "picture and sound out of step";
    }
}

TEST(DvrWriter, NoSoundTrackWhenNoneIsArriving)
{
    const auto tracks = readTracks(record(true, 2.0, 0, 0, false));
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.begin()->second.sampleEntry, "hvc1");
}

TEST(DvrWriter, NothingIsWrittenBeforeTheFirstKeyFrame)
{
    Sink      sink;
    DvrWriter w(&Sink::write, &sink, true);
    const auto t0 = Clock::now();
    // P-frames and audio first: the recording starts in the middle of a GOP.
    for (int f = 0; f < 10; ++f)
    {
        const auto s = slice(false);
        w.video(s.data(), s.size(), true, 1920, 1080, 60.0f, true, t0 + std::chrono::milliseconds(16 * f));
        const auto a = opusRtp((uint16_t) f, 960u * f);
        w.audio(a.data(), a.size(), t0 + std::chrono::milliseconds(16 * f));
    }
    EXPECT_EQ(w.videoSamples(), 0);
    for (const auto* ps : {&kVps, &kSps, &kPps}) w.video(ps->data(), ps->size(), true, 1920, 1080, 60.0f, true, t0 + std::chrono::milliseconds(170));
    const auto idr = slice(true);
    w.video(idr.data(), idr.size(), true, 1920, 1080, 60.0f, true, t0 + std::chrono::milliseconds(170));
    EXPECT_EQ(w.videoSamples(), 1);
    EXPECT_EQ(w.audioSamples(), 0);
    w.close();
}
