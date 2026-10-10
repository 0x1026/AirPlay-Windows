#include "airplay/info_plist.h"
#include "airplay/live_settings.h"
#include "airplay/routes.h"
#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/evp.h>

#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

namespace ap::airplay {
namespace {

#if defined(HAVE_LIBPLIST)

// mDNS TXT advertises `features` as two 32-bit hex words "0xLOW,0xHIGH".
// The /info plist expects a single 64-bit integer: high << 32 | low.
uint64_t parse_features(const std::string& s) {
    auto comma = s.find(',');
    uint64_t low  = 0;
    uint64_t high = 0;
    try {
        low = std::stoull(s.substr(0, comma), nullptr, 16);
        if (comma != std::string::npos) {
            high = std::stoull(s.substr(comma + 1), nullptr, 16);
        }
    } catch (...) {
        LOG_WARN << "could not parse features string: " << s;
    }
    return (high << 32) | (low & 0xFFFFFFFFULL);
}

void set_str(plist_t d, const char* k, const std::string& v) {
    plist_dict_set_item(d, k, plist_new_string(v.c_str()));
}

void set_uint(plist_t d, const char* k, uint64_t v) {
    plist_dict_set_item(d, k, plist_new_uint(v));
}

void set_bool(plist_t d, const char* k, bool v) {
    plist_dict_set_item(d, k, plist_new_bool(v ? 1 : 0));
}

plist_t make_display(int width, int height,
                     int refresh_rate, int max_fps) {
    plist_t d = plist_new_dict();
    set_uint(d, "features",      14);
    set_uint(d, "width",         static_cast<uint64_t>(width));
    set_uint(d, "height",        static_cast<uint64_t>(height));
    set_uint(d, "widthPixels",   static_cast<uint64_t>(width));
    set_uint(d, "heightPixels",  static_cast<uint64_t>(height));
    set_uint(d, "widthPhysical", 0);
    set_uint(d, "heightPhysical", 0);
    set_uint(d, "refreshRate",   static_cast<uint64_t>(refresh_rate));
    set_uint(d, "maxFPS",        static_cast<uint64_t>(max_fps));
    set_uint(d, "rotation",      0);
    set_bool(d, "overscanned",   false);
    set_str(d, "uuid", "e5f7a168-f1f9-4f51-9f9e-6a9cc0c8cc9f");
    return d;
}

plist_t make_audio_format(int type) {
    plist_t d = plist_new_dict();
    set_uint(d, "type",               type);
    set_uint(d, "audioInputFormats",  0x03FFFFFC);
    set_uint(d, "audioOutputFormats", 0x03FFFFFC);
    return d;
}

plist_t make_audio_latency(int type) {
    plist_t d = plist_new_dict();
    set_str(d, "audioType",           "default");
    set_uint(d, "type",                type);
    set_uint(d, "inputLatencyMicros",  3000);
    set_uint(d, "outputLatencyMicros", 90000);
    return d;
}

std::vector<unsigned char> serialize_binary_plist(plist_t root,
                                                  const char* label) {
    std::vector<unsigned char> out;
    if (!root) return out;

    char* data = nullptr;
    uint32_t len = 0;
    plist_to_bin(root, &data, &len);
    plist_free(root);

    if (!data || len == 0) {
        LOG_ERROR << label << ": plist_to_bin failed";
        if (data) std::free(data);
        return out;
    }

    out.assign(reinterpret_cast<unsigned char*>(data),
               reinterpret_cast<unsigned char*>(data) + len);
    std::free(data);
    return out;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       if (c >= 'a' && c <= 'z') {
                           return static_cast<char>(c - ('a' - 'A'));
                       }
                       return static_cast<char>(c);
                   });
    return value;
}

