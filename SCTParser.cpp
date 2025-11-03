#include "SCTParser.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include "Logger.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define ETCDEC_IMPLEMENTATION
#include "etcdec.h"
#define ASTCENC_API
#include <astcenc.h>
namespace SCTParser {

    Format DetectFormat(const std::vector<uint8_t>& data, bool debug) {
        if (data.size() < 4) {
            if (debug) std::cout << "Data too short: " << data.size() << " bytes\n";
            return Format::Unknown;
        }


        int signature = *reinterpret_cast<const int*>(data.data());
        if (debug) std::cout << "4-byte signature: " << signature << " (0x" << std::hex << signature << std::dec << ")\n";

        if (signature == SCT2_SIGNATURE) {
            if (debug) std::cout << "Matched SCT2!\n";
            return Format::SCT2;
        }


        if (data.size() >= 3) {
            uint16_t word = *reinterpret_cast<const uint16_t*>(data.data());
            uint8_t b = data[2];
            if (debug) std::cout << "SCT check: word=" << word << ", byte=" << (int)b << "\n";

            if (word == SCT_SIGNATURE_WORD && b == SCT_SIGNATURE_BYTE) {
                if (debug) std::cout << "Matched SCT!\n";
                return Format::SCT;
            }
        }

        return Format::Unknown;
    }

    Header ParseSCTHeader(const std::vector<uint8_t>& data) {
        if (data.size() < 9)
            throw std::runtime_error("File too small to contain a valid SCT header");

        Header header;
        header.signature = { data[0], data[1], data[2] };
        header.pixel_format = data[4];
        header.width = *reinterpret_cast<const uint16_t*>(&data[5]);
        header.height = *reinterpret_cast<const uint16_t*>(&data[7]);
        header.data_offset = 9;
        header.compressed = true;
        header.texture_width = header.width;
        header.texture_height = header.height;
        header.flags = 0;
        header.has_alpha = false;

        return header;
    }

    Header ParseSCT2Header(const std::vector<uint8_t>& data) {
        if (data.size() < 34)
            throw std::runtime_error("File too small to contain a valid SCT2 header");

        Header header;
        header.signature = { data[0], data[1], data[2], data[3] };
        header.total_size = *reinterpret_cast<const int*>(&data[4]);
        header.data_offset = *reinterpret_cast<const int*>(&data[12]);
        header.pixel_format = *reinterpret_cast<const int*>(&data[20]);
        header.width = *reinterpret_cast<const uint16_t*>(&data[24]);
        header.height = *reinterpret_cast<const uint16_t*>(&data[26]);
        header.texture_width = *reinterpret_cast<const uint16_t*>(&data[28]);
        header.texture_height = *reinterpret_cast<const uint16_t*>(&data[30]);
        header.flags = data[32];

        header.has_alpha = (header.flags & 0x01) != 0;
        header.crop_flag = (header.flags & 0x02) != 0;
        header.raw_data = (header.flags & 0x10) != 0;
        header.mipmap_flag2 = (header.flags & 0x20) != 0;
        header.compressed = (header.flags & 0x80) != 0;

        return header;
    }

