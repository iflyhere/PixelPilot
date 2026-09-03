//
// MSP telemetry, for the setups where the flight controller never speaks MAVLink.
//

#ifndef FPVUE_MSP_H
#define FPVUE_MSP_H

#include <cstddef>
#include <cstdint>

/**
 * Decodes the MSP frames in one datagram into the shared telemetry struct.
 *
 * A Betaflight flight controller wired to an OpenIPC camera over MSP DisplayPort speaks MSP
 * and nothing else, and msposd forwards that stream verbatim unless it is started with
 * -M/--mavlink. So the bytes that arrive on the telemetry port are aggregated MSP frames,
 * which the MAVLink parser silently discards - an empty OSD with nothing in the log.
 *
 * Both framings are accepted: MSP v1 ("$M>") and MSP v2 ("$X>"). One datagram carries several
 * frames back to back, so the whole buffer is walked.
 *
 * @return true if any field was updated, i.e. the caller should publish the struct.
 */
bool msp_parse_datagram(const uint8_t* data, size_t len);

/** True if the buffer starts with an MSP v1 or v2 magic. */
bool msp_looks_like_msp(const uint8_t* data, size_t len);

#endif  // FPVUE_MSP_H
