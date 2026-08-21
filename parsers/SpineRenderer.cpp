#define NOMINMAX
#include "SpineRenderer.h"
#include "SpineDictionary.h"
#include "archive/IArchive.h"
#include "SCSPParser.h"
#include "SCTParser.h"

#include "core/Logger.h"
#include "json.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <cstring>
#include <algorithm>

// spine-cpp requires this global function
spine::SpineExtension* spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

// ============================================================================
// PackTextureLoader
// ============================================================================

void PackTextureLoader::registerTexture(const std::string& name, GLuint textureId, int width, int height) {
    textures[name] = { textureId, width, height };
}

void PackTextureLoader::load(spine::AtlasPage& page, const spine::String& path) {
    std::string name(path.buffer());

    // Try exact match first
    auto it = textures.find(name);

    // Try with .sct → .png substitution
    if (it == textures.end()) {
        std::string alt = name;
        size_t pos = alt.rfind(".png");
        if (pos != std::string::npos) {
            // Try .sct version
            alt.replace(pos, 4, ".sct");
            it = textures.find(alt);
        }
        if (it == textures.end()) {
            pos = alt.rfind(".sct");
            if (pos != std::string::npos) {
                alt.replace(pos, 4, ".png");
                it = textures.find(alt);
            }
        }
    }

    if (it != textures.end()) {
        page.setRendererObject((void*)(uintptr_t)it->second.id);
        page.width = it->second.width;
        page.height = it->second.height;
    }
}

void PackTextureLoader::unload(void* texture) {
    // Textures are managed by SpineViewer::ownedTextures, not here
}

void PackTextureLoader::clearTextures() {
    textures.clear();
}

// ============================================================================
// SpineBatchRenderer
// ============================================================================

static const char* vertSrc = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* fragSrc = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
}
)";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

SpineBatchRenderer::SpineBatchRenderer() {}
SpineBatchRenderer::~SpineBatchRenderer() { dispose(); }

void SpineBatchRenderer::init() {
    if (initialized) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    projLoc = glGetUniformLocation(shaderProgram, "uProj");
    texLoc = glGetUniformLocation(shaderProgram, "uTex");

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    // pos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // uv
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    // color
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    initialized = true;
}

