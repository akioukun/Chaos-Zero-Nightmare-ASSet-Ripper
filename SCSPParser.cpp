#include "SCSPParser.h"
#include "json.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <map>

using json = nlohmann::ordered_json;

namespace
{
    template <typename T>
    T read_le(const uint8_t *buf, size_t offset)
    {
        T val;
        std::memcpy(&val, buf + offset, sizeof(T));
        return val;
    }

    std::string read_cstr(const uint8_t *buf, size_t start, size_t end)
    {
        if (start >= end)
            return "";
        size_t len = 0;
        while (start + len < end && buf[start + len] != 0)
        {
            len++;
        }
        return std::string(reinterpret_cast<const char *>(buf + start), len);
    }

    std::string rgba_to_hex(float r, float g, float b, float a)
    {
        auto clamp = [](float x)
        { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
        int R = (int)std::round(clamp(r) * 255.0f);
        int G = (int)std::round(clamp(g) * 255.0f);
        int B = (int)std::round(clamp(b) * 255.0f);
        int A = (int)std::round(clamp(a) * 255.0f);

        char hex[10];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X", R, G, B, A);
        return std::string(hex);
    }

    std::string rgb_to_hex(float r, float g, float b)
    {
        auto clamp = [](float x)
        { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
        int R = (int)std::round(clamp(r) * 255.0f);
        int G = (int)std::round(clamp(g) * 255.0f);
        int B = (int)std::round(clamp(b) * 255.0f);

        char hex[10];
        snprintf(hex, sizeof(hex), "%02X%02X%02X", R, G, B);
        return std::string(hex);
    }

    struct AttachmentMeta
    {
        bool weighted;
        std::vector<float> setup;
    };

    using AttachmentMetaMap = std::map<std::tuple<std::string, int, std::string>, AttachmentMeta>;

    void ParseAssignVertexAttachment(const uint8_t *buf, size_t buf_size, size_t &pos,
                                     size_t strings_base, size_t strings_end,
                                     std::vector<int16_t> &out_bones,
                                     std::vector<float> &out_verts,
                                     uint16_t &out_vcount,
                                     uint32_t &out_world_vertices_len,
                                     std::string &out_path)
    {
        out_world_vertices_len = 0;
        if (pos + 2 > buf_size)
            return;
        uint16_t bcount = read_le<uint16_t>(buf, pos);
        pos += 2;

        out_bones.clear();
        for (int i = 0; i < bcount; i++)
        {
            if (pos + 2 > buf_size)
                break;
            out_bones.push_back(read_le<int16_t>(buf, pos));
            pos += 2;
        }

        if (pos + 2 > buf_size)
            return;
        out_vcount = read_le<uint16_t>(buf, pos);
        pos += 2;

        out_verts.clear();
        for (int i = 0; i < out_vcount; i++)
        {
            if (pos + 4 > buf_size)
                break;
            out_verts.push_back(read_le<float>(buf, pos));
            pos += 4;
        }

        if (pos + 8 > buf_size)
            return;
        uint32_t world_vertices_len = read_le<uint32_t>(buf, pos);
        pos += 4;
        out_world_vertices_len = world_vertices_len;
        uint32_t path_off = read_le<uint32_t>(buf, pos);
        pos += 4;

        out_path = "";
        if (path_off != 0xFFFFFFFF && strings_base + path_off < strings_end)
        {
            out_path = read_cstr(buf, strings_base + path_off, strings_end);
        }
    }

    std::vector<float> read_f32_array(const uint8_t *buf, size_t buf_size, size_t &pos, int count)
    {
        std::vector<float> arr;
        for (int i = 0; i < count; i++)
        {
            if (pos + 4 > buf_size)
                break;
            arr.push_back(read_le<float>(buf, pos));
            pos += 4;
        }
        return arr;
    }

    bool bezier_from_spine_block(const std::vector<float> &block,
                                 float &cx1, float &cy1, float &cx2, float &cy2)
    {
        if (block.size() < 19)
            return false;

        float x0 = block[1], y0 = block[2];
        float x1 = block[3], y1 = block[4];
        float x2 = block[5], y2 = block[6];

        float dfx = x0;
        float ddfx = x1 - 2.0f * x0;
        float dddfx = x2 - 3.0f * x1 + 3.0f * x0;
        float dfy = y0;
        float ddfy = y1 - 2.0f * y0;
        float dddfy = y2 - 3.0f * y1 + 3.0f * y0;

        float h = 1.0f / 10.0f;
        float A = 3.0f * h * h;
        float B = 6.0f * h * h * h;

        float Ux = (dddfx / B - 1.0f) / 3.0f;
        float Vx = (ddfx - dddfx) / (2.0f * A);
        cx1 = -Vx - Ux;
        cx2 = -Vx - 2.0f * Ux;

        float Uy = (dddfy / B - 1.0f) / 3.0f;
        float Vy = (ddfy - dddfy) / (2.0f * A);
        cy1 = -Vy - Uy;
        cy2 = -Vy - 2.0f * Uy;

        auto clamp = [](float v)
        { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        cx1 = clamp(cx1);
        cy1 = clamp(cy1);
        cx2 = clamp(cx2);
        cy2 = clamp(cy2);

        return true;
    }

    double round_float(double value, int decimals = 6)
    {
        double rounded = std::round(value);
        if (std::abs(value - rounded) < 1e-5)
        {
            return rounded;
        }
        double multiplier = std::pow(10.0, decimals);
        return std::round(value * multiplier) / multiplier;
    }

    void maybe_add_curve(int i, const std::vector<float> &curves, json &frame)
    {
        size_t start = i * 19;
        size_t end = start + 19;
        if (end <= curves.size())
        {
            std::vector<float> b(curves.begin() + start, curves.begin() + end);
            if (b[0] == 1.0f)
            {
                frame["curve"] = "stepped";
            }
            else if (b[0] == 2.0f)
            {
                float cx1, cy1, cx2, cy2;
                if (bezier_from_spine_block(b, cx1, cy1, cx2, cy2))
                {
                    frame["curve"] = round_float(cx1);
                    frame["c2"] = round_float(cy1);
                    frame["c3"] = round_float(cx2);
                    frame["c4"] = round_float(cy2);
                }
            }
        }
    }
}

namespace SCSPParser
{

    // LZ4 Decompression (block format)
    std::vector<uint8_t> LZ4DecompressBlock(const uint8_t *src, size_t src_size, size_t uncompressed_size)
    {
        std::vector<uint8_t> out;
        out.reserve(uncompressed_size);

        size_t i = 0;
        while (i < src_size)
        {
            uint8_t token = src[i++];

            size_t lit_len = token >> 4;
            if (lit_len == 15)
            {
                uint8_t b = 255;
                while (i < src_size && b == 255)
                {
                    b = src[i++];
                    lit_len += b;
                }
            }

            if (lit_len > 0)
            {
                if (i + lit_len > src_size)
                    break;
                out.insert(out.end(), src + i, src + i + lit_len);
                i += lit_len;
                if (i >= src_size)
                    break;
            }

            if (i + 2 > src_size)
                break;
            uint16_t offset = src[i] | (src[i + 1] << 8);
            i += 2;
            if (offset == 0)
                break;

            size_t match_len = (token & 0x0F) + 4;
            if ((token & 0x0F) == 15)
            {
                uint8_t b = 255;
                while (i < src_size && b == 255)
                {
                    b = src[i++];
                    match_len += b;
                }
            }

            size_t start = out.size() - offset;
            for (size_t j = 0; j < match_len; j++)
            {
                out.push_back(out[start + j]);
            }
        }

        if (out.size() > uncompressed_size)
        {
            out.resize(uncompressed_size);
        }

        return out;
    }

    std::vector<uint8_t> DecompressSCSP(const std::vector<uint8_t> &data)
    {
        if (data.size() < 8)
        {
            throw std::runtime_error("SCSP file too small");
        }

        int32_t dec_len = read_le<int32_t>(data.data(), 0);
        int32_t comp_len = read_le<int32_t>(data.data(), 4);

        if (comp_len < 0 || dec_len <= 0)
        {
            throw std::runtime_error("Invalid SCSP header");
        }

        if (8 + comp_len > data.size())
        {
            throw std::runtime_error("Compressed block exceeds file size");
        }

        return LZ4DecompressBlock(data.data() + 8, comp_len, dec_len);
    }

    Header ParseHeader(const uint8_t *buf, size_t buf_size)
    {
        if (buf_size < 0x62)
        {
            throw std::runtime_error("Buffer too small for SCSP header");
        }

        Header hdr;
        hdr.string_offset = read_le<uint32_t>(buf, 0x00);
        hdr.string_length = read_le<uint32_t>(buf, 0x04);

        size_t strings_base = hdr.string_offset + 8;
        size_t strings_end = strings_base + hdr.string_length;

        if (strings_end > buf_size)
        {
            throw std::runtime_error("String table exceeds buffer");
        }

        // Check magic
        if (buf_size < 0x0C || memcmp(buf + 0x08, "scsp", 4) != 0)
        {
            throw std::runtime_error("Invalid SCSP magic");
        }

        hdr.hdr_version = read_le<uint32_t>(buf, 0x08 + 0x04);
        hdr.width = read_le<float>(buf, 0x08 + 0x0E);
        hdr.height = read_le<float>(buf, 0x08 + 0x12);

        auto read_string = [&](uint32_t rel) -> std::string
        {
            if (rel == 0xFFFFFFFF)
                return "";
            size_t off = strings_base + rel;
            if (off >= strings_end)
                return "";
            return read_cstr(buf, off, strings_end);
        };

        hdr.hash = read_string(read_le<uint32_t>(buf, 0x08 + 0x4A));
        hdr.version = read_string(read_le<uint32_t>(buf, 0x08 + 0x4E));
        hdr.images_path = read_string(read_le<uint32_t>(buf, 0x08 + 0x5A));
        hdr.audio_path = read_string(read_le<uint32_t>(buf, 0x08 + 0x5E));

        // Remove .scsp extension from version
        if (hdr.version.size() > 5 && hdr.version.substr(hdr.version.size() - 5) == ".scsp")
        {
            hdr.version = hdr.version.substr(0, hdr.version.size() - 5);
        }

        return hdr;
    }

    json ParseBones(const uint8_t *buf, size_t buf_size, size_t &pos,
                    size_t strings_base, size_t strings_end,
                    std::map<int16_t, std::string> &bone_names)
    {

        if (pos + 2 > buf_size)
            return json::array();

        uint16_t count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json bones = json::array();
        std::map<int16_t, std::string> transform_modes = {
            {0, "normal"}, {1, "onlyTranslation"}, {2, "noRotationOrReflection"}, {3, "noScale"}, {4, "noScaleOrReflection"}};

        for (uint16_t i = 0; i < count; i++)
        {
            if (pos + 6 > buf_size)
                break;

            int16_t index = read_le<int16_t>(buf, pos);
            pos += 2;
            uint32_t name_rel = read_le<uint32_t>(buf, pos);
            pos += 4;

            if (pos + 2 > buf_size)
                break;
            int16_t parent = read_le<int16_t>(buf, pos);
            pos += 2;

            if (pos + 32 > buf_size)
                break;
            float length = read_le<float>(buf, pos);
            float x = read_le<float>(buf, pos + 4);
            float y = read_le<float>(buf, pos + 8);
            float rot = read_le<float>(buf, pos + 12);
            float sx = read_le<float>(buf, pos + 16);
            float sy = read_le<float>(buf, pos + 20);
            float shx = read_le<float>(buf, pos + 24);
            float shy = read_le<float>(buf, pos + 28);
            pos += 32;

            if (pos + 3 > buf_size)
                break;
            uint16_t tmode = read_le<uint16_t>(buf, pos);
            pos += 2;
            bool skin = buf[pos] != 0;
            pos += 1;

            std::string name;
            if (name_rel != 0xFFFFFFFF && strings_base + name_rel < strings_end)
            {
                name = read_cstr(buf, strings_base + name_rel, strings_end);
            }

            if (name.empty())
                continue;

            bone_names[index] = name;

            json bone;
            bone["name"] = name;

            if (parent >= 0 && bone_names.count(parent))
            {
                bone["parent"] = bone_names[parent];
            }

            if (length != 0.0f)
                bone["length"] = length;
            if (x != 0.0f)
                bone["x"] = x;
            if (y != 0.0f)
                bone["y"] = y;
            if (rot != 0.0f)
                bone["rotation"] = rot;
            if (sx != 1.0f)
                bone["scaleX"] = sx;
            if (sy != 1.0f)
                bone["scaleY"] = sy;
            if (shx != 0.0f)
                bone["shearX"] = shx;
            if (shy != 0.0f)
                bone["shearY"] = shy;

            if (transform_modes.count(tmode))
            {
                bone["transform"] = transform_modes[tmode];
            }
            if (skin)
                bone["skin"] = true;

            bones.push_back(bone);
        }

        return bones;
    }

    json ParseSlots(const uint8_t *buf, size_t buf_size, size_t &pos,
                    size_t strings_base, size_t strings_end,
                    const std::map<int16_t, std::string> &bone_names,
                    std::map<int16_t, std::string> &slot_names)
    {

        if (pos + 2 > buf_size)
            return json::array();

        uint16_t count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json slots = json::array();
        std::map<uint16_t, std::string> blend_modes = {
            {0, "normal"}, {1, "additive"}, {2, "multiply"}, {3, "screen"}};

        for (uint16_t i = 0; i < count; i++)
        {
            if (pos + 2 > buf_size)
                break;

            int16_t slot_index = read_le<int16_t>(buf, pos);
            pos += 2;

            if (pos + 4 > buf_size)
                break;
            uint32_t name_rel = read_le<uint32_t>(buf, pos);
            pos += 4;

            std::string name;
            if (name_rel != 0xFFFFFFFF && strings_base + name_rel < strings_end)
            {
                name = read_cstr(buf, strings_base + name_rel, strings_end);
            }

            if (pos + 2 > buf_size)
                break;
            int16_t bone_idx = read_le<int16_t>(buf, pos);
            pos += 2;

            std::string bone_name;
            if (bone_names.count(bone_idx))
            {
                bone_name = bone_names.at(bone_idx);
            }

            if (pos + 32 > buf_size)
                break;
            float cr = read_le<float>(buf, pos + 0);
            float cg = read_le<float>(buf, pos + 4);
            float cb = read_le<float>(buf, pos + 8);
            float ca = read_le<float>(buf, pos + 12);
            pos += 16;

            float dr = read_le<float>(buf, pos + 0);
            float dg = read_le<float>(buf, pos + 4);
            float db = read_le<float>(buf, pos + 8);
            float da = read_le<float>(buf, pos + 12);
            pos += 16;

            if (pos + 1 > buf_size)
                break;
            bool hasDark = buf[pos] != 0;
            pos += 1;

            if (pos + 4 > buf_size)
                break;
            uint32_t attach_rel = read_le<uint32_t>(buf, pos);
            pos += 4;

            if (pos + 2 > buf_size)
                break;
            uint16_t blend_raw = read_le<uint16_t>(buf, pos);
            pos += 2;

            std::string attachment;
            if (attach_rel != 0xFFFFFFFF && strings_base + attach_rel < strings_end)
            {
                attachment = read_cstr(buf, strings_base + attach_rel, strings_end);
            }

            if (name.empty())
            {
                name = "slot" + std::to_string(slot_index);
            }

            slot_names[slot_index] = name;

            json slot;
            slot["name"] = name;
            slot["bone"] = bone_name;

            std::string col_hex = rgba_to_hex(cr, cg, cb, ca);
            if (col_hex != "FFFFFFFF")
            {
                slot["color"] = col_hex;
            }

            if (hasDark)
            {
                slot["dark"] = rgb_to_hex(dr, dg, db);
            }

            if (!attachment.empty())
            {
                slot["attachment"] = attachment;
            }

            std::string blend = "normal";
            if (blend_modes.count(blend_raw))
            {
                blend = blend_modes[blend_raw];
            }
            if (blend != "normal")
            {
                slot["blend"] = blend;
            }

            slots.push_back(slot);
        }

        return slots;
    }

    json ParseIKConstraints(const uint8_t *buf, size_t buf_size, size_t &pos,
                            size_t strings_base, size_t strings_end,
                            const std::map<int16_t, std::string> &bone_names,
                            std::map<int, std::string> &ik_names)
    {

        if (pos + 2 > buf_size)
            return json::array();

        uint16_t count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json iks = json::array();

        for (int i = 0; i < count; i++)
        {
            if (pos + 4 > buf_size)
                break;

            uint32_t name_rel = read_le<uint32_t>(buf, pos);
            pos += 4;
            std::string name;
            if (name_rel != 0xFFFFFFFF && strings_base + name_rel < strings_end)
            {
                name = read_cstr(buf, strings_base + name_rel, strings_end);
            }
            if (name.empty())
                name = "ik" + std::to_string(i);

            if (pos + 4 > buf_size)
                break;
            uint32_t order = read_le<uint32_t>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool skinRequired = buf[pos] != 0;
            pos += 1;

            if (pos + 4 > buf_size)
                break;
            int32_t bendDirection = read_le<int32_t>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool compress = buf[pos] != 0;
            pos += 1;

            if (pos + 8 > buf_size)
                break;
            float mix = read_le<float>(buf, pos);
            pos += 4;
            float softness = read_le<float>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool stretch = buf[pos] != 0;
            pos += 1;

            if (pos + 1 > buf_size)
                break;
            bool uniform = buf[pos] != 0;
            pos += 1;

            if (pos + 2 > buf_size)
                break;
            int16_t t_idx = read_le<int16_t>(buf, pos);
            pos += 2;
            std::string target_name;
            if (t_idx >= 0 && bone_names.count(t_idx))
            {
                target_name = bone_names.at(t_idx);
            }

            if (pos + 2 > buf_size)
                break;
            uint16_t nb = read_le<uint16_t>(buf, pos);
            pos += 2;

            json bnames = json::array();
            for (uint16_t j = 0; j < nb; j++)
            {
                if (pos + 2 > buf_size)
                    break;
                int16_t bidx = read_le<int16_t>(buf, pos);
                pos += 2;
                if (bidx >= 0 && bone_names.count(bidx))
                {
                    bnames.push_back(bone_names.at(bidx));
                }
            }

            json ik;
            ik["name"] = name;
            ik["order"] = (int)order;
            ik["skin"] = skinRequired;
            ik["bones"] = bnames;
            ik["target"] = target_name;
            ik["mix"] = mix;
            ik["softness"] = softness;
            ik["bendPositive"] = (bendDirection >= 0);
            if (compress)
                ik["compress"] = true;
            if (stretch)
                ik["stretch"] = true;
            if (uniform)
                ik["uniform"] = true;

            ik_names[i] = name;
            iks.push_back(ik);
        }

        return iks;
    }

    json ParseTransformConstraints(const uint8_t *buf, size_t buf_size, size_t &pos,
                                   size_t strings_base, size_t strings_end,
                                   const std::map<int16_t, std::string> &bone_names,
                                   std::map<int, std::string> &transform_names)
    {

        if (pos + 2 > buf_size)
            return json::array();

        uint16_t count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json transforms = json::array();

        for (int i = 0; i < count; i++)
        {
            if (pos + 4 > buf_size)
                break;

            uint32_t name_off = read_le<uint32_t>(buf, pos);
            pos += 4;
            std::string name;
            if (name_off != 0xFFFFFFFF && strings_base + name_off < strings_end)
            {
                name = read_cstr(buf, strings_base + name_off, strings_end);
            }
            if (name.empty())
                name = "transform" + std::to_string(i);

            if (pos + 4 > buf_size)
                break;
            uint32_t order = read_le<uint32_t>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool skin_req = buf[pos] != 0;
            pos += 1;

            if (pos + 44 > buf_size)
                break;
            float rotateMix = read_le<float>(buf, pos);
            pos += 4;
            float translateMix = read_le<float>(buf, pos);
            pos += 4;
            float scaleMix = read_le<float>(buf, pos);
            pos += 4;
            float shearMix = read_le<float>(buf, pos);
            pos += 4;
            float offsetRotation = read_le<float>(buf, pos);
            pos += 4;
            float offsetX = read_le<float>(buf, pos);
            pos += 4;
            float offsetY = read_le<float>(buf, pos);
            pos += 4;
            float offsetScaleX = read_le<float>(buf, pos);
            pos += 4;
            float offsetScaleY = read_le<float>(buf, pos);
            pos += 4;
            float offsetShearY = read_le<float>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool isRelative = buf[pos] != 0;
            pos += 1;

            if (pos + 1 > buf_size)
                break;
            bool isLocal = buf[pos] != 0;
            pos += 1;

            if (pos + 2 > buf_size)
                break;
            int16_t tgt = read_le<int16_t>(buf, pos);
            pos += 2;
            std::string tgt_name = "root";
            if (tgt >= 0 && bone_names.count(tgt))
            {
                tgt_name = bone_names.at(tgt);
            }

            if (pos + 2 > buf_size)
                break;
            uint16_t bc = read_le<uint16_t>(buf, pos);
            pos += 2;

            json blist = json::array();
            for (uint16_t j = 0; j < bc; j++)
            {
                if (pos + 2 > buf_size)
                    break;
                int16_t bi = read_le<int16_t>(buf, pos);
                pos += 2;
                if (bi >= 0 && bone_names.count(bi))
                {
                    blist.push_back(bone_names.at(bi));
                }
                else
                {
                    blist.push_back("root");
                }
            }

            json tr;
            tr["name"] = name;
            tr["order"] = (int)order;
            tr["skin"] = skin_req;
            tr["target"] = tgt_name;
            tr["bones"] = blist;
            tr["rotateMix"] = rotateMix;
            tr["translateMix"] = translateMix;
            tr["scaleMix"] = scaleMix;
            tr["shearMix"] = shearMix;
            tr["rotation"] = offsetRotation;
            tr["x"] = offsetX;
            tr["y"] = offsetY;
            tr["scaleX"] = offsetScaleX;
            tr["scaleY"] = offsetScaleY;
            tr["shearY"] = offsetShearY;
            tr["relative"] = isRelative;
            tr["local"] = isLocal;

            transform_names[i] = name;
            transforms.push_back(tr);
        }

        return transforms;
    }

    json ParsePathConstraints(const uint8_t *buf, size_t buf_size, size_t &pos,
                              size_t strings_base, size_t strings_end,
                              const std::map<int16_t, std::string> &bone_names,
                              const std::map<int16_t, std::string> &slot_names,
                              std::map<int, std::string> &path_names)
    {

        if (pos + 2 > buf_size)
            return json::array();

        uint16_t count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json paths = json::array();
        std::map<uint16_t, std::string> pos_modes = {{0, "fixed"}, {1, "percent"}};
        std::map<uint16_t, std::string> spacing_modes = {{0, "length"}, {1, "fixed"}, {2, "percent"}};
        std::map<uint16_t, std::string> rotate_modes = {{0, "tangent"}, {1, "chain"}, {2, "chainScale"}};

        for (int i = 0; i < count; i++)
        {
            if (pos + 4 > buf_size)
                break;

            uint32_t name_off = read_le<uint32_t>(buf, pos);
            pos += 4;
            std::string name;
            if (name_off != 0xFFFFFFFF && strings_base + name_off < strings_end)
            {
                name = read_cstr(buf, strings_base + name_off, strings_end);
            }
            if (name.empty())
                name = "path" + std::to_string(i);

            if (pos + 4 > buf_size)
                break;
            uint32_t order = read_le<uint32_t>(buf, pos);
            pos += 4;

            if (pos + 1 > buf_size)
                break;
            bool skin_req = buf[pos] != 0;
            pos += 1;

            if (pos + 6 > buf_size)
                break;
            uint16_t positionMode = read_le<uint16_t>(buf, pos);
            pos += 2;
            uint16_t spacingMode = read_le<uint16_t>(buf, pos);
            pos += 2;
            uint16_t rotateMode = read_le<uint16_t>(buf, pos);
            pos += 2;

            if (pos + 20 > buf_size)
                break;
            float offsetRotation = read_le<float>(buf, pos);
            pos += 4;
            float position = read_le<float>(buf, pos);
            pos += 4;
            float spacing = read_le<float>(buf, pos);
            pos += 4;
            float rotateMix = read_le<float>(buf, pos);
            pos += 4;
            float translateMix = read_le<float>(buf, pos);
            pos += 4;

            if (pos + 2 > buf_size)
                break;
            int16_t tgt = read_le<int16_t>(buf, pos);
            pos += 2;
            std::string tgt_name = "slot0";
            if (tgt >= 0 && slot_names.count(tgt))
            {
                tgt_name = slot_names.at(tgt);
            }

            if (pos + 2 > buf_size)
                break;
            uint16_t bc = read_le<uint16_t>(buf, pos);
            pos += 2;

            json blist = json::array();
            for (uint16_t j = 0; j < bc; j++)
            {
                if (pos + 2 > buf_size)
                    break;
                int16_t bi = read_le<int16_t>(buf, pos);
                pos += 2;
                if (bi >= 0 && bone_names.count(bi))
                {
                    blist.push_back(bone_names.at(bi));
                }
                else
                {
                    blist.push_back("root");
                }
            }

            json pc;
            pc["name"] = name;
            pc["order"] = (int)order;
            pc["skin"] = skin_req;
            pc["positionMode"] = pos_modes.count(positionMode) ? pos_modes[positionMode] : "percent";
            pc["spacingMode"] = spacing_modes.count(spacingMode) ? spacing_modes[spacingMode] : "length";
            pc["rotateMode"] = rotate_modes.count(rotateMode) ? rotate_modes[rotateMode] : "tangent";
            pc["rotation"] = offsetRotation;
            pc["position"] = position;
            pc["spacing"] = spacing;
            pc["rotateMix"] = rotateMix;
            pc["translateMix"] = translateMix;
            pc["target"] = tgt_name;
            pc["bones"] = blist;

            path_names[i] = name;
            paths.push_back(pc);
        }

        return paths;
    }

    json ParseSkins(const uint8_t *buf, size_t buf_size, size_t &pos,
                    size_t strings_base, size_t strings_end,
                    const std::map<int16_t, std::string> &slot_names,
                    AttachmentMetaMap &attachment_meta,
                    int hdr_version,
                    std::map<int, std::string> &skin_names)
    {

        json skins = json::array();

        if (pos + 2 > buf_size)
            return skins;
        uint16_t skin_count = read_le<uint16_t>(buf, pos);
        pos += 2;

        for (int sidx = 0; sidx < skin_count; sidx++)
        {
            std::string name = "default";
            if (pos + 4 > buf_size)
                break;
            uint32_t off = read_le<uint32_t>(buf, pos);
            pos += 4;
            if (off != 0xFFFFFFFF && strings_base + off < strings_end)
            {
                std::string s = read_cstr(buf, strings_base + off, strings_end);
                if (!s.empty())
                    name = s;
            }
            skin_names[sidx] = name;

            if (pos + 2 > buf_size)
                break;
            uint16_t bc = read_le<uint16_t>(buf, pos);
            pos += 2;
            pos += 2 * bc;

            if (pos + 2 > buf_size)
                break;
            uint16_t cc = read_le<uint16_t>(buf, pos);
            pos += 2;
            for (int k = 0; k < cc; k++)
            {
                if (pos + 4 > buf_size)
                    break;
                pos += 4;
            }

            json attachments_json = json::object();

            if (pos + 2 > buf_size)
                break;
            uint16_t ac = read_le<uint16_t>(buf, pos);
            pos += 2;

            for (int a = 0; a < ac; a++)
            {
                if (pos + 2 > buf_size)
                    break;
                uint16_t slot_idx = read_le<uint16_t>(buf, pos);
                pos += 2;
                std::string slot_name = "slot" + std::to_string(slot_idx);
                if (slot_names.count(slot_idx))
                    slot_name = slot_names.at(slot_idx);

                if (pos + 4 > buf_size)
                    break;
                uint32_t name_off = read_le<uint32_t>(buf, pos);
                pos += 4;
                std::string att_name = "att" + std::to_string(a);
                if (name_off != 0xFFFFFFFF && strings_base + name_off < strings_end)
                {
                    std::string s = read_cstr(buf, strings_base + name_off, strings_end);
                    if (!s.empty())
                        att_name = s;
                }

                if (pos + 2 > buf_size)
                    break;
                int16_t atype = read_le<int16_t>(buf, pos);
                pos += 2;

                if (pos + 4 > buf_size)
                    break;
                read_le<uint32_t>(buf, pos);
                pos += 4; // ctor_name unused

                json att;

                if (atype == 0)
                { 
                    // Region
                    if (pos + 24 > buf_size)
                        break;
                    float x = read_le<float>(buf, pos);
                    pos += 4;
                    float y = read_le<float>(buf, pos);
                    pos += 4;
                    float rot = read_le<float>(buf, pos);
                    pos += 4;
                    float sx = read_le<float>(buf, pos);
                    pos += 4;
                    float sy = read_le<float>(buf, pos);
                    pos += 4;
                    float w = read_le<float>(buf, pos);
                    pos += 4;
                    float h = read_le<float>(buf, pos);
                    pos += 4;

                    // Skip 6 floats (24 bytes) before vc/uc
                    pos += 24;

                    // vc, uc
                    if (pos + 2 > buf_size)
                        break;
                    uint16_t vc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    pos += 4 * vc;
                    if (pos + 2 > buf_size)
                        break;
                    uint16_t uc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    pos += 4 * uc;

                    // path
                    if (pos + 4 > buf_size)
                        break;
                    uint32_t poff = read_le<uint32_t>(buf, pos);
                    pos += 4;
                    std::string path;
                    if (poff != 0xFFFFFFFF && strings_base + poff < strings_end)
                    {
                        path = read_cstr(buf, strings_base + poff, strings_end);
                    }

                    // color
                    if (pos + 16 > buf_size)
                        break;
                    float cr = read_le<float>(buf, pos);
                    pos += 4;
                    float cg = read_le<float>(buf, pos);
                    pos += 4;
                    float cb = read_le<float>(buf, pos);
                    pos += 4;
                    float ca = read_le<float>(buf, pos);
                    pos += 4;

                    att["type"] = "region";
                    att["x"] = x;
                    att["y"] = y;
                    att["rotation"] = rot;
                    att["scaleX"] = sx;
                    att["scaleY"] = sy;
                    att["width"] = w;
                    att["height"] = h;
                    if (!path.empty())
                        att["path"] = path;
                    std::string color = rgba_to_hex(cr, cg, cb, ca);
                    if (color != "FFFFFFFF")
                        att["color"] = color;
                }
                else if (atype == 1)
                {
                    // Bounding Box
                    std::vector<int16_t> bones;
                    std::vector<float> verts;
                    uint16_t vcount = 0;
                    uint32_t world_vertices_len = 0;
                    std::string vpath;
                    ParseAssignVertexAttachment(buf, buf_size, pos, strings_base, strings_end, bones, verts, vcount, world_vertices_len, vpath);

                    bool is_weighted = !bones.empty();
                    attachment_meta[{name, slot_idx, att_name}] = {is_weighted, is_weighted ? std::vector<float>() : verts};

                    att["type"] = "boundingbox";
                    att["vertexCount"] = (int)world_vertices_len >> 1;

                    if (!bones.empty())
                    {
                        json jv = json::array();
                        int i = 0, vf = 0;
                        while (i < (int)bones.size())
                        {
                            int c = bones[i++];
                            jv.push_back(c);
                            for (int k = 0; k < c; k++)
                            {
                                jv.push_back(bones[i++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                            }
                        }
                        att["vertices"] = jv;
                    }
                    else
                    {
                        att["vertices"] = verts;
                    }

                    if (!vpath.empty())
                        att["path"] = vpath;
                }
                else if (atype == 2 || atype == 3)
                {
                    // Mesh or Linked Mesh
                    std::vector<int16_t> bones;
                    std::vector<float> verts;
                    uint16_t vcount_dummy;
                    uint32_t world_vertices_len_dummy = 0;
                    std::string vpath;
                    ParseAssignVertexAttachment(buf, buf_size, pos, strings_base, strings_end, bones, verts, vcount_dummy, world_vertices_len_dummy, vpath);

                    bool is_weighted = !bones.empty();
                    attachment_meta[{name, slot_idx, att_name}] = {is_weighted, is_weighted ? std::vector<float>() : verts};

                    pos += 4 * 6; // Skip 24 bytes

                    uint16_t uvc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> uvs = read_f32_array(buf, buf_size, pos, uvc);

                    uint16_t ruvc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> regionUVs = read_f32_array(buf, buf_size, pos, ruvc);

                    uint16_t tc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<uint16_t> triangles;
                    for (int i = 0; i < tc; i++)
                    {
                        triangles.push_back(read_le<uint16_t>(buf, pos));
                        pos += 2;
                    }

                    uint16_t ec = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<uint16_t> edges;
                    for (int i = 0; i < ec; i++)
                    {
                        edges.push_back(read_le<uint16_t>(buf, pos));
                        pos += 2;
                    }

                    std::string mpath = vpath;
                    uint32_t moff = read_le<uint32_t>(buf, pos);
                    pos += 4;
                    if (moff != 0xFFFFFFFF && strings_base + moff < strings_end)
                    {
                        std::string s = read_cstr(buf, strings_base + moff, strings_end);
                        if (!s.empty())
                            mpath = s;
                    }

                    float regionU = read_le<float>(buf, pos);
                    pos += 4;
                    float regionV = read_le<float>(buf, pos);
                    pos += 4;
                    float regionU2 = read_le<float>(buf, pos);
                    pos += 4;
                    float regionV2 = read_le<float>(buf, pos);
                    pos += 4;
                    float width = read_le<float>(buf, pos);
                    pos += 4;
                    float height = read_le<float>(buf, pos);
                    pos += 4;

                    float cr = read_le<float>(buf, pos);
                    pos += 4;
                    float cg = read_le<float>(buf, pos);
                    pos += 4;
                    float cb = read_le<float>(buf, pos);
                    pos += 4;
                    float ca = read_le<float>(buf, pos);
                    pos += 4;

                    uint32_t hull = read_le<uint32_t>(buf, pos);
                    pos += 4;
                    bool regionRotate = buf[pos] != 0;
                    pos += 1;
                    pos += 4; // _deg

                    uint32_t parent_off = read_le<uint32_t>(buf, pos);
                    pos += 4;
                    std::string parent_name;
                    if (parent_off != 0xFFFFFFFF && strings_base + parent_off < strings_end)
                    {
                        parent_name = read_cstr(buf, strings_base + parent_off, strings_end);
                    }

                    json out_verts_json;
                    if (!bones.empty())
                    {
                        json jv = json::array();
                        int i = 0, vf = 0;
                        while (i < (int)bones.size())
                        {
                            int c = bones[i++];
                            jv.push_back(c);
                            for (int k = 0; k < c; k++)
                            {
                                jv.push_back(bones[i++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                            }
                        }
                        out_verts_json = jv;
                    }
                    else
                    {
                        out_verts_json = verts;
                    }

                    if (atype == 3)
                    {
                        // Linked Mesh
                        int16_t temp_skin_idx = 0;
                        std::string temp_skin_name = "";
                        if (hdr_version > 0x7530)
                        {
                            pos += 2;
                        }
                        else
                        {
                            temp_skin_idx = read_le<int16_t>(buf, pos);
                            pos += 2;
                            uint32_t soff = read_le<uint32_t>(buf, pos);
                            pos += 4;
                            if (soff != 0xFFFFFFFF && strings_base + soff < strings_end)
                            {
                                temp_skin_name = read_cstr(buf, strings_base + soff, strings_end);
                            }
                        }

                        int16_t skin_idx = read_le<int16_t>(buf, pos);
                        pos += 2;
                        bool deform_flag = buf[pos] != 0;
                        pos += 1;

                        att["type"] = "linkedmesh";
                        att["parent"] = (!parent_name.empty()) ? parent_name : att_name;
                        att["deform"] = deform_flag;
                        att["uvs"] = regionUVs;
                        att["triangles"] = triangles;
                        att["vertices"] = out_verts_json;
                        att["hull"] = hull;
                        att["edges"] = edges;
                        att["width"] = width;
                        att["height"] = height;

                        if (hdr_version > 0x7530)
                        {
                            att["skinIndex"] = skin_idx;
                        }
                        else
                        {
                            att["skin"] = temp_skin_name.empty() ? "default" : temp_skin_name;
                        }
                        if (!mpath.empty())
                            att["path"] = mpath;
                        // att["color"] = rgba_to_hex(cr, cg, cb, ca);
                    }
                    else
                    {
                        // Mesh
                        pos += 5; // skip 5 bytes
                        att["type"] = "mesh";
                        att["uvs"] = regionUVs;
                        att["triangles"] = triangles;
                        att["vertices"] = out_verts_json;
                        att["hull"] = hull;
                        att["edges"] = edges;
                        att["width"] = width;
                        att["height"] = height;
                        if (!mpath.empty())
                            att["path"] = mpath;
                        // att["color"] = rgba_to_hex(cr, cg, cb, ca);
                    }
                }
                else if (atype == 4)
                {
                    // Path
                    std::vector<int16_t> bones;
                    std::vector<float> verts;
                    uint16_t vcount = 0;
                    uint32_t world_vertices_len = 0;
                    std::string vpath;
                    ParseAssignVertexAttachment(buf, buf_size, pos, strings_base, strings_end, bones, verts, vcount, world_vertices_len, vpath);

                    bool is_weighted = !bones.empty();
                    attachment_meta[{name, slot_idx, att_name}] = {is_weighted, is_weighted ? std::vector<float>() : verts};

                    uint16_t cnt = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> lengths = read_f32_array(buf, buf_size, pos, cnt);
                    bool closed = buf[pos] != 0;
                    pos += 1;
                    bool constantSpeed = buf[pos] != 0;
                    pos += 1;

                    att["type"] = "path";
                    att["closed"] = closed;
                    att["constantSpeed"] = constantSpeed;
                    att["lengths"] = lengths;
                    att["vertexCount"] = (int)world_vertices_len >> 1;

                    if (!bones.empty())
                    {
                        json jv = json::array();
                        int i = 0, vf = 0;
                        while (i < (int)bones.size())
                        {
                            int c = bones[i++];
                            jv.push_back(c);
                            for (int k = 0; k < c; k++)
                            {
                                jv.push_back(bones[i++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                            }
                        }
                        att["vertices"] = jv;
                    }
                    else
                    {
                        att["vertices"] = verts;
                    }
                    if (!vpath.empty())
                        att["path"] = vpath;
                }
                else if (atype == 5)
                {
                    // Point
                    float x = read_le<float>(buf, pos);
                    pos += 4;
                    float y = read_le<float>(buf, pos);
                    pos += 4;
                    float rotation = read_le<float>(buf, pos);
                    pos += 4;
                    pos += 4;

                    att["type"] = "point";
                    att["x"] = x;
                    att["y"] = y;
                    att["rotation"] = rotation;
                }
                else if (atype == 6)
                {
                    // Clipping
                    std::vector<int16_t> bones;
                    std::vector<float> verts;
                    uint16_t vcount = 0;
                    uint32_t world_vertices_len = 0;
                    std::string vpath;
                    ParseAssignVertexAttachment(buf, buf_size, pos, strings_base, strings_end, bones, verts, vcount, world_vertices_len, vpath);

                    bool is_weighted = !bones.empty();
                    attachment_meta[{name, slot_idx, att_name}] = {is_weighted, is_weighted ? std::vector<float>() : verts};

                    int16_t end_slot_idx = read_le<int16_t>(buf, pos);
                    pos += 2;
                    std::string end_slot_name = "slot" + std::to_string(end_slot_idx);
                    if (slot_names.count(end_slot_idx))
                        end_slot_name = slot_names.at(end_slot_idx);

                    att["type"] = "clipping";
                    att["end"] = end_slot_name;
                    att["vertexCount"] = (int)world_vertices_len >> 1;

                    if (!bones.empty())
                    {
                        json jv = json::array();
                        int i = 0, vf = 0;
                        while (i < (int)bones.size())
                        {
                            int c = bones[i++];
                            jv.push_back(c);
                            for (int k = 0; k < c; k++)
                            {
                                jv.push_back(bones[i++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                                jv.push_back(verts[vf++]);
                            }
                        }
                        att["vertices"] = jv;
                    }
                    else
                    {
                        att["vertices"] = verts;
                    }
                    if (!vpath.empty())
                        att["path"] = vpath;
                }

                if (!att.empty())
                {
                    if (!attachments_json.contains(slot_name))
                        attachments_json[slot_name] = json::object();
                    attachments_json[slot_name][att_name] = att;
                }
            }

            json skin_obj;
            skin_obj["name"] = name;
            skin_obj["attachments"] = attachments_json;
            skins.push_back(skin_obj);
        }

        // Post-process linked mesh skin indices
        for (auto &skin : skins)
        {
            if (skin.contains("attachments"))
            {
                for (auto &[slot, slots] : skin["attachments"].items())
                {
                    for (auto &[att_name, att] : slots.items())
                    {
                        if (att.contains("type") && att["type"] == "linkedmesh" && att.contains("skinIndex"))
                        {
                            int idx = att["skinIndex"];
                            if (skin_names.count(idx))
                            {
                                att["skin"] = skin_names[idx];
                            }
                            else
                            {
                                att["skin"] = "default";
                            }
                            att.erase("skinIndex");
                        }
                    }
                }
            }
        }

        return skins;
    }

    json ParseEvents(const uint8_t *buf, size_t buf_size, size_t &pos,
                     size_t strings_base, size_t strings_end,
                     std::map<int, std::string> &event_names)
    {

        if (pos + 2 > buf_size)
            return json::object();

        uint16_t event_count = read_le<uint16_t>(buf, pos);
        pos += 2;

        json events = json::object();

        for (uint16_t e = 0; e < event_count; e++)
        {
            if (pos + 4 > buf_size)
                break;

            uint32_t name_off = read_le<uint32_t>(buf, pos);
            pos += 4;
            std::string name;
            if (name_off != 0xFFFFFFFF && strings_base + name_off < strings_end)
            {
                name = read_cstr(buf, strings_base + name_off, strings_end);
            }

            if (pos + 12 > buf_size)
                break;
            uint32_t intData = read_le<uint32_t>(buf, pos);
            pos += 4;
            float floatData = read_le<float>(buf, pos);
            pos += 4;
            uint32_t stringOff = read_le<uint32_t>(buf, pos);
            pos += 4;

            std::string stringData;
            if (stringOff != 0xFFFFFFFF && strings_base + stringOff < strings_end)
            {
                stringData = read_cstr(buf, strings_base + stringOff, strings_end);
            }

            if (pos + 12 > buf_size)
                break;
            uint32_t audioOff = read_le<uint32_t>(buf, pos);
            pos += 4;

            std::string audioData;
            if (audioOff != 0xFFFFFFFF && strings_base + audioOff < strings_end)
            {
                audioData = read_cstr(buf, strings_base + audioOff, strings_end);
            }

            float volume = read_le<float>(buf, pos);
            pos += 4;
            float balance = read_le<float>(buf, pos);
            pos += 4;

            json evt;
            evt["int"] = intData;
            evt["float"] = floatData;
            evt["string"] = stringData;
            evt["audio"] = audioData;
            evt["volume"] = volume;
            evt["balance"] = balance;

            if (!name.empty())
            {
                events[name] = evt;
                event_names[e] = name;
            }
        }

        return events;
    }

    json ParseAnimations(const uint8_t *buf, size_t buf_size, size_t &pos,
                         size_t strings_base, size_t strings_end,
                         const std::map<int16_t, std::string> &bone_names,
                         const std::map<int16_t, std::string> &slot_names,
                         const std::map<int, std::string> &skin_names,
                         const std::map<int, std::string> &ik_names,
                         const std::map<int, std::string> &transform_names,
                         const std::map<int, std::string> &path_names,
                         const std::map<int, std::string> &event_names,
                         const AttachmentMetaMap &attachment_meta,
                         int hdr_version)
    {

        json animations = json::object();
        if (pos + 2 > buf_size)
            return animations;

        uint16_t anim_count = read_le<uint16_t>(buf, pos);
        pos += 2;

        for (uint16_t ai = 0; ai < anim_count; ai++)
        {
            if (pos + 8 > buf_size)
                break;
            uint32_t name_off = read_le<uint32_t>(buf, pos);
            pos += 4;
            float duration = read_le<float>(buf, pos);
            pos += 4;

            std::string name = "anim" + std::to_string(ai);
            if (name_off != 0xFFFFFFFF && strings_base + name_off < strings_end)
            {
                name = read_cstr(buf, strings_base + name_off, strings_end);
            }

            json anim;
            anim["bones"] = json::object();
            anim["slots"] = json::object();
            anim["ik"] = json::object();
            anim["transform"] = json::object();
            anim["path"] = json::object();
            anim["deform"] = json::object();
            anim["events"] = json::array();

            if (pos + 2 > buf_size)
                break;
            uint16_t timeline_count = read_le<uint16_t>(buf, pos);
            pos += 2;

            for (int k = 0; k < timeline_count; k++)
            {
                if (pos + 2 > buf_size)
                    break;
                uint16_t ttype = read_le<uint16_t>(buf, pos);
                pos += 2;

                if (ttype <= 3)
                {
                    // Rotate, Translate, Scale, Shear
                    uint16_t bone_idx = read_le<uint16_t>(buf, pos);
                    pos += 2;

                    // Read frames
                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> values = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t cc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> curves = read_f32_array(buf, buf_size, pos, cc);

                    std::string bkey = std::to_string(bone_idx);
                    if (bone_names.count(bone_idx))
                        bkey = bone_names.at(bone_idx);

                    std::string tname_str;
                    if (ttype == 0)
                        tname_str = "rotate";
                    else if (ttype == 1)
                        tname_str = "translate";
                    else if (ttype == 2)
                        tname_str = "scale";
                    else
                        tname_str = "shear";

                    if (!anim["bones"].contains(bkey))
                        anim["bones"][bkey] = json::object();
                    if (!anim["bones"][bkey].contains(tname_str))
                        anim["bones"][bkey][tname_str] = json::array();

                    int valCount = (ttype == 0) ? 2 : 3;
                    int frameCount = fc / valCount;

                    for (int i = 0; i < frameCount; i++)
                    {
                        json fr;
                        if (ttype == 0)
                        {
                            fr["time"] = values[i * 2];
                            fr["angle"] = values[i * 2 + 1];
                        }
                        else
                        {
                            fr["time"] = values[i * 3];
                            fr["x"] = values[i * 3 + 1];
                            fr["y"] = values[i * 3 + 2];
                        }
                        maybe_add_curve(i, curves, fr);
                        anim["bones"][bkey][tname_str].push_back(fr);
                    }
                }
                else if (ttype == 4)
                {
                    // Attachment
                    uint16_t slot_idx = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> times = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t name_cnt = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<std::string> names;
                    for (int i = 0; i < name_cnt; i++)
                    {
                        uint32_t soff = read_le<uint32_t>(buf, pos);
                        pos += 4;
                        if (soff != 0xFFFFFFFF && strings_base + soff < strings_end)
                        {
                            names.push_back(read_cstr(buf, strings_base + soff, strings_end));
                        }
                        else
                        {
                            names.push_back("");
                        }
                    }

                    std::string sname = std::to_string(slot_idx);
                    if (slot_names.count(slot_idx))
                        sname = slot_names.at(slot_idx);

                    if (!anim["slots"].contains(sname))
                        anim["slots"][sname] = json::object();
                    if (!anim["slots"][sname].contains("attachment"))
                        anim["slots"][sname]["attachment"] = json::array();

                    int count = std::min((int)fc, (int)names.size());
                    for (int i = 0; i < count; i++)
                    {
                        json fr;
                        fr["time"] = times[i];
                        if (!names[i].empty())
                            fr["name"] = names[i];
                        else
                            fr["name"] = nullptr;
                        anim["slots"][sname]["attachment"].push_back(fr);
                    }
                }
                else if (ttype == 6)
                {
                    // Deform
                    uint16_t slot_idx = read_le<uint16_t>(buf, pos);
                    pos += 2;

                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> values = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t cc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> curves = read_f32_array(buf, buf_size, pos, cc);

                    uint16_t fv_frames = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<std::vector<float>> frame_vertices;
                    for (int i = 0; i < fv_frames; i++)
                    {
                        uint16_t cnt = read_le<uint16_t>(buf, pos);
                        pos += 2;
                        frame_vertices.push_back(read_f32_array(buf, buf_size, pos, cnt));
                    }

                    uint32_t att_off = read_le<uint32_t>(buf, pos);
                    pos += 4;
                    std::string att_name = "";
                    if (att_off != 0xFFFFFFFF && strings_base + att_off < strings_end)
                        att_name = read_cstr(buf, strings_base + att_off, strings_end);

                    std::string skinname = "default";
                    if (hdr_version > 0x7530 && pos + 2 <= buf_size)
                    {
                        uint16_t sidx = read_le<uint16_t>(buf, pos);
                        pos += 2;
                        if (skin_names.count(sidx))
                            skinname = skin_names.at(sidx);
                    }

                    // Process Deform
                    bool is_unweighted = true;
                    std::vector<float> setup;
                    auto key = std::make_tuple(skinname, (int)slot_idx, att_name);
                    if (attachment_meta.count(key))
                    {
                        is_unweighted = !attachment_meta.at(key).weighted;
                        setup = attachment_meta.at(key).setup;
                    }

                    std::string sname = std::to_string(slot_idx);
                    if (slot_names.count(slot_idx))
                        sname = slot_names.at(slot_idx);

                    if (!anim["deform"].contains(skinname))
                        anim["deform"][skinname] = json::object();
                    if (!anim["deform"][skinname].contains(sname))
                        anim["deform"][skinname][sname] = json::object();
                    if (!anim["deform"][skinname][sname].contains(att_name))
                        anim["deform"][skinname][sname][att_name] = json::array();

                    int n = std::min((int)fc, (int)fv_frames);
                    for (int i = 0; i < n; i++)
                    {
                        json fr;
                        fr["time"] = values[i];
                        const auto &verts = frame_vertices[i];
                        if (!verts.empty())
                        {
                            std::vector<float> diffs;
                            if (is_unweighted && setup.size() == verts.size())
                            {
                                for (size_t k = 0; k < verts.size(); k++)
                                    diffs.push_back(verts[k] - setup[k]);
                            }
                            else
                            {
                                diffs = verts;
                            }

                            int start = 0;
                            while (start < (int)diffs.size() && std::abs(diffs[start]) < 1e-6)
                                start++;
                            if (start < (int)diffs.size())
                            {
                                int end = (int)diffs.size() - 1;
                                while (end >= 0 && std::abs(diffs[end]) < 1e-6)
                                    end--;

                                json v = json::array();
                                for (int k = start; k <= end; k++)
                                    v.push_back(diffs[k]);
                                fr["vertices"] = v;
                                if (start > 0)
                                    fr["offset"] = start;
                            }
                        }
                        maybe_add_curve(i, curves, fr);
                        anim["deform"][skinname][sname][att_name].push_back(fr);
                    }
                }
                else if (ttype == 7)
                {
                    // Events timeline (consume only)
                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> times = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t evc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    for (int i = 0; i < evc; i++)
                    {
                        (void)read_le<uint32_t>(buf, pos);
                        pos += 4;
                    }
                }
                else if (ttype == 8)
                {
                    // DrawOrder
                    int slot_count = (int)slot_names.size();

                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> times = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t groups = read_le<uint16_t>(buf, pos);
                    pos += 2;

                    json drawOrder = json::array();
                    for (int i = 0; i < groups; i++)
                    {
                        uint16_t c = read_le<uint16_t>(buf, pos);
                        pos += 2;
                        json fr;
                        fr["time"] = (i < (int)times.size()) ? times[i] : 0.0f;

                        json offsets = json::array();
                        if (c == slot_count)
                        {
                            std::vector<int> new_order;
                            for (int j = 0; j < c; j++)
                            {
                                new_order.push_back((int)read_le<uint32_t>(buf, pos));
                                pos += 4;
                            }
                            // Map new positions
                            for (int orig = 0; orig < slot_count; orig++)
                            {
                                int new_pos = -1;
                                for (int p = 0; p < slot_count; p++)
                                    if (new_order[p] == orig)
                                    {
                                        new_pos = p;
                                        break;
                                    }
                                if (new_pos != -1 && new_pos != orig)
                                {
                                    json off;
                                    off["slot"] = slot_names.count(orig) ? slot_names.at(orig) : std::to_string(orig);
                                    off["offset"] = new_pos - orig;
                                    offsets.push_back(off);
                                }
                            }
                        }
                        else
                        {
                            for (int j = 0; j < c; j++)
                            {
                                int sidx = read_le<uint32_t>(buf, pos);
                                pos += 4;
                                int offset = read_le<int32_t>(buf, pos);
                                pos += 4;
                                if (offset != 0)
                                {
                                    json off;
                                    off["slot"] = slot_names.count(sidx) ? slot_names.at(sidx) : std::to_string(sidx);
                                    off["offset"] = offset;
                                    offsets.push_back(off);
                                }
                            }
                        }
                        if (!offsets.empty())
                        {
                            fr["offsets"] = offsets;
                            drawOrder.push_back(fr);
                        }
                    }
                    if (!drawOrder.empty())
                        anim["drawOrder"] = drawOrder;
                }
                else if (ttype == 5 || ttype == 9 || ttype == 10 || ttype >= 11)
                {
                    uint16_t idx = read_le<uint16_t>(buf, pos);
                    pos += 2;

                    uint16_t fc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> values = read_f32_array(buf, buf_size, pos, fc);
                    uint16_t cc = read_le<uint16_t>(buf, pos);
                    pos += 2;
                    std::vector<float> curves = read_f32_array(buf, buf_size, pos, cc);

                    if (ttype == 5)
                    {
                        // Color
                        std::string sname = slot_names.count(idx) ? slot_names.at(idx) : std::to_string(idx);
                        int ENTRIES = 5;
                        int frames = fc / ENTRIES;

                        if (!anim["slots"].contains(sname))
                            anim["slots"][sname] = json::object();
                        if (!anim["slots"][sname].contains("color"))
                            anim["slots"][sname]["color"] = json::array();

                        for (int i = 0; i < frames; i++)
                        {
                            int b = i * ENTRIES;
                            json fr;
                            fr["time"] = values[b];
                            fr["color"] = rgba_to_hex(values[b + 1], values[b + 2], values[b + 3], values[b + 4]);
                            maybe_add_curve(i, curves, fr);
                            anim["slots"][sname]["color"].push_back(fr);
                        }
                    }
                    else if (ttype == 9)
                    {
                        // IK
                        std::string cname = ik_names.count(idx) ? ik_names.at(idx) : "ik" + std::to_string(idx);
                        int ENTRIES = 6;
                        int frames = fc / ENTRIES;

                        if (!anim["ik"].contains(cname))
                            anim["ik"][cname] = json::array();

                        for (int i = 0; i < frames; i++)
                        {
                            int b = i * ENTRIES;
                            json fr;
                            fr["time"] = values[b];
                            fr["mix"] = values[b + 1];
                            fr["softness"] = values[b + 2];
                            fr["bendPositive"] = (values[b + 3] >= 0.0f);
                            if (values[b + 4] != 0)
                                fr["compress"] = true;
                            if (values[b + 5] != 0)
                                fr["stretch"] = true;
                            maybe_add_curve(i, curves, fr);
                            anim["ik"][cname].push_back(fr);
                        }
                    }
                    else if (ttype == 10)
                    {
                        // Transform
                        std::string cname = transform_names.count(idx) ? transform_names.at(idx) : "transform" + std::to_string(idx);
                        int ENTRIES = 5;
                        int frames = fc / ENTRIES;

                        if (!anim["transform"].contains(cname))
                            anim["transform"][cname] = json::array();

                        for (int i = 0; i < frames; i++)
                        {
                            int b = i * ENTRIES;
                            json fr;
                            fr["time"] = values[b];
                            fr["rotateMix"] = values[b + 1];
                            fr["translateMix"] = values[b + 2];
                            fr["scaleMix"] = values[b + 3];
                            fr["shearMix"] = values[b + 4];
                            maybe_add_curve(i, curves, fr);
                            anim["transform"][cname].push_back(fr);
                        }
                    }
                    else if (ttype == 11 || ttype == 12 || ttype == 13)
                    {
                        // Path
                        std::string cname = path_names.count(idx) ? path_names.at(idx) : "path" + std::to_string(idx);

                        if (!anim["path"].contains(cname))
                            anim["path"][cname] = json::object();

                        if (ttype == 13)
                        {
                            // Path Mix
                            int ENTRIES = 3;
                            int frames = fc / ENTRIES;
                            anim["path"][cname]["mix"] = json::array();
                            for (int i = 0; i < frames; i++)
                            {
                                int b = i * ENTRIES;
                                json fr;
                                fr["time"] = values[b];
                                fr["rotateMix"] = values[b + 1];
                                fr["translateMix"] = values[b + 2];
                                maybe_add_curve(i, curves, fr);
                                anim["path"][cname]["mix"].push_back(fr);
                            }
                        }
                        else
                        {
                            // Position or Spacing
                            int ENTRIES = 2;
                            int frames = fc / ENTRIES;
                            std::string key = (ttype == 11) ? "position" : "spacing";
                            anim["path"][cname][key] = json::array();
                            for (int i = 0; i < frames; i++)
                            {
                                int b = i * ENTRIES;
                                json fr;
                                fr["time"] = values[b];
                                fr[key] = values[b + 1];
                                maybe_add_curve(i, curves, fr);
                                anim["path"][cname][key].push_back(fr);
                            }
                        }
                    }
                    else if (ttype == 14)
                    {
                        // TwoColor
                        std::string sname = slot_names.count(idx) ? slot_names.at(idx) : std::to_string(idx);
                        int ENTRIES = 8;
                        int frames = fc / ENTRIES;

                        if (!anim["slots"].contains(sname))
                            anim["slots"][sname] = json::object();
                        if (!anim["slots"][sname].contains("twoColor"))
                            anim["slots"][sname]["twoColor"] = json::array();

                        for (int i = 0; i < frames; i++)
                        {
                            int b = i * ENTRIES;
                            json fr;
                            fr["time"] = values[b];
                            fr["light"] = rgba_to_hex(values[b + 1], values[b + 2], values[b + 3], values[b + 4]);
                            fr["dark"] = rgb_to_hex(values[b + 5], values[b + 6], values[b + 7]);
                            maybe_add_curve(i, curves, fr);
                            anim["slots"][sname]["twoColor"].push_back(fr);
                        }
                    }
                }
                else
                {
                    break;
                }
            }

            if (anim["events"].empty())
            {
                anim.erase("events");
            }

            anim["duration"] = duration;
            animations[name] = anim;
        }
        return animations;
    }

    std::string ParseSCSPToJson(const std::vector<uint8_t> &decompressed_data)
    {
        if (decompressed_data.empty())
            return "{}";

        const uint8_t *buf = decompressed_data.data();
        size_t buf_size = decompressed_data.size();

        // Parse header
        Header hdr = ParseHeader(buf, buf_size);

        size_t strings_base = hdr.string_offset + 8;
        size_t strings_end = strings_base + hdr.string_length;

        // Build skeleton section
        json skeleton;
        skeleton["spine"] = hdr.version.empty() ? "3.8.79" : hdr.version;
        skeleton["x"] = 0.0f;
        skeleton["y"] = 0.0f;

        if (hdr.width != 0.0f)
            skeleton["width"] = hdr.width;
        if (hdr.height != 0.0f)
            skeleton["height"] = hdr.height;
        if (!hdr.hash.empty())
            skeleton["hash"] = hdr.hash;
        if (!hdr.images_path.empty())
            skeleton["images"] = hdr.images_path;
        if (!hdr.audio_path.empty())
            skeleton["audio"] = hdr.audio_path;

        // Parse bones
        size_t pos = 0x08 + 0x62; // Start of bones section
        std::map<int16_t, std::string> bone_names;
        json bones = ParseBones(buf, buf_size, pos, strings_base, strings_end, bone_names);

        // Parse IK constraints
        std::map<int, std::string> ik_names;
        json iks = ParseIKConstraints(buf, buf_size, pos, strings_base, strings_end, bone_names, ik_names);

        // Parse slots
        std::map<int16_t, std::string> slot_names;
        json slots = ParseSlots(buf, buf_size, pos, strings_base, strings_end, bone_names, slot_names);

        // Parse transform constraints
        std::map<int, std::string> transform_names;
        json transforms = ParseTransformConstraints(buf, buf_size, pos, strings_base, strings_end, bone_names, transform_names);

        // Parse path constraints
        std::map<int, std::string> path_names;
        json paths = ParsePathConstraints(buf, buf_size, pos, strings_base, strings_end, bone_names, slot_names, path_names);

        // Parse skins (Full)
        AttachmentMetaMap attachment_meta;
        std::map<int, std::string> skin_names;
        json skins = ParseSkins(buf, buf_size, pos, strings_base, strings_end, slot_names, attachment_meta, hdr.hdr_version, skin_names);

        // Parse events
        std::map<int, std::string> event_names;
        json events = ParseEvents(buf, buf_size, pos, strings_base, strings_end, event_names);

        // Parse animations
        json animations = ParseAnimations(buf, buf_size, pos, strings_base, strings_end,
                                          bone_names, slot_names, skin_names, ik_names, transform_names, path_names, event_names,
                                          attachment_meta, hdr.hdr_version);

        // Build final JSON
        json result;
        result["skeleton"] = skeleton;
        result["bones"] = bones;
        result["ik"] = iks;
        result["slots"] = slots;
        result["transform"] = transforms;
        result["path"] = paths;
        result["skins"] = skins;
        result["events"] = events;
        result["animations"] = animations;

        return result.dump(4);
    }

    std::string ConvertSCSPToJson(const std::vector<uint8_t> &scsp_data)
    {
        auto decompressed = DecompressSCSP(scsp_data);
        return ParseSCSPToJson(decompressed);
    }

}