#pragma once

#include "airplay/routes.h"

#include <vector>

namespace ap::airplay {

// Build the binary-plist payload returned for `GET /info`.
//
// Structure and field set mirror UxPlay's `plist/info.plist` baseline. Values
// specific to this receiver (deviceid, name, pi, model, sourceVersion,
// features) come from `ctx`.
std::vector<unsigned char> build_info_plist(const DeviceContext& ctx);

// Build the richer receiver-info dictionary placed in the AirPlay 2 event
// channel's `{ "type": "updateInfo", "value": ... }` command.
//
// This intentionally is not identical to GET /info: modern senders expect
// event-channel fields such as featuresEx, txtAirPlay, supportedFormats,
// protocolVersion and playbackCapabilities.
std::vector<unsigned char> build_event_info_plist(const DeviceContext& ctx);

} // namespace ap::airplay