void SpineBatchRenderer::dispose() {
    if (!initialized) return;
    if (shaderProgram) { glDeleteProgram(shaderProgram); shaderProgram = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (ibo) { glDeleteBuffers(1, &ibo); ibo = 0; }
    initialized = false;
}

void SpineBatchRenderer::begin(float proj[16], bool pma) {
    memcpy(currentProj, proj, sizeof(float) * 16);
    currentPMA = pma;
    batchVertices.clear();
    batchIndices.clear();
    currentTexture = 0;
    currentBlend = spine::BlendMode_Normal;
}

void SpineBatchRenderer::addTriangles(GLuint texture, const float* vertices, int vertexCount,
                                       const unsigned short* indices, int indexCount,
                                       spine::BlendMode blendMode) {
    if (texture != currentTexture || blendMode != currentBlend) {
        flush();
        currentTexture = texture;
        currentBlend = blendMode;
    }

    unsigned short baseVertex = (unsigned short)batchVertices.size();
    // vertices come as: x, y, u, v, r, g, b, a (stride 8)
    for (int i = 0; i < vertexCount; i++) {
        Vertex v;
        int off = i * 8;
        v.x = vertices[off + 0];
        v.y = vertices[off + 1];
        v.u = vertices[off + 2];
        v.v = vertices[off + 3];
        v.r = vertices[off + 4];
        v.g = vertices[off + 5];
        v.b = vertices[off + 6];
        v.a = vertices[off + 7];
        batchVertices.push_back(v);
    }

    for (int i = 0; i < indexCount; i++) {
        batchIndices.push_back(baseVertex + indices[i]);
    }
}

void SpineBatchRenderer::flush() {
    if (batchIndices.empty()) return;

    glUseProgram(shaderProgram);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, currentProj);
    glUniform1i(texLoc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, batchVertices.size() * sizeof(Vertex), batchVertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, batchIndices.size() * sizeof(unsigned short), batchIndices.data(), GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    if (currentPMA) {
        switch (currentBlend) {
            case spine::BlendMode_Additive: glBlendFunc(GL_ONE, GL_ONE); break;
            case spine::BlendMode_Multiply: glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case spine::BlendMode_Screen: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            default: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
    } else {
        switch (currentBlend) {
            case spine::BlendMode_Additive: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            case spine::BlendMode_Multiply: glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case spine::BlendMode_Screen: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            default: glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
    }

    glDrawElements(GL_TRIANGLES, (GLsizei)batchIndices.size(), GL_UNSIGNED_SHORT, 0);

    glBindVertexArray(0);
    batchVertices.clear();
    batchIndices.clear();
}

void SpineBatchRenderer::end() {
    flush();
}

// ============================================================================
// SpineViewer
// ============================================================================

SpineViewer::SpineViewer() {}

SpineViewer::~SpineViewer() {
    unload();
    batchRenderer.dispose();
    cleanupFBO();
}

void SpineViewer::unload() {
    if (animState) { delete animState; animState = nullptr; }
    if (stateData) { delete stateData; stateData = nullptr; }
    if (skeleton) { delete skeleton; skeleton = nullptr; }
    if (skeletonData) { delete skeletonData; skeletonData = nullptr; }
    if (atlas) { delete atlas; atlas = nullptr; }

    for (GLuint tex : ownedTextures) {
        glDeleteTextures(1, &tex);
    }
    ownedTextures.clear();
    for (GLuint tex : swappedTextures) glDeleteTextures(1, &tex);
    swappedTextures.clear();
    textureLoader.clearTextures();
    boneOverrides.clear();
    textureSwaps.clear();
    originalJson.clear();
    errorMsg.clear();
    selectedBoneIndex = -1;
    boundsComputed = false;
}

GLuint SpineViewer::loadTextureFromRGBA(const unsigned char* data, int width, int height) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (premultiplyTextures) {
        size_t pixelCount = (size_t)width * height;
        std::vector<unsigned char> pma(pixelCount * 4);
        for (size_t i = 0; i < pixelCount; i++) {
            unsigned int a = data[i * 4 + 3];
            pma[i * 4 + 0] = (unsigned char)((data[i * 4 + 0] * a + 127) / 255);
            pma[i * 4 + 1] = (unsigned char)((data[i * 4 + 1] * a + 127) / 255);
            pma[i * 4 + 2] = (unsigned char)((data[i * 4 + 2] * a + 127) / 255);
            pma[i * 4 + 3] = (unsigned char)a;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pma.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }
    ownedTextures.push_back(tex);
    return tex;
}

bool SpineViewer::loadSkeleton(const SpineDictionary& dict, IArchive& pack, const SpineEntry& entry) {
    unload();
    batchRenderer.init();

    dict.EnsureDetailsLoaded(pack, entry);

    // 1. Read and convert SCSP to JSON
    std::string jsonStr;
    try {
        std::vector<uint8_t> scspData = pack.GetFileData(*entry.scsp_node);
        if (scspData.empty()) { errorMsg = "Failed to read SCSP file"; LogError("SpineViewer: " + errorMsg); return false; }
        jsonStr = SCSPParser::ConvertSCSPToJson(scspData);
        if (jsonStr.empty()) { errorMsg = "Failed to convert SCSP to JSON"; LogError("SpineViewer: " + errorMsg); return false; }
        originalJson = jsonStr;
    } catch (const std::exception& e) {
        errorMsg = std::string("SCSP parse error: ") + e.what();
        LogError("SpineViewer: " + errorMsg);
        return false;
    }

    // 2. Read atlas text
    std::string atlasStr;
    if (entry.atlas_node) {
        std::vector<uint8_t> atlasData = pack.GetFileData(*entry.atlas_node);
        if (!atlasData.empty()) {
            atlasStr = std::string(atlasData.begin(), atlasData.end());
        }
    }
    if (atlasStr.empty()) {
        errorMsg = "No atlas file found for this skeleton";
        LogError("SpineViewer: " + errorMsg);
        return false;
    }

    // 3. Load texture images and register with texture loader
    for (const auto* imgNode : entry.image_nodes) {
        if (!std::holds_alternative<Core::FileInfo>(imgNode->data)) continue;
        const auto& fi = std::get<Core::FileInfo>(imgNode->data);

        std::vector<uint8_t> fileData = pack.GetFileData(*imgNode);
        if (fileData.empty()) { LogError("SpineViewer: empty texture data for " + imgNode->name); continue; }

        std::string ext = fi.format;
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        int w = 0, h = 0;
        GLuint tex = 0;

        try {
            if (ext == ".sct" || ext == ".sct2") {
                SCTParser::RGBAImage rgba = SCTParser::ConvertToRGBA(fileData);
                if (rgba.data.empty()) { LogError("SpineViewer: SCT decode failed for " + imgNode->name); continue; }
                w = rgba.width; h = rgba.height;
                tex = loadTextureFromRGBA(rgba.data.data(), w, h);
            } else {
                SDL_RWops* rw = SDL_RWFromMem(fileData.data(), (int)fileData.size());
                if (!rw) continue;
                SDL_Surface* surface = IMG_Load_RW(rw, 1);
                if (!surface) { LogError("SpineViewer: IMG_Load failed for " + imgNode->name); continue; }
                SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
                SDL_FreeSurface(surface);
                if (!rgba) continue;
                w = rgba->w; h = rgba->h;
                tex = loadTextureFromRGBA((const unsigned char*)rgba->pixels, w, h);
                SDL_FreeSurface(rgba);
            }
        } catch (const std::exception& e) {
            LogError("SpineViewer: texture load exception for " + imgNode->name + ": " + e.what());
            continue;
        }

        if (tex) {
            textureLoader.registerTexture(imgNode->name, tex, w, h);
            std::string pngName = imgNode->name;
            size_t dotPos = pngName.rfind('.');
            if (dotPos != std::string::npos) {
                std::string baseName = pngName.substr(0, dotPos);
                textureLoader.registerTexture(baseName + ".png", tex, w, h);
            }
        }
    }

    // 4. Create spine Atlas from text + texture loader
    try {
        std::string atlasFixed = atlasStr;
        size_t pos = 0;
        while ((pos = atlasFixed.find(".sct", pos)) != std::string::npos) {
            if (pos + 4 < atlasFixed.size() && atlasFixed[pos + 4] == '2') {
                atlasFixed.replace(pos, 5, ".png");
                pos += 4;
            } else {
                atlasFixed.replace(pos, 4, ".png");
                pos += 4;
            }
        }

        // Guard: atlas too small or no textures loaded — spine-cpp will crash on malformed data
        if (atlasFixed.size() < 50 || ownedTextures.empty()) {
            errorMsg = "Atlas too small (" + std::to_string(atlasFixed.size()) + " bytes) or no textures loaded";
            LogError("SpineViewer: " + errorMsg);
            return false;
        }

        atlas = new spine::Atlas(atlasFixed.c_str(), (int)atlasFixed.size(), "", &textureLoader, true);
        if (atlas->getPages().size() == 0) {
            errorMsg = "Atlas has no texture pages (textures missing or failed to load)";
            LogError("SpineViewer: " + errorMsg);
            delete atlas; atlas = nullptr;
            return false;
        }
    } catch (const std::exception& e) {
        errorMsg = std::string("Failed to create spine Atlas: ") + e.what();
        LogError("SpineViewer: " + errorMsg);
        return false;
    } catch (...) {
        errorMsg = "Failed to create spine Atlas (unknown error)";
        LogError("SpineViewer: " + errorMsg);
        return false;
    }

    // 5. Load skeleton data from JSON
    try {
        spine::SkeletonJson json(atlas);
        json.setScale(1.0f);
        skeletonData = json.readSkeletonData(jsonStr.c_str());
        if (!skeletonData) {
            errorMsg = "Failed to parse skeleton JSON";
            LogError("SpineViewer: " + errorMsg);
            return false;
        }
    } catch (const std::exception& e) {
        errorMsg = std::string("Exception loading skeleton: ") + e.what();
        LogError("SpineViewer: " + errorMsg);
        return false;
    } catch (...) {
        errorMsg = "Exception loading skeleton data (unknown)";
        LogError("SpineViewer: " + errorMsg);
        return false;
    }

    // 6. Create skeleton and animation state
    try {
        skeleton = new spine::Skeleton(skeletonData);
        skeleton->setToSetupPose();
        skeleton->updateWorldTransform();

        stateData = new spine::AnimationStateData(skeletonData);
        stateData->setDefaultMix(0.2f);
        animState = new spine::AnimationState(stateData);

        auto& anims = skeletonData->getAnimations();
        if (anims.size() > 0) {
            animState->setAnimation(0, anims[0]->getName(), true);
        }
    } catch (const std::exception& e) {
        errorMsg = std::string("Exception creating skeleton instance: ") + e.what();
        LogError("SpineViewer: " + errorMsg);
        return false;
    }

    computeStableBounds();
    return true;
}

void SpineViewer::computeStableBounds() {
    if (!skeleton) return;

    // Try actual rendered bounds from setup pose first (most accurate)
    spine::Vector<float> vbuf;
    float bx, by, bw, bh;
    skeleton->getBounds(bx, by, bw, bh, vbuf);
    if (bw > 10 && bh > 10) {
        cachedBoundsX = bx;
        cachedBoundsY = by;
        cachedBoundsW = bw;
        cachedBoundsH = bh;
        boundsComputed = true;
        zoom = 1.0f;
        panX = panY = 0;
        return;
    }

    // Fallback to skeleton declared dimensions
    cachedBoundsW = skeletonData->getWidth();
    cachedBoundsH = skeletonData->getHeight();
    if (cachedBoundsW > 10 && cachedBoundsH > 10) {
        cachedBoundsX = -cachedBoundsW / 2;
        cachedBoundsY = -cachedBoundsH / 2;
        boundsComputed = true;
        zoom = 1.0f;
        panX = panY = 0;
        return;
    }

    // Last resort
    cachedBoundsW = 800;
    cachedBoundsH = 800;
    cachedBoundsX = -400;
    cachedBoundsY = -400;
    boundsComputed = true;
    zoom = 1.0f;
    panX = panY = 0;
}

void SpineViewer::screenToWorld(float sx, float sy, int vpW, int vpH, float& wx, float& wy) {
    float padX = cachedBoundsW * 0.1f;
    float padY = cachedBoundsH * 0.1f;
    float left = cachedBoundsX - padX - panX;
    float right = cachedBoundsX + cachedBoundsW + padX - panX;
    float bottom = cachedBoundsY - padY - panY;
    float top = cachedBoundsY + cachedBoundsH + padY - panY;

    float cx = (left + right) / 2.0f;
    float cy = (bottom + top) / 2.0f;
    float hw = (right - left) / (2.0f * zoom);
    float hh = (top - bottom) / (2.0f * zoom);

    // Maintain aspect ratio
    float viewAspect = (float)vpW / vpH;
    float boundsAspect = hw / hh;
    if (boundsAspect > viewAspect) {
        hh = hw / viewAspect;
    } else {
        hw = hh * viewAspect;
    }

    wx = cx - hw + (sx / vpW) * 2.0f * hw;
    wy = cy + hh - (sy / vpH) * 2.0f * hh; // Y flipped
}

std::string SpineViewer::hitTestBone(float screenX, float screenY, int vpW, int vpH) {
    if (!skeleton) return "";

    float wx, wy;
    screenToWorld(screenX, screenY, vpW, vpH, wx, wy);

    auto& bones = skeleton->getBones();
    float bestDist = 30.0f * (cachedBoundsW / (vpW * zoom)); // ~30px threshold in world units
    std::string bestName;
    int bestIdx = -1;

    for (size_t i = 0; i < bones.size(); i++) {
        float bx = bones[i]->getWorldX();
        float by = bones[i]->getWorldY();
        float dx = wx - bx;
        float dy = wy - by;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestName = std::string(bones[i]->getData().getName().buffer());
            bestIdx = (int)i;
        }
    }

    selectedBoneIndex = bestIdx;
    return bestName;
}

void SpineViewer::update(float deltaTime) {
    if (!skeleton || !animState) return;
    try {
        skeleton->setScaleX(flipX ? -1.0f : 1.0f);
        skeleton->setScaleY(flipY ? -1.0f : 1.0f);

        // Always advance and apply animation (even if deltaTime is 0 when paused)
        // so that the pose is valid for override application
        if (deltaTime > 0) {
            animState->update(deltaTime);
        }
        animState->apply(*skeleton);

        // Autoplay: advance to next animation when current one completes
        if (autoplayNext && deltaTime > 0) {
            spine::TrackEntry* track = animState->getCurrent(0);
            if (track && !track->getLoop() && track->isComplete()) {
                nextAnimation();
            }
        }

        // Apply bone overrides on top of animation
        applyBoneOverrides();

        skeleton->updateWorldTransform();
    } catch (const std::exception& e) {
        LogError("SpineViewer::update exception: " + std::string(e.what()));
    } catch (...) {
        LogError("SpineViewer::update unknown exception");
    }
}

void SpineViewer::applyBoneOverrides() {
    if (!skeleton) return;
    for (auto& [name, ovr] : boneOverrides) {
        spine::Bone* bone = skeleton->findBone(spine::String(name.c_str()));
        if (bone) {
            bone->setX(ovr.x);
            bone->setY(ovr.y);
            bone->setRotation(ovr.rotation);
            bone->setScaleX(ovr.scaleX);
            bone->setScaleY(ovr.scaleY);
            bone->setShearX(ovr.shearX);
            bone->setShearY(ovr.shearY);
        }
    }
}

void SpineViewer::render(int viewportWidth, int viewportHeight) {
    if (!skeleton || viewportWidth <= 0 || viewportHeight <= 0) return;

    ensureFBO(viewportWidth, viewportHeight);

    // Save GL state
    GLint prevFBO; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4]; glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend; glGetBooleanv(GL_BLEND, &prevBlend);
    GLboolean prevDepth; glGetBooleanv(GL_DEPTH_TEST, &prevDepth);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, viewportWidth, viewportHeight);

    glClearColor(bgR, bgG, bgB, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);

    // Use cached stable bounds (computed once on load from multiple animation frames)
    float bx = cachedBoundsX;
    float by = cachedBoundsY;
    float bw = cachedBoundsW;
    float bh = cachedBoundsH;

    if (bw <= 0 || bh <= 0) {
        bw = 800; bh = 800; bx = -400; by = -400;
    }

    // Add padding
    float padX = bw * 0.1f;
    float padY = bh * 0.1f;
    float left = bx - padX;
    float right = bx + bw + padX;
    float bottom = by - padY;
    float top = by + bh + padY;

    // Apply zoom (zoom > 1 = zoom in = smaller view area) and pan
    {
        float cx = (left + right) / 2.0f - panX;
        float cy = (bottom + top) / 2.0f - panY;
        float hw = (right - left) / (2.0f * zoom);
        float hh = (top - bottom) / (2.0f * zoom);
        left = cx - hw; right = cx + hw;
        bottom = cy - hh; top = cy + hh;
    }

    // Maintain aspect ratio
    float viewAspect = (float)viewportWidth / viewportHeight;
    float boundsAspect = (right - left) / (top - bottom);
    if (boundsAspect > viewAspect) {
        float extra = (right - left) / viewAspect - (top - bottom);
        bottom -= extra / 2;
        top += extra / 2;
    } else {
        float extra = (top - bottom) * viewAspect - (right - left);
        left -= extra / 2;
        right += extra / 2;
    }

    // Orthographic projection — flip Y so FBO output is Y-down (Nuklear expects top-left origin)
    float proj[16] = {};
    proj[0] = 2.0f / (right - left);
    proj[5] = -2.0f / (top - bottom);  // negated for Y-flip
    proj[10] = -1.0f;
    proj[12] = -(right + left) / (right - left);
    proj[13] = (top + bottom) / (top - bottom);  // sign flipped
    proj[15] = 1.0f;

    batchRenderer.begin(proj, usePMA);

    // Render slots in draw order
    try {
    auto& drawOrder = skeleton->getDrawOrder();
    for (size_t i = 0; i < drawOrder.size(); i++) {
        spine::Slot* slot = drawOrder[i];
        spine::Attachment* attachment = slot->getAttachment();
        if (!attachment) continue;

        // Skip slots attached to hidden bones (check ancestors too)
        {
            bool boneHidden = false;
            spine::Bone* b = &slot->getBone();
            while (b) {
                if (hiddenBones.count(std::string(b->getData().getName().buffer()))) {
                    boneHidden = true;
                    break;
                }
                b = b->getParent();
            }
            if (boneHidden) continue;
        }

        spine::Color skeletonColor = skeleton->getColor();
        spine::Color slotColor = slot->getColor();
        float tintR = skeletonColor.r * slotColor.r;
        float tintG = skeletonColor.g * slotColor.g;
        float tintB = skeletonColor.b * slotColor.b;
        float tintA = skeletonColor.a * slotColor.a;
        if (tintA <= 0) continue;

        GLuint texture = 0;
        spine::BlendMode blendMode = slot->getData().getBlendMode();

        // Interleaved vertex data: x, y, u, v, r, g, b, a
        static float worldVertices[8192];

        if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            auto* region = static_cast<spine::RegionAttachment*>(attachment);

            auto* atlasRegion = (spine::AtlasRegion*)region->getRendererObject();
            if (!atlasRegion || !atlasRegion->page || !atlasRegion->page->getRendererObject())
                continue; // skip — no texture loaded for this attachment

            texture = (GLuint)(uintptr_t)atlasRegion->page->getRendererObject();
            region->computeWorldVertices(slot->getBone(), worldVertices, 0, 2);

            static float regionVerts[8 * 4];
            static unsigned short regionIndices[] = { 0, 1, 2, 2, 3, 0 };
            spine::Vector<float>& uvs = region->getUVs();

            spine::Color attachColor = region->getColor();
            float a = tintA * attachColor.a;
            float pm = usePMA ? a : 1.0f;
            float r = tintR * attachColor.r * pm;
            float g = tintG * attachColor.g * pm;
            float b = tintB * attachColor.b * pm;

            for (int j = 0; j < 4; j++) {
                regionVerts[j * 8 + 0] = worldVertices[j * 2 + 0];
                regionVerts[j * 8 + 1] = worldVertices[j * 2 + 1];
                regionVerts[j * 8 + 2] = uvs[j * 2 + 0];
                regionVerts[j * 8 + 3] = uvs[j * 2 + 1];
                regionVerts[j * 8 + 4] = r;
                regionVerts[j * 8 + 5] = g;
                regionVerts[j * 8 + 6] = b;
                regionVerts[j * 8 + 7] = a;
            }

            batchRenderer.addTriangles(texture, regionVerts, 4, regionIndices, 6, blendMode);

        } else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(attachment);

            auto* atlasRegion = (spine::AtlasRegion*)mesh->getRendererObject();
            if (!atlasRegion || !atlasRegion->page || !atlasRegion->page->getRendererObject())
                continue; // skip — no texture loaded

            texture = (GLuint)(uintptr_t)atlasRegion->page->getRendererObject();

            auto& meshIndices = mesh->getTriangles();
            auto& uvs = mesh->getUVs();
            int meshVertCount = (int)mesh->getWorldVerticesLength() / 2;

            mesh->computeWorldVertices(*slot, 0, mesh->getWorldVerticesLength(), worldVertices, 0, 2);

            spine::Color attachColor = mesh->getColor();
            float a = tintA * attachColor.a;
            float pm = usePMA ? a : 1.0f;
            float r = tintR * attachColor.r * pm;
            float g = tintG * attachColor.g * pm;
            float b = tintB * attachColor.b * pm;

            // Build interleaved vertex data
            static std::vector<float> meshVerts;
            meshVerts.resize(meshVertCount * 8);
            for (int j = 0; j < meshVertCount; j++) {
                meshVerts[j * 8 + 0] = worldVertices[j * 2 + 0];
                meshVerts[j * 8 + 1] = worldVertices[j * 2 + 1];
                meshVerts[j * 8 + 2] = uvs[j * 2 + 0];
                meshVerts[j * 8 + 3] = uvs[j * 2 + 1];
                meshVerts[j * 8 + 4] = r;
                meshVerts[j * 8 + 5] = g;
                meshVerts[j * 8 + 6] = b;
                meshVerts[j * 8 + 7] = a;
            }

            batchRenderer.addTriangles(texture, meshVerts.data(), meshVertCount,
                                        meshIndices.buffer(), (int)meshIndices.size(), blendMode);

        } else if (attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
            auto* clip = static_cast<spine::ClippingAttachment*>(attachment);
            clipper.clipStart(*slot, clip);
            continue;
        }

        clipper.clipEnd(*slot);
    }
    clipper.clipEnd();
    } catch (const std::exception& e) {
        LogError("SpineViewer::render exception: " + std::string(e.what()));
    } catch (...) {
        LogError("SpineViewer::render unknown exception");
    }

    // Draw transform gizmo on selected bone
    if (selectedBoneIndex >= 0) {
        GizmoState gs = getSelectedBoneGizmo();
        if (gs.valid) {
            // 1x1 white texture for drawing lines/rects
            static GLuint gizmoTex = 0;
            if (!gizmoTex) {
                unsigned char white[] = { 255, 255, 255, 255 };
                glGenTextures(1, &gizmoTex);
                glBindTexture(GL_TEXTURE_2D, gizmoTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
            }
            unsigned short qi[] = { 0,1,2, 2,3,0 };

            float lineW = (gs.bboxMaxX - gs.bboxMinX) * 0.005f;
            if (lineW < 0.5f) lineW = 0.5f;

            // Box outline color (cyan)
            float lr = 0.0f, lg = 0.85f, lb = 1.0f, la = 0.8f;
            if (usePMA) { lr *= la; lg *= la; lb *= la; }

            auto drawLine = [&](float x1, float y1, float x2, float y2) {
                float dx = x2 - x1, dy = y2 - y1;
                float len = sqrtf(dx*dx + dy*dy);
                if (len < 0.001f) return;
                float nx = -dy / len * lineW, ny = dx / len * lineW;
                float v[] = {
                    x1-nx, y1-ny, 0,0, lr,lg,lb,la,
                    x1+nx, y1+ny, 1,0, lr,lg,lb,la,
                    x2+nx, y2+ny, 1,1, lr,lg,lb,la,
                    x2-nx, y2-ny, 0,1, lr,lg,lb,la,
                };
                batchRenderer.addTriangles(gizmoTex, v, 4, qi, 6, spine::BlendMode_Normal);
            };

            float x0 = gs.bboxMinX, y0 = gs.bboxMinY, x1 = gs.bboxMaxX, y1 = gs.bboxMaxY;

            // Bounding box edges
            drawLine(x0, y0, x1, y0); // bottom
            drawLine(x1, y0, x1, y1); // right
            drawLine(x1, y1, x0, y1); // top
            drawLine(x0, y1, x0, y0); // left

            // Corner handles (small squares)
            float hs = (x1 - x0) * 0.025f;
            if (hs < 2) hs = 2;
            float hr = 1.0f, hg = 1.0f, hb = 1.0f, ha = 0.9f;
            if (usePMA) { hr *= ha; hg *= ha; hb *= ha; }

            auto drawHandle = [&](float cx, float cy) {
                float v[] = {
                    cx-hs, cy-hs, 0,0, hr,hg,hb,ha,
                    cx+hs, cy-hs, 1,0, hr,hg,hb,ha,
                    cx+hs, cy+hs, 1,1, hr,hg,hb,ha,
                    cx-hs, cy+hs, 0,1, hr,hg,hb,ha,
                };
                batchRenderer.addTriangles(gizmoTex, v, 4, qi, 6, spine::BlendMode_Normal);
            };

            drawHandle(x0, y0); // BL
            drawHandle(x1, y0); // BR
            drawHandle(x0, y1); // TL
            drawHandle(x1, y1); // TR

            // Rotation handle (circle outside top-right)
            float rotX = x1 + hs * 4, rotY = y1 + hs * 4;
            // Line from corner to rotation handle
            drawLine(x1, y1, rotX, rotY);
            // Rotation circle
            float rr = 0.9f, rg = 0.5f, rb = 1.0f, ra = 0.9f;
            if (usePMA) { rr *= ra; rg *= ra; rb *= ra; }
            float circR = hs * 1.5f;
            int segs = 10;
            for (int s = 0; s < segs; s++) {
                float a1 = (float)s / segs * 6.2832f;
                float a2 = (float)(s + 1) / segs * 6.2832f;
                float v[] = {
                    rotX, rotY, 0.5f, 0.5f, rr,rg,rb,ra,
                    rotX + cosf(a1)*circR, rotY + sinf(a1)*circR, 0,0, rr,rg,rb,ra,
                    rotX + cosf(a2)*circR, rotY + sinf(a2)*circR, 1,0, rr,rg,rb,ra,
                    rotX, rotY, 0.5f, 0.5f, rr,rg,rb,ra, // degenerate 4th vert
                };
                unsigned short ti[] = { 0,1,2 };
                batchRenderer.addTriangles(gizmoTex, v, 3, ti, 3, spine::BlendMode_Normal);
            }

            // Center crosshair at bone position
            auto& bonez = skeleton->getBones();
            if (selectedBoneIndex < (int)bonez.size()) {
                float bx = bonez[selectedBoneIndex]->getWorldX();
                float by = bonez[selectedBoneIndex]->getWorldY();
                float cs = hs * 2;
                drawLine(bx - cs, by, bx + cs, by);
                drawLine(bx, by - cs, bx, by + cs);
            }
        }
    }

    batchRenderer.end();

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
}