std::string public_key_hex(const std::vector<unsigned char>& key) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (unsigned char b : key) {
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::string base64_without_padding(const unsigned char* data,
                                   std::size_t len) {
    if (!data || len == 0) return {};

    const std::size_t capacity = 4 * ((len + 2) / 3);
    std::string out(capacity, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data,
        static_cast<int>(len));
    if (written <= 0) return {};

    out.resize(static_cast<std::size_t>(written));
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

// AirPlay `featuresEx` is the base64 representation of the 64-bit feature
// bitmap in little-endian byte order, with base64 padding omitted.
std::string make_features_ex(uint64_t features) {
    unsigned char le[8];
    for (int i = 0; i < 8; ++i) {
        le[i] = static_cast<unsigned char>((features >> (8 * i)) & 0xff);
    }
    return base64_without_padding(le, sizeof(le));
}

void append_txt_record(std::vector<unsigned char>& out,
                       const std::string& value) {
    if (value.empty()) return;
    if (value.size() > 255) {
        LOG_WARN << "AirPlay event txtAirPlay entry too long, skipping: "
                 << value.substr(0, 32) << "...";
        return;
    }

    out.push_back(static_cast<unsigned char>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

// `txtAirPlay` is not a normal plist dictionary. It is the raw DNS-SD TXT
// payload: each UTF-8 key/value string is prefixed by its one-byte length.
// This mirrors the shape used by real AirPlay 2 receivers and Shairport Sync.
std::vector<unsigned char> make_txt_airplay(const DeviceContext& ctx,
                                             const std::string& features_ex,
                                             const std::string& psi) {
    std::vector<unsigned char> out;
    const std::string pk = public_key_hex(ctx.public_key);

    append_txt_record(out, "acl=0");
    append_txt_record(out, "deviceid=" + ctx.deviceid);
    append_txt_record(out, "fex=" + features_ex);
    append_txt_record(out, "features=" + ctx.features);
    append_txt_record(out, "flags=0x244");

    // We are not in an AirPlay group. Using pi as gid is the standard
    // standalone-receiver representation.
    append_txt_record(out, "gid=" + ctx.pi);
    append_txt_record(out, "igl=1");
    append_txt_record(out, "gcgl=1");
    append_txt_record(out, "model=" + ctx.model);
    append_txt_record(out, "protovers=1.1");
    append_txt_record(out, "pi=" + ctx.pi);
    append_txt_record(out, "psi=" + psi);
    if (!pk.empty()) append_txt_record(out, "pk=" + pk);
    append_txt_record(out, "srcvers=" + ctx.srcvers);
    append_txt_record(out, "vv=2");
    return out;
}

uint64_t live_features(const DeviceContext& ctx) {
    bool hevc = true;
    if (ctx.live) {
        hevc = ctx.live->hevc_enabled.load(std::memory_order_relaxed);
    }

    uint64_t feat = parse_features(ctx.features);
    if (hevc) feat |=  (UINT64_C(0x400) << 32);
    else      feat &= ~(UINT64_C(0x400) << 32);
    return feat;
}

#endif // HAVE_LIBPLIST

} // namespace

std::vector<unsigned char> build_info_plist(const DeviceContext& ctx) {
#if !defined(HAVE_LIBPLIST)
    (void)ctx;
    LOG_WARN << "libplist unavailable — cannot build binary /info plist";
    return {};
#else
    plist_t root = plist_new_dict();

    int mirror_w     = ctx.mirror_width;
    int mirror_h     = ctx.mirror_height;
    int max_fps      = 60;
    int refresh_rate = 60;
    if (ctx.live) {
        mirror_w     = ctx.live->mirror_width.load(std::memory_order_relaxed);
        mirror_h     = ctx.live->mirror_height.load(std::memory_order_relaxed);
        max_fps      = ctx.live->max_fps.load(std::memory_order_relaxed);
        refresh_rate = ctx.live->refresh_rate.load(std::memory_order_relaxed);
    }

    const uint64_t feat = live_features(ctx);

    set_str(root, "deviceid",      ctx.deviceid);
    set_str(root, "macAddress",    ctx.deviceid);
    set_str(root, "model",         ctx.model);
    set_str(root, "name",          ctx.name);
    set_str(root, "pi",            ctx.pi);
    set_str(root, "sourceVersion", ctx.srcvers);
    set_uint(root, "features",     feat);
    set_uint(root, "statusFlags",  68);
    set_uint(root, "vv",           2);
    set_bool(root, "keepAliveLowPower",        true);
    set_bool(root, "keepAliveSendStatsAsBody", true);

    if (!ctx.public_key.empty()) {
        plist_dict_set_item(root, "pk", plist_new_data(
            reinterpret_cast<const char*>(ctx.public_key.data()),
            ctx.public_key.size()));
    } else {
        LOG_WARN << "DeviceContext.public_key empty — /info pk=<32 zeros>";
        unsigned char zero[32] = {0};
        plist_dict_set_item(root, "pk",
            plist_new_data(reinterpret_cast<const char*>(zero), sizeof(zero)));
    }

    plist_t displays = plist_new_array();
    plist_array_append_item(displays,
        make_display(mirror_w, mirror_h, refresh_rate, max_fps));
    plist_dict_set_item(root, "displays", displays);

    plist_t formats = plist_new_array();
    plist_array_append_item(formats, make_audio_format(100));
    plist_array_append_item(formats, make_audio_format(101));
    plist_dict_set_item(root, "audioFormats", formats);

    plist_t latencies = plist_new_array();
    plist_array_append_item(latencies, make_audio_latency(100));
    plist_dict_set_item(root, "audioLatencies", latencies);

    return serialize_binary_plist(root, "/info");
#endif
}

std::vector<unsigned char> build_event_info_plist(const DeviceContext& ctx) {
#if !defined(HAVE_LIBPLIST)
    (void)ctx;
    LOG_WARN << "libplist unavailable — cannot build AirPlay event updateInfo plist";
    return {};
#else
    // The event-channel updateInfo is related to GET /info, but real AirPlay 2
    // receivers publish a richer receiver description here. In particular,
    // modern iOS expects `featuresEx` and the raw `txtAirPlay` DNS-SD payload.
    // Sending only our minimal GET /info dictionary makes iOS accept the HAP
    // TCP channel but silently ignore the updateInfo request.
    plist_t root = plist_new_dict();
    if (!root) return {};

    const uint64_t feat = live_features(ctx);
    const std::string fex = make_features_ex(feat);
    const std::string psi = uppercase_ascii(ctx.pi);
    const auto txt_airplay = make_txt_airplay(ctx, fex, psi);

    set_str(root, "psi", psi);
    set_uint(root, "vv", 2);

    plist_t playback = plist_new_dict();
    set_bool(playback, "supportsInterstitials", false);
    set_bool(playback, "supportsFPSSecureStop", false);
    set_bool(playback, "supportsUIForAudioOnlyContent", true);
    plist_dict_set_item(root, "playbackCapabilities", playback);

    set_bool(root, "canRecordScreenStream", false);
    set_uint(root, "statusFlags", 0x244);
    set_bool(root, "keepAliveSendStatsAsBody", true);
    set_str(root, "name", ctx.name);
    set_str(root, "protocolVersion", "1.1");
    set_uint(root, "volumeControlType", 4);
    set_str(root, "deviceID", ctx.deviceid);
    set_str(root, "pi", ctx.pi);
    set_bool(root, "screenDemoMode", false);
    set_str(root, "featuresEx", fex);

    if (!txt_airplay.empty()) {
        plist_dict_set_item(root, "txtAirPlay",
            plist_new_data(
                reinterpret_cast<const char*>(txt_airplay.data()),
                static_cast<uint64_t>(txt_airplay.size())));
    }

    // Known-good format map used by Apple TV class AirPlay 2 receivers. These
    // fields describe receiver capabilities on the event channel; they do not
    // alter the already-negotiated RAOP type-96 stream.
    plist_t supported = plist_new_dict();
    set_uint(supported, "lowLatencyAudioStream", 0);
    set_uint(supported, "screenStream", 21235712);
    set_uint(supported, "audioStream", 21235712);
    set_uint(supported, "bufferStream", 14680064);
    plist_dict_set_item(root, "supportedFormats", supported);

    set_str(root, "sourceVersion", ctx.srcvers);
    set_bool(root, "hasUDPMirroringSupport", true);
    set_str(root, "model", ctx.model);

    if (!ctx.public_key.empty()) {
        plist_dict_set_item(root, "pk", plist_new_data(
            reinterpret_cast<const char*>(ctx.public_key.data()),
            ctx.public_key.size()));
    } else {
        unsigned char zero[32] = {0};
        plist_dict_set_item(root, "pk",
            plist_new_data(reinterpret_cast<const char*>(zero), sizeof(zero)));
    }

    set_str(root, "macAddress", ctx.deviceid);
    set_uint(root, "features", feat);

    LOG_INFO << "AirPlay event updateInfo prepared: featuresEx=" << fex
             << " txtAirPlay=" << txt_airplay.size() << "B"
             << " statusFlags=0x244";

    return serialize_binary_plist(root, "AirPlay event updateInfo");
#endif
}

} // namespace ap::airplay
