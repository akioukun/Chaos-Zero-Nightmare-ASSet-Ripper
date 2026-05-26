#pragma once

#include <GL/glew.h>
#include <spine/spine.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory>

class DataPack;
struct SpineEntry;
class SpineDictionary;

// Custom texture loader that uses pre-registered GL textures
class PackTextureLoader : public spine::TextureLoader {
public:
    void registerTexture(const std::string& name, GLuint textureId, int width, int height);
    void load(spine::AtlasPage& page, const spine::String& path) override;
    void unload(void* texture) override;
    void clearTextures();

    struct TexInfo { GLuint id; int width; int height; };
    const std::map<std::string, TexInfo>& getTextures() const { return textures; }

private:
    std::map<std::string, TexInfo> textures;
};

// Minimal GL3.3 2D batch renderer for spine
class SpineBatchRenderer {
public:
    SpineBatchRenderer();
    ~SpineBatchRenderer();

    void init();
    void dispose();
    void begin(float projMatrix[16], bool pma = true);
    void addTriangles(GLuint texture, const float* vertices, int vertexCount,
                      const unsigned short* indices, int indexCount,
                      spine::BlendMode blendMode);
    void end();

private:
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };

    void flush();

    GLuint shaderProgram = 0;
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLint projLoc = -1, texLoc = -1;

    std::vector<Vertex> batchVertices;
    std::vector<unsigned short> batchIndices;
    GLuint currentTexture = 0;
    spine::BlendMode currentBlend = spine::BlendMode_Normal;
    float currentProj[16] = {};
    bool currentPMA = true;
    bool initialized = false;
};

// Bone edit override — stores all editable bone properties
struct BoneOverride {
    float x = 0, y = 0;
    float rotation = 0;
    float scaleX = 1.0f, scaleY = 1.0f;
    float shearX = 0, shearY = 0;
};

// Main Spine viewer class
class SpineViewer {
public:
    SpineViewer();
    ~SpineViewer();

    bool loadSkeleton(const SpineDictionary& dict, DataPack& pack, const SpineEntry& entry);
    void unload();
    void update(float deltaTime);
    void render(int viewportWidth, int viewportHeight);

    GLuint getFBOTexture() const { return fboTexture; }
    bool isLoaded() const { return skeleton != nullptr; }

    // Animation/skin
    std::vector<std::string> getAnimationNames() const;
    std::vector<std::string> getSkinNames() const;
    void setAnimation(const std::string& name, bool loop = true);
    void setSkin(const std::string& name);

    // Playback
    void setPlaybackSpeed(float speed) { playbackSpeed = speed; }
    void setPlaying(bool p) { playing = p; }
    bool isPlaying() const { return playing; }
    float getPlaybackSpeed() const { return playbackSpeed; }

    // View
    void setFlipX(bool flip) { flipX = flip; }
    void setFlipY(bool flip) { flipY = flip; }

    // Blend config
    void setUsePMA(bool pma) { usePMA = pma; }
    bool getUsePMA() const { return usePMA; }
    void setPremultiplyTextures(bool pm) { premultiplyTextures = pm; reloadTextures = true; }
    bool getPremultiplyTextures() const { return premultiplyTextures; }
    bool needsTextureReload() const { return reloadTextures; }

