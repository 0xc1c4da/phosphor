#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// SAUCE (Standard Architecture for Universal Comment Extensions) helpers.
//
// This module is intended to be reusable across:
// - Import: detect/strip SAUCE and apply metadata (dimensions, author/title, flags).
// - Export: append SAUCE (+ optional EOF 0x1A and comment block) to a byte stream.
// - Project persistence (.phos): store SAUCE metadata in CBOR even though .phos is not a SAUCE-appended format.
//
// Spec reference: references/sauce-spec.md
namespace sauce
{
// SAUCE "00" record layout is always 128 bytes.
static constexpr size_t kSauceRecordSize = 128;
static constexpr size_t kSauceCommentHeaderSize = 5; // "COMNT"

enum class DataType : std::uint8_t
{
    None       = 0,
    Character  = 1,
    Bitmap     = 2,
    Vector     = 3,
    Audio      = 4,
    BinaryText = 5,
    XBin       = 6,
    Archive    = 7,
    Executable = 8,
};

// In SAUCE 00, all character fields are fixed-width and typically CP437.
// We expose strings as UTF-8 in the app layer.
struct Record
{
    bool present = false; // whether a SAUCE record was found / should be written

    // Fixed fields.
    std::string title;   // 35 chars
    std::string author;  // 20 chars
    std::string group;   // 20 chars
    std::string date;    // 8 chars "CCYYMMDD" (we keep as string to preserve unknown/invalid)

    // These are the raw SAUCE fields, preserved for round-tripping.
    std::uint32_t file_size = 0; // original file size, often unreliable in the wild
    std::uint8_t data_type = (std::uint8_t)DataType::Character;
    std::uint8_t file_type = 1; // Character->ANSi by default
    std::uint16_t tinfo1 = 0;
    std::uint16_t tinfo2 = 0;
    std::uint16_t tinfo3 = 0;
    std::uint16_t tinfo4 = 0;
    std::uint8_t comments_count = 0;
    std::uint8_t tflags = 0;
    std::string tinfos; // 22-byte ZString (often SAUCE font name)

    // Comment lines (each max 64 chars when written). Stored as UTF-8 strings.
    std::vector<std::string> comments;
};

struct Parsed
{
    Record record;

    // Where the "payload" (art bytes) effectively ends, derived from structure:
    // payload [ + optional 0x1A ] [ + optional COMNT block ] + SAUCE record
    // This is preferred over trusting Record::file_size.
    size_t payload_size = 0;

    bool has_eof_byte = false;       // whether an 0x1A byte was found right before metadata
    bool has_comment_block = false;  // whether COMNT block was validated and parsed
};

struct WriteOptions
{
    bool include_eof_byte = true;   // append 0x1A before COMNT/SAUCE
    bool include_comments = true;   // write COMNT block if Record::comments not empty
    bool encode_cp437 = true;       // encode fixed fields as CP437 bytes (fallback to '?' if not representable)
};

// ---------------------------------------------------------------------------
// SAUCE helper utilities (shared by core/UI/exporters)
// ---------------------------------------------------------------------------

// Filter out ASCII control bytes from a UTF-8 string (best-effort).
// - Keeps bytes >= 0x20 and != 0x7F
// - Leaves UTF-8 multibyte sequences intact
void FilterControlChars(std::string& s);

// Keep only ASCII digits (0..9).
void KeepOnlyDigits(std::string& s);

// Count Unicode codepoints in a UTF-8 string (best-effort; invalid sequences count as 1 byte/1 cp).
size_t Utf8CodepointCount(std::string_view s);

// Trim UTF-8 string to at most `max_codepoints` Unicode codepoints (best-effort).
// This is used for enforcing SAUCE fixed-width field limits in the UI.
void TrimUtf8ToCodepoints(std::string& s, size_t max_codepoints);

// Date helpers: SAUCE stores dates as exactly 8 ASCII digits: CCYYMMDD.
bool ParseDateYYYYMMDD(std::string_view s, int& y, int& m, int& d);
void FormatDateYYYYMMDD(int y, int m, int d, std::string& out);
std::string TodayYYYYMMDD();

// Parse SAUCE (and optional COMNT) from the end of `bytes`.
// On success, out.record.present indicates whether a SAUCE record was found.
bool ParseFromBytes(const std::vector<std::uint8_t>& bytes, Parsed& out, std::string& err, bool decode_cp437 = true);

// Return the payload size after stripping SAUCE/COMNT/EOF if present.
// If no SAUCE is present, this returns bytes.size().
size_t ComputePayloadSize(const std::vector<std::uint8_t>& bytes);

// Copy payload bytes without SAUCE/COMNT/EOF if present.
std::vector<std::uint8_t> StripFromBytes(const std::vector<std::uint8_t>& bytes);

// Append SAUCE/COMNT/EOF to an existing payload stream.
// Returns an empty vector on error and sets err.
std::vector<std::uint8_t> AppendToBytes(const std::vector<std::uint8_t>& payload,
                                        const Record& record,
                                        const WriteOptions& opt,
                                        std::string& err);

// Convenience: encode a fixed-width SAUCE character field.
// - Pads with spaces
// - Truncates to `width`
// - Encodes as CP437 (when encode_cp437=true) or ASCII-ish bytes
std::vector<std::uint8_t> EncodeCharField(std::string_view s, size_t width, bool encode_cp437);
} // namespace sauce