    std::vector<uint8_t> LZ4Decompress(const std::vector<uint8_t>& compressed_data) {
        if (compressed_data.size() < 8)
            throw std::runtime_error("Compressed data too short");

        int decompressed_size = *reinterpret_cast<const int*>(compressed_data.data());
        int compressed_size = *reinterpret_cast<const int*>(compressed_data.data() + 4);

        std::vector<uint8_t> dst(decompressed_size);
        size_t src_pos = 8;
        size_t dst_pos = 0;

        while (src_pos < compressed_data.size() && dst_pos < (size_t)decompressed_size) {
            if (src_pos >= compressed_data.size()) break;

            uint8_t token = compressed_data[src_pos++];
            int literal_length = (token >> 4) & 0x0F;
            int match_length = token & 0x0F;

            if (literal_length == 15) {
                while (src_pos < compressed_data.size()) {
                    uint8_t extra = compressed_data[src_pos++];
                    literal_length += extra;
                    if (extra != 255) break;
                }
            }

            if (literal_length > 0) {
                if (src_pos + literal_length > compressed_data.size())
                    literal_length = compressed_data.size() - src_pos;
                if (dst_pos + literal_length > dst.size())
                    literal_length = dst.size() - dst_pos;

                std::memcpy(dst.data() + dst_pos, compressed_data.data() + src_pos, literal_length);
                src_pos += literal_length;
                dst_pos += literal_length;
            }

            if (src_pos >= compressed_data.size() || dst_pos >= (size_t)decompressed_size)
                break;

            if (src_pos + 1 >= compressed_data.size()) break;

            uint16_t offset = *reinterpret_cast<const uint16_t*>(compressed_data.data() + src_pos);
            src_pos += 2;

            if (match_length == 15) {
                while (src_pos < compressed_data.size()) {
                    uint8_t extra = compressed_data[src_pos++];
                    match_length += extra;
                    if (extra != 255) break;
                }
            }

            match_length += 4;

            int match_start = dst_pos - offset;
            if (match_start < 0) break;

            for (int i = 0; i < match_length && dst_pos < (size_t)decompressed_size && match_start + i < (int)dst_pos; i++) {
                dst[dst_pos++] = dst[match_start + i];
            }
        }

        if (dst_pos < (size_t)decompressed_size) {
            dst.resize(dst_pos);
        }

        return dst;
    }

    std::vector<uint8_t> RGB565LEToRGB(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> rgb_data;
        rgb_data.reserve((data.size() / 2) * 3);

        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            uint16_t pixel = *reinterpret_cast<const uint16_t*>(&data[i]);

            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;

            rgb_data.push_back(r);
            rgb_data.push_back(g);
            rgb_data.push_back(b);
        }

