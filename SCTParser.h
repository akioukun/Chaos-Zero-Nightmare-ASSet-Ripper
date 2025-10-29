#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>
#include <tuple>
#include <astcenc.h>

namespace SCTParser {

    static constexpr int SCT2_SIGNATURE = 844383059;
    static constexpr int SCT_SIGNATURE_WORD = 17235;
    static constexpr uint8_t SCT_SIGNATURE_BYTE = 84;


    enum class Format {
        Unknown = -1,
        SCT = 10001,
        SCT2 = 10002
    };


    struct Header {
        std::vector<uint8_t> signature;
        int pixel_format = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t texture_width = 0;
        uint16_t texture_height = 0;
        int data_offset = 0;
        bool compressed = false;


        int total_size = 0;
        uint8_t flags = 0;
        bool has_alpha = false;
        bool crop_flag = false;
        bool raw_data = false;
        bool mipmap_flag2 = false;
    };


    struct PixelFormatInfo {
        std::string format;
        int channels;
        std::string type;
    };


    Format DetectFormat(const std::vector<uint8_t>& data, bool debug = false);
    Header ParseSCTHeader(const std::vector<uint8_t>& data);
    Header ParseSCT2Header(const std::vector<uint8_t>& data);


    std::vector<uint8_t> LZ4Decompress(const std::vector<uint8_t>& compressed_data);


    std::vector<uint8_t> RGB565LEToRGB(const std::vector<uint8_t>& data);
    std::vector<uint8_t> RGBToRGBA(const std::vector<uint8_t>& rgb_data);
    void BGRASwapRB(std::vector<uint8_t>& buffer);


    PixelFormatInfo GetPixelFormatInfo(int format_code);
    bool ShouldDecompressIntelligently(const std::vector<uint8_t>& image_data,
        int width, int height, int pixel_format,
        bool verbose = false);


    std::vector<uint8_t> ConvertToPNG(const std::vector<uint8_t>& data, bool verbose = false);


    std::vector<uint8_t> DecodeASTC(const std::vector<uint8_t>& compressed_data,
        int width, int height, int block_width, int block_height);
    std::vector<uint8_t> DecodeETC2RGBA8(const std::vector<uint8_t>& compressed_data,
        int width, int height, bool verbose = false);
}
