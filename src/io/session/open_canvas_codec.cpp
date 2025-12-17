#include "io/session/open_canvas_codec.h"

#include "io/binary_codec.h"
#include "io/session/project_state_json.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace open_canvas_codec
{
bool DecodeProjectState(const SessionState::OpenCanvas& oc, AnsiCanvas::ProjectState& out_ps, std::string& err)
{
    err.clear();
    if (oc.project_cbor_zstd_b64.empty() || oc.project_cbor_size == 0)
        return false;

    std::string comp_bytes;
    if (!Base64Decode(oc.project_cbor_zstd_b64, comp_bytes))
    {
        err = "base64 decode failed";
        return false;
    }

    std::string cbor_bytes;
    std::string zerr;
    if (!ZstdDecompressBytesKnownSize(comp_bytes, oc.project_cbor_size, cbor_bytes, zerr))
    {
        err = zerr;
        return false;
    }

    nlohmann::json j;
    try
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(cbor_bytes.data());
        j = nlohmann::json::from_cbor(p, p + cbor_bytes.size());
    }
    catch (const std::exception& e)
    {
        err = std::string("CBOR decode failed: ") + e.what();
        return false;
    }

    return project_state_json::FromJson(j, out_ps, err);
}

bool EncodeProjectState(const AnsiCanvas::ProjectState& ps, SessionState::OpenCanvas& oc, std::string& err)
{
    err.clear();
    oc.project_cbor_zstd_b64.clear();
    oc.project_cbor_size = 0;

    nlohmann::json j = project_state_json::ToJson(ps);
    std::vector<std::uint8_t> cbor;
    try
    {
        cbor = nlohmann::json::to_cbor(j);
    }
    catch (const std::exception& e)
    {
        err = std::string("CBOR encode failed: ") + e.what();
        return false;
    }

    oc.project_cbor_size = (std::uint64_t)cbor.size();
    std::string cbor_bytes(reinterpret_cast<const char*>(cbor.data()),
                           reinterpret_cast<const char*>(cbor.data() + cbor.size()));

    std::string comp;
    std::string zerr;
    if (!ZstdCompressBytes(cbor_bytes, comp, zerr))
    {
        err = zerr;
        return false;
    }
    if (!Base64Encode(reinterpret_cast<const std::uint8_t*>(comp.data()), comp.size(), oc.project_cbor_zstd_b64))
    {
        err = "base64 encode failed";
        return false;
    }
    return true;
}
} // namespace open_canvas_codec