        return rgb_data;
    }

    std::vector<uint8_t> RGBToRGBA(const std::vector<uint8_t>& rgb_data) {
        std::vector<uint8_t> rgba(rgb_data.size() / 3 * 4);

        for (size_t i = 0, j = 0; i < rgb_data.size() - 2; i += 3, j += 4) {
            rgba[j] = rgb_data[i];
            rgba[j + 1] = rgb_data[i + 1];
            rgba[j + 2] = rgb_data[i + 2];
            rgba[j + 3] = 255;
        }

        return rgba;
    }

    void BGRASwapRB(std::vector<uint8_t>& buffer) {
        for (size_t i = 0; i + 3 < buffer.size(); i += 4) {
            std::swap(buffer[i], buffer[i + 2]);
        }
    }

    std::vector<uint8_t> L8ToRGBA(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> rgba(data.size() * 4);
    
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t gray = data[i];
        rgba[i * 4 + 0] = gray;  // R
        rgba[i * 4 + 1] = gray;  // G
        rgba[i * 4 + 2] = gray;  // B
        rgba[i * 4 + 3] = 255;   // A 
    }
    
    return rgba;
}

    PixelFormatInfo GetPixelFormatInfo(int format_code) {
        static const std::map<int, PixelFormatInfo> format_map = {
            {4, {"RGB", 3, "RGB565_LE"}},
            {6, {"RGB", 3, "RGB"}},
            {16, {"RGB", 3, "RGB565"}},
            {19, {"RGBA", 4, "ETC2_RGBA8"}},
            {40, {"RGBA", 4, "ASTC_4x4"}},
            {44, {"RGBA", 4, "ASTC_6x6"}},
            {47, {"RGBA", 4, "ASTC_8x8"}},
            {102, {"L", 1, "L8"}}
        };

        auto it = format_map.find(format_code);
        if (it != format_map.end()) {
            return it->second;
        }

        if (format_code >= 17 && format_code <= 26 && format_code != 19 && format_code != 40) {
            return { "RGBA", 4, "RGBA" };
        }

        std::vector<int> excluded = { 44, 47 };
        if (format_code >= 41 && format_code <= 53 &&
            std::find(excluded.begin(), excluded.end(), format_code) == excluded.end()) {
            return { "RGBA", 4, "COMPRESSED" };
        }

        return { "RGBA", 4, "UNKNOWN" };
    }

    bool ShouldDecompressIntelligently(const std::vector<uint8_t>& image_data,
        int width, int height, int pixel_format,
        bool verbose) {
        if (image_data.size() < 8) return false;

        int expected_astc_size;
        if (pixel_format == 40) {
            int blocks_w = (width + 3) / 4;
            int blocks_h = (height + 3) / 4;
            expected_astc_size = blocks_w * blocks_h * 16;
        }
        else {
            expected_astc_size = width * height * 2;
        }

        double size_ratio = (double)image_data.size() / expected_astc_size;

        std::vector<uint8_t> decompressed;
        double decomp_ratio = 0;
        bool lz4_works = false;

        try {
            decompressed = LZ4Decompress(image_data);
            decomp_ratio = (double)decompressed.size() / expected_astc_size;
            lz4_works = !decompressed.empty();
        }
        catch (...) {
            decomp_ratio = 0;
            lz4_works = false;
        }

        bool should_decompress = (size_ratio < 0.95 && lz4_works && decomp_ratio > size_ratio);

        if (verbose) {
            if (should_decompress) {
                std::cout << "Intelligent detection: data appears to be LZ4 compressed\n";
                std::cout << "   Size ratio: " << size_ratio << " (< 0.95)\n";
                std::cout << "   LZ4 decompression: works (" << decomp_ratio << ")\n";
            }
            else {
                std::cout << "Intelligent detection: data appears to be already decompressed\n";
                std::cout << "   Size ratio: " << size_ratio << "\n";
                if (!lz4_works)
                    std::cout << "   LZ4 decompression: fails\n";
            }
        }

        return should_decompress;
    }


    void initialize_astc() {
        static bool is_initialized = false;
        if (!is_initialized) {
            astcenc_config config;
            astcenc_error status = astcenc_config_init(
                ASTCENC_PRF_LDR,
                4, 4, 1,
                ASTCENC_PRE_FASTEST,
                0,
                &config
            );

            if (status == ASTCENC_SUCCESS) {
                astcenc_context* context;
                if (astcenc_context_alloc(&config, 1, &context) == ASTCENC_SUCCESS) {
                    astcenc_context_free(context);
                }
            }
            is_initialized = true;
        }
    }

    std::vector<uint8_t> DecodeASTC(const std::vector<uint8_t>& compressed_data,
        int width, int height, int block_width, int block_height) {


        initialize_astc();


        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
            LogError("ASTC decode: invalid dimensions");
            return std::vector<uint8_t>();
        }
        int blocks_x = (width + block_width - 1) / block_width;
        int blocks_y = (height + block_height - 1) / block_height;
        size_t expected_size = static_cast<size_t>(blocks_x) * static_cast<size_t>(blocks_y) * 16;
        if (compressed_data.size() < expected_size) {
            LogError(std::string("ASTC data too small: expected ") + std::to_string(expected_size) + ", got " + std::to_string(compressed_data.size()));
            return std::vector<uint8_t>();
        }

        astcenc_config config;
        astcenc_error status = astcenc_config_init(
            ASTCENC_PRF_LDR,
            block_width, block_height, 1,
            ASTCENC_PRE_FASTEST,
            0,
            &config
        );

        if (status != ASTCENC_SUCCESS) {
            std::cerr << "Error: astcenc_config_init failed\n";
            return std::vector<uint8_t>(width * height * 4, 128);
        }


        astcenc_context* context;
        status = astcenc_context_alloc(&config, 1, &context);
        if (status != ASTCENC_SUCCESS) {
            std::cerr << "Error: astcenc_context_alloc failed\n";
            return std::vector<uint8_t>(width * height * 4, 128);
        }


        std::vector<uint8_t> rgba(width * height * 4);


        astcenc_image image;
        image.dim_x = width;
        image.dim_y = height;
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;

        void* data_ptr = rgba.data();
        image.data = &data_ptr;


        astcenc_swizzle swizzle = { ASTCENC_SWZ_B, ASTCENC_SWZ_G, ASTCENC_SWZ_R, ASTCENC_SWZ_A };


        status = astcenc_decompress_image(
            context,
            compressed_data.data(),
            compressed_data.size(),
            &image,
            &swizzle,
            0
        );

        if (status != ASTCENC_SUCCESS) {
            std::cerr << "Error: astcenc_decompress_image failed (" << status << ")\n";
            std::fill(rgba.begin(), rgba.end(), 128);
        }


        astcenc_context_free(context);

        return rgba;
    }

    std::vector<uint8_t> DecodeETC2RGBA8(const std::vector<uint8_t>& compressed_data,
        int width, int height, bool verbose) {
        if (verbose) std::cout << "Decoding ETC2 RGBA8 with etcdec.h...\n";


        const int block_width = 4;
        const int block_height = 4;
        const int block_size = 16;


        int num_blocks_x = (width + block_width - 1) / block_width;
        int num_blocks_y = (height + block_height - 1) / block_height;


        size_t expected_size = num_blocks_x * num_blocks_y * block_size;
        if (compressed_data.size() < expected_size) {
            if (verbose) std::cerr << "Error: ETC2 data size mismatch. Expected " << expected_size << " bytes, got " << compressed_data.size() << "\n";

            return std::vector<uint8_t>(width * height * 4, 128);
        }


        std::vector<uint8_t> rgba(width * height * 4);

        uint8_t block_pixels[block_width * block_height * 4];

        const uint8_t* src_ptr = compressed_data.data();

        for (int by = 0; by < num_blocks_y; ++by) {
            for (int bx = 0; bx < num_blocks_x; ++bx) {

                etcdec_eac_rgba(src_ptr, block_pixels, 16);


                src_ptr += block_size;


                for (int y = 0; y < block_height; ++y) {
                    for (int x = 0; x < block_width; ++x) {

                        int final_x = bx * block_width + x;
                        int final_y = by * block_height + y;



                        if (final_x < width && final_y < height) {
                            size_t block_idx = (y * block_width + x) * 4;
                            size_t final_idx = (final_y * width + final_x) * 4;

                            rgba[final_idx + 0] = block_pixels[block_idx + 0];
                            rgba[final_idx + 1] = block_pixels[block_idx + 1];
                            rgba[final_idx + 2] = block_pixels[block_idx + 2];
                            rgba[final_idx + 3] = block_pixels[block_idx + 3];
                        }
                    }
                }
            }
        }

        return rgba;
    }

    std::vector<uint8_t> ConvertToPNG(const std::vector<uint8_t>& data, bool verbose) {
        try {
            Format format_type = DetectFormat(data);
            Header header;
            std::vector<uint8_t> image_data;
            PixelFormatInfo format_info;

            if (format_type == Format::SCT2) {
                header = ParseSCT2Header(data);
                size_t image_data_start = header.data_offset;
                image_data.assign(data.begin() + image_data_start, data.end());

                if (header.raw_data || header.has_alpha) {
                    if (ShouldDecompressIntelligently(image_data, header.width, header.height,
                        header.pixel_format, verbose)) {
                        try {
                            image_data = LZ4Decompress(image_data);
                            if (verbose) std::cout << "LZ4 decompression applied: " << image_data.size() << " bytes\n";
                        }
                        catch (...) {
                            if (verbose) std::cout << "Decompression failed, using raw data\n";
                        }
                    }
                }
                else if (header.pixel_format == 40 || header.compressed) {
                    try {
                        image_data = LZ4Decompress(image_data);
                        if (verbose) std::cout << "Decompression successful: " << image_data.size() << " bytes\n";
                    }
                    catch (...) {
                        if (verbose) std::cout << "Decompression failed\n";
                    }
                }

                format_info = GetPixelFormatInfo(header.pixel_format);

            }
            else if (format_type == Format::SCT) {
                header = ParseSCTHeader(data);
                size_t image_data_start = header.data_offset;
                image_data.assign(data.begin() + image_data_start, data.end());

                if (verbose) std::cout << "Decompressing data...\n";
                try {
                    image_data = LZ4Decompress(image_data);
                    if (verbose) std::cout << "Decompressed: " << image_data.size() << " bytes\n";
                }
                catch (const std::exception& e) {
                    if (verbose) std::cout << "Error during decompression: " << e.what() << "\n";
                    throw;
                }

                format_info = GetPixelFormatInfo(header.pixel_format);

            }
            else {
                if (verbose) std::cout << "Unsupported format\n";
                return {};
            }

            int width = header.width;
            int height = header.height;
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
                LogError("SCT ConvertToPNG: invalid dimensions");
                return {};
            }
            std::vector<uint8_t> final_rgba_data;
            bool has_alpha = false;

            if(format_info.type == "L8")
            {
                if (verbose) std::cout<< "Decoding L8...\n";
                final_rgba_data = L8ToRGBA(image_data);
                has_alpha = false;
            }
            else if (format_info.type == "RGB565_LE") {
                if (verbose) std::cout << "Decoding RGB565 Little Endian...\n";
                auto rgb_data = RGB565LEToRGB(image_data);
                final_rgba_data = RGBToRGBA(rgb_data);
            }
            else if (format_info.type == "ETC2_RGBA8") {
                if (verbose) std::cout << "Decoding ETC2 RGBA8...\n";
                final_rgba_data = DecodeETC2RGBA8(image_data, width, height, verbose);
                has_alpha = true;
            }
            else if (format_info.type == "ASTC_4x4") {
                if (verbose) std::cout << "Decoding ASTC 4x4...\n";
                final_rgba_data = DecodeASTC(image_data, width, height, 4, 4);
                if (final_rgba_data.empty()) { LogError("ASTC 4x4 decode failed"); return {}; }
                BGRASwapRB(final_rgba_data);
                has_alpha = true;
            }
            else if(format_info.type == "ASTC_6x6"){
                if (verbose) std::cout << "Decoding ASTC 6x6...\n";
                final_rgba_data = DecodeASTC(image_data, width, height, 6, 6);
                if(final_rgba_data.empty()) { LogError("ASTC 6x6 decode failed"); return {};}
                BGRASwapRB(final_rgba_data);
                has_alpha = true;
            }
            else if (format_info.type == "ASTC_8x8") {
                if (verbose) std::cout << "Decoding ASTC 8x8...\n";
                final_rgba_data = DecodeASTC(image_data, width, height, 8, 8);
                if (final_rgba_data.empty()) { LogError("ASTC 8x8 decode failed"); return {}; }
                BGRASwapRB(final_rgba_data);
                has_alpha = true;
            }
            else {
                if (verbose) std::cout << "Using raw " << format_info.type << " data\n";
                final_rgba_data = image_data;
                has_alpha = format_info.format.find("RGBA") != std::string::npos || header.has_alpha;
            }

            if (final_rgba_data.empty() || final_rgba_data.size() < static_cast<size_t>(width) * height * 4) {
                if (verbose) std::cout << "Error: No valid image data produced\n";
                LogError("SCT ConvertToPNG: RGBA buffer invalid size");
                return {};
            }


            std::vector<uint8_t> png_data;
            auto write_func = [](void* context, void* data, int size) {
                auto* vec = static_cast<std::vector<uint8_t>*>(context);
                uint8_t* bytes = static_cast<uint8_t*>(data);
                vec->insert(vec->end(), bytes, bytes + size);
                };

            int result = stbi_write_png_to_func(write_func, &png_data, width, height, 4,
                final_rgba_data.data(), width * 4);

            if (result == 0) {
                if (verbose) std::cout << "Error: PNG encoding failed\n";
                LogError("SCT ConvertToPNG: PNG encoding failed");
                return {};
            }

            return png_data;

        }
        catch (const std::exception& e) {
            if (verbose) std::cout << "Error during conversion: " << e.what() << "\n";
            LogError(std::string("SCT ConvertToPNG exception: ") + e.what());
            return {};
        }
    }

}