void SpineViewer::ensureFBO(int width, int height) {
    if (fbo && fboWidth == width && fboHeight == height) return;
    cleanupFBO();

    fboWidth = width;
    fboHeight = height;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &fboTexture);
    glBindTexture(GL_TEXTURE_2D, fboTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTexture, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpineViewer::cleanupFBO() {
    if (fboTexture) { glDeleteTextures(1, &fboTexture); fboTexture = 0; }
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    fboWidth = fboHeight = 0;
}

std::vector<std::string> SpineViewer::getAnimationNames() const {
    std::vector<std::string> names;
    if (!skeletonData) return names;
    auto& anims = skeletonData->getAnimations();
    for (size_t i = 0; i < anims.size(); i++) {
        names.push_back(std::string(anims[i]->getName().buffer()));
    }
    return names;
}

std::vector<std::string> SpineViewer::getSkinNames() const {
    std::vector<std::string> names;
    if (!skeletonData) return names;
    auto& skins = skeletonData->getSkins();
    for (size_t i = 0; i < skins.size(); i++) {
        names.push_back(std::string(skins[i]->getName().buffer()));
    }
    return names;
}

void SpineViewer::setAnimation(const std::string& name, bool loop) {
    if (!animState || !skeletonData) return;
    animState->setAnimation(0, spine::String(name.c_str()), loop);
    // Track current index for autoplay
    auto& anims = skeletonData->getAnimations();
    for (size_t i = 0; i < anims.size(); i++) {
        if (std::string(anims[i]->getName().buffer()) == name) {
            currentAnimIndex = (int)i;
            break;
        }
    }
}

void SpineViewer::nextAnimation() {
    if (!animState || !skeletonData) return;
    auto& anims = skeletonData->getAnimations();
    if (anims.size() == 0) return;
    currentAnimIndex = (currentAnimIndex + 1) % (int)anims.size();
    animState->setAnimation(0, anims[currentAnimIndex]->getName(), !autoplayNext);
}

void SpineViewer::setSkin(const std::string& name) {
    if (!skeleton) return;
    skeleton->setSkin(spine::String(name.c_str()));
    skeleton->setSlotsToSetupPose();
}

// ============================================================================
// Bone editing
// ============================================================================

std::vector<SpineViewer::BoneInfo> SpineViewer::getBoneList() const {
    std::vector<BoneInfo> list;
    if (!skeleton) return list;
    auto& bones = skeleton->getBones();
    for (size_t i = 0; i < bones.size(); i++) {
        BoneInfo bi;
        bi.name = std::string(bones[i]->getData().getName().buffer());

        // Parent and depth
        spine::BoneData& data = bones[i]->getData();
        if (data.getParent()) {
            bi.parentName = std::string(data.getParent()->getName().buffer());
            int d = 0;
            spine::BoneData* p = data.getParent();
            while (p) { d++; p = p->getParent(); }
            bi.depth = d;
        }

        // Setup pose values (stable baseline from the skeleton data)
        bi.setupX = data.getX(); bi.setupY = data.getY();
        bi.setupRot = data.getRotation();
        bi.setupSX = data.getScaleX(); bi.setupSY = data.getScaleY();
        bi.setupShX = data.getShearX(); bi.setupShY = data.getShearY();

        // Current animated values (live, changes each frame)
        bi.animX = bones[i]->getX(); bi.animY = bones[i]->getY();
        bi.animRot = bones[i]->getRotation();
        bi.animSX = bones[i]->getScaleX(); bi.animSY = bones[i]->getScaleY();
        bi.animShX = bones[i]->getShearX(); bi.animShY = bones[i]->getShearY();

        // Editable values: override if user edited, otherwise setup pose
        auto it = boneOverrides.find(bi.name);
        bi.hasOverride = (it != boneOverrides.end());
        if (bi.hasOverride) {
            bi.x = it->second.x; bi.y = it->second.y;
            bi.rotation = it->second.rotation;
            bi.scaleX = it->second.scaleX; bi.scaleY = it->second.scaleY;
            bi.shearX = it->second.shearX; bi.shearY = it->second.shearY;
        } else {
            bi.x = bi.setupX; bi.y = bi.setupY;
            bi.rotation = bi.setupRot;
            bi.scaleX = bi.setupSX; bi.scaleY = bi.setupSY;
            bi.shearX = bi.setupShX; bi.shearY = bi.setupShY;
        }
        bi.hidden = hiddenBones.count(bi.name) > 0;
        list.push_back(bi);
    }
    return list;
}

void SpineViewer::setBoneOverride(const std::string& boneName, const BoneOverride& ovr) {
    boneOverrides[boneName] = ovr;
}

void SpineViewer::resetBone(const std::string& boneName) {
    if (!skeleton) { boneOverrides.erase(boneName); return; }
    auto& bones = skeleton->getBones();
    for (size_t i = 0; i < bones.size(); i++) {
        if (std::string(bones[i]->getData().getName().buffer()) == boneName) {
            auto& data = bones[i]->getData();
            BoneOverride ovr;
            ovr.x = data.getX(); ovr.y = data.getY();
            ovr.rotation = data.getRotation();
            ovr.scaleX = data.getScaleX(); ovr.scaleY = data.getScaleY();
            ovr.shearX = data.getShearX(); ovr.shearY = data.getShearY();
            boneOverrides[boneName] = ovr;
            return;
        }
    }
    boneOverrides.erase(boneName);
}

void SpineViewer::resetBoneEdits() {
    boneOverrides.clear();
    hiddenBones.clear();
}

SpineViewer::GizmoState SpineViewer::getSelectedBoneGizmo() const {
    GizmoState gs;
    gs.valid = false;
    if (!skeleton || selectedBoneIndex < 0) return gs;

    auto& bones = skeleton->getBones();
    if (selectedBoneIndex >= (int)bones.size()) return gs;
    spine::Bone* selBone = bones[selectedBoneIndex];

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    bool found = false;
    float worldVerts[8192];

    // Find all slots attached to this bone and compute their world vertex bounds
    auto& drawOrder = skeleton->getDrawOrder();
    for (size_t i = 0; i < drawOrder.size(); i++) {
        spine::Slot* slot = drawOrder[i];
        if (&slot->getBone() != selBone) continue;
        spine::Attachment* att = slot->getAttachment();
        if (!att) continue;

        int vertCount = 0;
        if (att->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            auto* reg = static_cast<spine::RegionAttachment*>(att);
            reg->computeWorldVertices(slot->getBone(), worldVerts, 0, 2);
            vertCount = 4;
        } else if (att->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(att);
            vertCount = (int)mesh->getWorldVerticesLength() / 2;
            mesh->computeWorldVertices(*slot, 0, mesh->getWorldVerticesLength(), worldVerts, 0, 2);
        }

        for (int v = 0; v < vertCount; v++) {
            float vx = worldVerts[v * 2], vy = worldVerts[v * 2 + 1];
            if (vx < minX) minX = vx;
            if (vy < minY) minY = vy;
            if (vx > maxX) maxX = vx;
            if (vy > maxY) maxY = vy;
            found = true;
        }
    }

    // Fallback: use bone position with a small default size
    if (!found) {
        float bx = selBone->getWorldX(), by = selBone->getWorldY();
        float sz = cachedBoundsW * 0.05f;
        minX = bx - sz; minY = by - sz; maxX = bx + sz; maxY = by + sz;
    }

    gs.bboxMinX = minX; gs.bboxMinY = minY;
    gs.bboxMaxX = maxX; gs.bboxMaxY = maxY;
    gs.valid = true;
    return gs;
}

SpineViewer::GizmoHandle SpineViewer::hitTestGizmo(float screenX, float screenY, int vpW, int vpH) const {
    GizmoState gs = getSelectedBoneGizmo();
    if (!gs.valid) return GizmoHandle::None;

    float wx, wy;
    const_cast<SpineViewer*>(this)->screenToWorld(screenX, screenY, vpW, vpH, wx, wy);

    float handleSz = (gs.bboxMaxX - gs.bboxMinX) * 0.08f;
    if (handleSz < 3) handleSz = 3;

    // Corner handles (scale)
    auto inHandle = [&](float hx, float hy) {
        return fabsf(wx - hx) < handleSz && fabsf(wy - hy) < handleSz;
    };
    if (inHandle(gs.bboxMinX, gs.bboxMaxY)) return GizmoHandle::ScaleTL;
    if (inHandle(gs.bboxMaxX, gs.bboxMaxY)) return GizmoHandle::ScaleTR;
    if (inHandle(gs.bboxMinX, gs.bboxMinY)) return GizmoHandle::ScaleBL;
    if (inHandle(gs.bboxMaxX, gs.bboxMinY)) return GizmoHandle::ScaleBR;

    // Rotate handle (outside top-right corner)
    float rotX = gs.bboxMaxX + handleSz * 3, rotY = gs.bboxMaxY + handleSz * 3;
    if (fabsf(wx - rotX) < handleSz * 1.5f && fabsf(wy - rotY) < handleSz * 1.5f)
        return GizmoHandle::Rotate;

    // Inside bbox = move
    if (wx >= gs.bboxMinX && wx <= gs.bboxMaxX && wy >= gs.bboxMinY && wy <= gs.bboxMaxY)
        return GizmoHandle::Move;

    return GizmoHandle::None;
}

void SpineViewer::toggleBoneHidden(const std::string& boneName) {
    if (hiddenBones.count(boneName))
        hiddenBones.erase(boneName);
    else
        hiddenBones.insert(boneName);
}

bool SpineViewer::isBoneHidden(const std::string& boneName) const {
    return hiddenBones.count(boneName) > 0;
}

// ============================================================================
// Texture management
// ============================================================================

std::vector<SpineViewer::TextureInfo> SpineViewer::getTextureList() const {
    std::vector<TextureInfo> list;
    if (!atlas) return list;
    auto& pages = atlas->getPages();
    for (size_t i = 0; i < pages.size(); i++) {
        TextureInfo ti;
        ti.name = std::string(pages[i]->name.buffer());
        ti.width = pages[i]->width;
        ti.height = pages[i]->height;
        ti.glId = (GLuint)(uintptr_t)pages[i]->getRendererObject();
        list.push_back(ti);
    }
    return list;
}

bool SpineViewer::swapTexture(const std::string& pageName, const std::string& pngPath) {
    if (!atlas) return false;

    // Load replacement PNG
    SDL_RWops* rw = SDL_RWFromFile(pngPath.c_str(), "rb");
    if (!rw) { LogError("swapTexture: can't open " + pngPath); return false; }
    SDL_Surface* surface = IMG_Load_RW(rw, 1);
    if (!surface) { LogError("swapTexture: IMG_Load failed for " + pngPath); return false; }
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return false;

    GLuint newTex = loadTextureFromRGBA((const unsigned char*)rgba->pixels, rgba->w, rgba->h);
    int w = rgba->w, h = rgba->h;
    SDL_FreeSurface(rgba);

    if (!newTex) return false;
    swappedTextures.push_back(newTex);

    // Find the atlas page and update its renderer object
    auto& pages = atlas->getPages();
    for (size_t i = 0; i < pages.size(); i++) {
        if (std::string(pages[i]->name.buffer()) == pageName) {
            pages[i]->setRendererObject((void*)(uintptr_t)newTex);
            pages[i]->width = w;
            pages[i]->height = h;
            textureSwaps[pageName] = pngPath;
            LogInfo("swapTexture: replaced '" + pageName + "' with " + pngPath);
            return true;
        }
    }

    LogError("swapTexture: page '" + pageName + "' not found");
    return false;
}

void SpineViewer::resetTextureSwaps() {
    // Reloading the skeleton is the cleanest way to reset textures
    textureSwaps.clear();
}

// ============================================================================
// Export modified JSON
// ============================================================================

std::string SpineViewer::getModifiedSkeletonJson() const {
    if (originalJson.empty()) return "";
    if (boneOverrides.empty()) return originalJson;

    try {
        // Parse, modify bone scales, re-serialize
        // Using nlohmann json (already included via json.hpp)
        auto j = nlohmann::ordered_json::parse(originalJson);
        if (j.contains("bones") && j["bones"].is_array()) {
            for (auto& bone : j["bones"]) {
                if (!bone.contains("name")) continue;
                std::string name = bone["name"].get<std::string>();
                auto it = boneOverrides.find(name);
                if (it != boneOverrides.end()) {
                    bone["x"] = it->second.x;
                    bone["y"] = it->second.y;
                    bone["rotation"] = it->second.rotation;
                    bone["scaleX"] = it->second.scaleX;
                    bone["scaleY"] = it->second.scaleY;
                    bone["shearX"] = it->second.shearX;
                    bone["shearY"] = it->second.shearY;
                }
            }
        }
        return j.dump(2);
    } catch (const std::exception& e) {
        LogError("getModifiedSkeletonJson: " + std::string(e.what()));
        return originalJson;
    }
}
