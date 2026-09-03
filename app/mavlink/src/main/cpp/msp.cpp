//
// MSP telemetry, for the setups where the flight controller never speaks MAVLink.
//

#include "msp.h"

#include <android/log.h>

#include <cmath>
#include <cstring>
#include <set>

#include "mavlink.h"

#define TAG "pixelpilot"

// Betaflight command ids. Only the ones carrying something the OSD can show.
#define MSP_STATUS 101
#define MSP_RC 105
#define MSP_RAW_GPS 106
#define MSP_COMP_GPS 107
#define MSP_ATTITUDE 108
#define MSP_ALTITUDE 109
#define MSP_ANALOG 110
#define MSP_BATTERY_STATE 130

namespace {

// Reported once per command id, the same way the MAVLink path reports msgids: without it a
// message that is simply not being forwarded looks identical to one that is decoded wrongly.
std::set<uint16_t> g_seen;

// Little endian readers. MSP is little endian throughout, and the payloads are unaligned, so
// nothing may be cast to a wider type in place.
uint16_t rd_u16(const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

int16_t rd_i16(const uint8_t* p) { return (int16_t) rd_u16(p); }

uint32_t rd_u32(const uint8_t* p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

int32_t rd_i32(const uint8_t* p) { return (int32_t) rd_u32(p); }

uint8_t crc8_dvb_s2(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; ++i)
    {
        crc = (crc & 0x80) ? (uint8_t) ((crc << 1) ^ 0xD5) : (uint8_t) (crc << 1);
    }
    return crc;
}

/** Great-circle distance in metres. Same job as the MAVLink path's home distance. */
double distance_between(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusM = 6371000.0;
    constexpr double kDeg2Rad      = M_PI / 180.0;
    const double     dLat          = (lat2 - lat1) * kDeg2Rad;
    const double     dLon          = (lon2 - lon1) * kDeg2Rad;
    const double     a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * kDeg2Rad) *
                                                                       std::cos(lat2 * kDeg2Rad) *
                                                                       std::sin(dLon / 2) * std::sin(dLon / 2);
    return kEarthRadiusM * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

bool handle_frame(uint16_t cmd, const uint8_t* p, size_t len)
{
    if (g_seen.insert(cmd).second)
    {
        __android_log_print(ANDROID_LOG_INFO, TAG, "msp: first cmd %u (%zu byte payload)", cmd, len);
    }

    switch (cmd)
    {
        case MSP_ATTITUDE:
            // roll and pitch in 0.1 degrees, yaw in whole degrees.
            if (len < 6) return false;
            latestMavlinkData.telemetry_roll  = rd_i16(p) / 10.0f;
            latestMavlinkData.telemetry_pitch = rd_i16(p + 2) / 10.0f;
            latestMavlinkData.telemetry_yaw   = (float) rd_i16(p + 4);
            latestMavlinkData.heading         = (uint16_t) ((rd_i16(p + 4) + 360) % 360);
            return true;

        case MSP_ALTITUDE:
            // Estimated altitude in cm, then the variometer in cm/s.
            if (len < 6) return false;
            latestMavlinkData.telemetry_altitude = rd_i32(p) / 100.0f;
            latestMavlinkData.telemetry_vspeed   = rd_i16(p + 4) / 100.0f;
            return true;

        case MSP_ANALOG:
        {
            // Legacy layout is vbat(u8 0.1V), mAhDrawn(u16), rssi(u16 0..1023), amperage(i16
            // 0.01A); API 1.41 appended voltage as u16 in 0.01V, which is the one to prefer
            // because the u8 saturates at 25.5V - a 6S pack sits right at that edge.
            if (len < 7) return false;
            latestMavlinkData.telemetry_current_consumed = (float) rd_u16(p + 1);
            latestMavlinkData.telemetry_rssi             = rd_u16(p + 3) * 100.0f / 1023.0f;
            latestMavlinkData.telemetry_current          = rd_i16(p + 5) / 100.0f;
            latestMavlinkData.telemetry_battery =
                (len >= 9) ? rd_u16(p + 7) / 100.0f : p[0] / 10.0f;
            return true;
        }

        case MSP_BATTERY_STATE:
            // cellCount(u8), capacity(u16), vbat(u8 0.1V), mAhDrawn(u16), amperage(i16 0.01A),
            // state(u8), voltage(u16 0.01V). Preferred over MSP_ANALOG when present.
            if (len < 9) return false;
            latestMavlinkData.telemetry_current_consumed = (float) rd_u16(p + 4);
            latestMavlinkData.telemetry_current          = rd_i16(p + 6) / 100.0f;
            if (len >= 12)
            {
                latestMavlinkData.telemetry_battery = rd_u16(p + 10) / 100.0f;
            }
            return true;

        case MSP_RAW_GPS:
        {
            // fixType(u8), numSat(u8), lat(i32 1e7), lon(i32 1e7), alt(i16 m),
            // groundSpeed(u16 cm/s), groundCourse(u16 0.1deg), hdop(u16).
            if (len < 16) return false;
            const uint8_t fix                = p[0];
            latestMavlinkData.gps_fix_type   = fix ? MAVLINK_GPS_FIX_TYPE_3D_FIX : MAVLINK_GPS_FIX_TYPE_NO_FIX;
            latestMavlinkData.telemetry_sats = (float) p[1];
            latestMavlinkData.telemetry_lat  = rd_i32(p + 2) / 1e7;
            latestMavlinkData.telemetry_lon  = rd_i32(p + 6) / 1e7;
            latestMavlinkData.telemetry_gspeed = rd_u16(p + 12) / 100.0f;
            if (len >= 18)
            {
                latestMavlinkData.hdop = rd_u16(p + 16);
            }
            // Home is latched on the first fix, the same as the MAVLink path does, so the
            // distance readout has something to measure against.
            if (fix && latestMavlinkData.telemetry_lat_base == 0 && latestMavlinkData.telemetry_lon_base == 0)
            {
                latestMavlinkData.telemetry_lat_base = latestMavlinkData.telemetry_lat;
                latestMavlinkData.telemetry_lon_base = latestMavlinkData.telemetry_lon;
            }
            return true;
        }

        case MSP_COMP_GPS:
            // distanceToHome(u16 m), directionToHome(i16 deg), update(u8). Betaflight computes
            // both itself, so they are preferred over deriving them here.
            if (len < 4) return false;
            latestMavlinkData.telemetry_distance = (double) rd_u16(p);
            latestMavlinkData.telemetry_hdg      = (double) rd_i16(p + 2);
            return true;

        case MSP_STATUS:
        {
            // cycleTime(u16), i2cErrors(u16), sensors(u16), flightModeFlags(u32), ...
            // Bit 0 of the mode flags is BOXARM on Betaflight.
            if (len < 10) return false;
            const uint32_t modes            = rd_u32(p + 6);
            latestMavlinkData.telemetry_arm = (modes & 1u) ? 1.0f : 0.0f;
            latestMavlinkData.flight_mode   = (uint8_t) ((modes & 1u) ? FLIGHT_MODE_ARMED : 0);
            return true;
        }

        case MSP_RC:
            // Channels in microseconds. Throttle is channel 3 on the AETR order Betaflight
            // reports here, regardless of the transmitter's own channel map.
            if (len < 8) return false;
            latestMavlinkData.telemetry_throttle = (rd_u16(p + 6) - 1000) / 10.0f;
            if (latestMavlinkData.telemetry_throttle < 0) latestMavlinkData.telemetry_throttle = 0;
            if (latestMavlinkData.telemetry_throttle > 100) latestMavlinkData.telemetry_throttle = 100;
            return true;

        default:
            return false;
    }
}

}  // namespace

bool msp_looks_like_msp(const uint8_t* data, size_t len)
{
    return len >= 3 && data[0] == '$' && (data[1] == 'M' || data[1] == 'X');
}

bool msp_parse_datagram(const uint8_t* data, size_t len)
{
    bool   changed = false;
    size_t i       = 0;

    while (i + 3 <= len)
    {
        if (data[i] != '$')
        {
            ++i;
            continue;
        }

        if (data[i + 1] == 'M')
        {
            // $ M dir size cmd payload... crc, crc = XOR over size, cmd and payload.
            if (i + 6 > len) break;
            const uint8_t  size    = data[i + 3];
            const uint8_t  cmd     = data[i + 4];
            const size_t   total   = 6u + size;
            if (i + total > len) break;
            const uint8_t* payload = data + i + 5;

            uint8_t crc = size ^ cmd;
            for (size_t k = 0; k < size; ++k) crc ^= payload[k];
            if (crc == data[i + 5 + size])
            {
                changed |= handle_frame(cmd, payload, size);
                i += total;
                continue;
            }
            // Bad checksum: step one byte rather than the whole frame, so a resync lands on
            // the next real magic instead of skipping past it.
            ++i;
            continue;
        }

        if (data[i + 1] == 'X')
        {
            // $ X dir flag cmd(u16) size(u16) payload... crc8_dvb_s2 over flag..payload.
            if (i + 9 > len) break;
            const uint16_t cmd   = rd_u16(data + i + 4);
            const uint16_t size  = rd_u16(data + i + 6);
            const size_t   total = 9u + size;
            if (i + total > len) break;
            const uint8_t* payload = data + i + 8;

            uint8_t crc = 0;
            for (size_t k = 3; k < 8; ++k) crc = crc8_dvb_s2(crc, data[i + k]);
            for (size_t k = 0; k < size; ++k) crc = crc8_dvb_s2(crc, payload[k]);
            if (crc == data[i + 8 + size])
            {
                changed |= handle_frame(cmd, payload, size);
                i += total;
                continue;
            }
            ++i;
            continue;
        }

        ++i;
    }

    if (changed && latestMavlinkData.telemetry_lat_base != 0 && latestMavlinkData.telemetry_distance == 0)
    {
        // Only as a fallback: Betaflight sends MSP_COMP_GPS when it has a home position, and
        // its own figure is the better one.
        latestMavlinkData.telemetry_distance = distance_between(latestMavlinkData.telemetry_lat_base,
                                                                latestMavlinkData.telemetry_lon_base,
                                                                latestMavlinkData.telemetry_lat,
                                                                latestMavlinkData.telemetry_lon);
    }

    return changed;
}