    // Autoplay
    void nextAnimation();
    int getCurrentAnimIndex() const { return currentAnimIndex; }
    void setAutoplayNext(bool a) { autoplayNext = a; }
    bool getAutoplayNext() const { return autoplayNext; }
    bool getFlipX() const { return flipX; }
    bool getFlipY() const { return flipY; }
    void setZoom(float z) { zoom = z; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    float getZoom() const { return zoom; }
    void zoomBy(float factor) { zoom *= factor; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    void pan(float dx, float dy) { panX += dx; panY += dy; }
    void resetView() { zoom = 1.0f; panX = 0; panY = 0; }
    float getPanX() const { return panX; }
    float getPanY() const { return panY; }
    std::string getError() const { return errorMsg; }

    // Viewport background color
    void setBgColor(float r, float g, float b) { bgR = r; bgG = g; bgB = b; }
    void getBgColor(float& r, float& g, float& b) const { r = bgR; g = bgG; b = bgB; }

    // Bone editing
    struct BoneInfo {
        std::string name;
        std::string parentName;
        int depth = 0; // 0 = root, 1 = child, etc.
        // Override values (what the user edits — stable, not affected by animation)
        float x, y, rotation, scaleX, scaleY, shearX, shearY;
        // Original setup pose (for reset)
        float setupX, setupY, setupRot, setupSX, setupSY, setupShX, setupShY;
        // Current animated values (read-only, changes each frame)
        float animX, animY, animRot, animSX, animSY, animShX, animShY;
        bool hasOverride;
        bool hidden;
    };
    std::vector<BoneInfo> getBoneList() const;
    void setBoneOverride(const std::string& boneName, const BoneOverride& ovr);
    void resetBone(const std::string& boneName);
    void resetBoneEdits();
    bool hasBoneOverrides() const { return !boneOverrides.empty(); }
    const std::map<std::string, BoneOverride>& getBoneOverrides() const { return boneOverrides; }
    void toggleBoneHidden(const std::string& boneName);
    bool isBoneHidden(const std::string& boneName) const;

    // Hit testing — returns bone name or empty string
    std::string hitTestBone(float screenX, float screenY, int vpW, int vpH);
    int selectedBoneIndex = -1;

    // Transform gizmo
    enum class GizmoHandle { None, Move, ScaleTL, ScaleTR, ScaleBL, ScaleBR, Rotate };
    struct GizmoState {
        float bboxMinX, bboxMinY, bboxMaxX, bboxMaxY; // world-space bounding box
        bool valid = false;
    };
    GizmoState getSelectedBoneGizmo() const;
    GizmoHandle hitTestGizmo(float screenX, float screenY, int vpW, int vpH) const;

    // Texture info and swap
    struct TextureInfo { std::string name; int width; int height; GLuint glId; };
    std::vector<TextureInfo> getTextureList() const;
    bool swapTexture(const std::string& pageName, const std::string& pngPath);
    void resetTextureSwaps();
    bool hasTextureSwaps() const { return !textureSwaps.empty(); }

    // Export modified JSON
    std::string getModifiedSkeletonJson() const;

private:
    void ensureFBO(int width, int height);
    void cleanupFBO();
    GLuint loadTextureFromRGBA(const unsigned char* data, int width, int height);
    void computeStableBounds();
    void screenToWorld(float sx, float sy, int vpW, int vpH, float& wx, float& wy);
    void applyBoneOverrides();

    // Spine objects
    PackTextureLoader textureLoader;
    spine::Atlas* atlas = nullptr;
    spine::SkeletonData* skeletonData = nullptr;
    spine::Skeleton* skeleton = nullptr;
    spine::AnimationStateData* stateData = nullptr;
    spine::AnimationState* animState = nullptr;
    spine::SkeletonClipping clipper;

    SpineBatchRenderer batchRenderer;

    // FBO
    GLuint fbo = 0, fboTexture = 0, fboDepth = 0;
    int fboWidth = 0, fboHeight = 0;

    float playbackSpeed = 1.0f;
    bool playing = true;
    bool flipX = false;
    bool flipY = false;
    float zoom = 1.0f;
    float panX = 0, panY = 0;
    bool usePMA = true;
    bool premultiplyTextures = false; // ASTC source textures are already PMA — don't re-premultiply
    bool reloadTextures = false;
    bool autoplayNext = false;
    int currentAnimIndex = 0;
    std::string errorMsg;
    float bgR = 0, bgG = 0, bgB = 0; // viewport background (default transparent black)

    // Cached bounds
    float cachedBoundsX = 0, cachedBoundsY = 0, cachedBoundsW = 0, cachedBoundsH = 0;
    bool boundsComputed = false;

    // Bone editing
    std::map<std::string, BoneOverride> boneOverrides;
    std::set<std::string> hiddenBones;

    // Texture swaps (page name → replacement file path)
    std::map<std::string, std::string> textureSwaps;
    std::vector<GLuint> swappedTextures; // GL textures to clean up

    // Original JSON for export
    std::string originalJson;

    std::vector<GLuint> ownedTextures;
};
