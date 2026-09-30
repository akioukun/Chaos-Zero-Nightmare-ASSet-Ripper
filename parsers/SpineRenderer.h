#pragma once

#include <GL/glew.h>
#include <spine/spine.h>
#include <string>
#include <map>
#include <set>
#include <vector>

class IArchive;
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
    [[nodiscard]] const std::map<std::string, TexInfo>& getTextures() const { return textures; }

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
    void addTriangles(GLuint texture, const float* vertices, int vertexCount, const unsigned short* indices, int indexCount, spine::BlendMode blendMode);
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

    bool loadSkeleton(const SpineDictionary& dict, IArchive& pack, const SpineEntry& entry);
    void unload();
    void update(float deltaTime);
    void render(int viewportWidth, int viewportHeight);

    [[nodiscard]] GLuint getFBOTexture() const { return fboTexture; }
    [[nodiscard]] bool isLoaded() const { return skeleton != nullptr; }

    // Animation/skin
    [[nodiscard]] std::vector<std::string> getAnimationNames() const;
    [[nodiscard]] std::vector<std::string> getSkinNames() const;
    void setAnimation(const std::string& name, bool loop = true);
    void setSkin(const std::string& name) const;

    // Playback
    void setPlaybackSpeed(float speed) { playbackSpeed = speed; }
    void setPlaying(bool p) { playing = p; }
    [[nodiscard]] bool isPlaying() const { return playing; }
    [[nodiscard]] float getPlaybackSpeed() const { return playbackSpeed; }

    // View
    void setFlipX(const bool flip) { flipX = flip; }
    void setFlipY(const bool flip) { flipY = flip; }

    // Blend config
    void setUsePMA(const bool pma) { usePMA = pma; }
    [[nodiscard]] bool getUsePMA() const { return usePMA; }
    void setPremultiplyTextures(bool pm) { premultiplyTextures = pm; reloadTextures = true; }
    [[nodiscard]] bool getPremultiplyTextures() const { return premultiplyTextures; }
    [[nodiscard]] bool needsTextureReload() const { return reloadTextures; }

    // Autoplay
    void nextAnimation();
    [[nodiscard]] int getCurrentAnimIndex() const { return currentAnimIndex; }
    void setAutoplayNext(bool a) { autoplayNext = a; }
    [[nodiscard]] bool getAutoplayNext() const { return autoplayNext; }
    [[nodiscard]] bool getFlipX() const { return flipX; }
    [[nodiscard]] bool getFlipY() const { return flipY; }
    void setZoom(float z) { zoom = z; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    [[nodiscard]] float getZoom() const { return zoom; }
    void zoomBy(const float factor) { zoom *= factor; if (zoom < 0.1f) zoom = 0.1f; if (zoom > 10.0f) zoom = 10.0f; }
    void pan(const float dx, const float dy) { panX += dx; panY += dy; }
    void resetView() { zoom = 1.0f; panX = 0; panY = 0; }
    [[nodiscard]] float getPanX() const { return panX; }
    [[nodiscard]] float getPanY() const { return panY; }
    [[nodiscard]] std::string getError() const { return errorMsg; }

    // Viewport background color
    void setBgColor(const float r, const float g, const float b) { bgR = r; bgG = g; bgB = b; }
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
    [[nodiscard]] std::vector<BoneInfo> getBoneList() const;
    void setBoneOverride(const std::string& boneName, const BoneOverride& ovr);
    void resetBone(const std::string& boneName);
    void resetBoneEdits();
    [[nodiscard]] bool hasBoneOverrides() const { return !boneOverrides.empty(); }
    [[nodiscard]] const std::map<std::string, BoneOverride>& getBoneOverrides() const { return boneOverrides; }
    void toggleBoneHidden(const std::string& boneName);
    [[nodiscard]] bool isBoneHidden(const std::string& boneName) const;

    // Hit testing — returns bone name or empty string
    std::string hitTestBone(float screenX, float screenY, int vpW, int vpH);
    int selectedBoneIndex = -1;

    // Transform gizmo
    enum class GizmoHandle { None, Move, ScaleTL, ScaleTR, ScaleBL, ScaleBR, Rotate };
    struct GizmoState {
        float bboxMinX{}, bboxMinY{}, bboxMaxX{}, bboxMaxY{}; // world-space bounding box
        bool valid = false;
    };
    [[nodiscard]] GizmoState getSelectedBoneGizmo() const;
    [[nodiscard]] GizmoHandle hitTestGizmo(float screenX, float screenY, int vpW, int vpH) const;

    // Texture info and swap
    struct TextureInfo { std::string name; int width; int height; GLuint glId; };
    [[nodiscard]] std::vector<TextureInfo> getTextureList() const;
    bool swapTexture(const std::string& pageName, const std::string& pngPath);
    void resetTextureSwaps();
    [[nodiscard]] bool hasTextureSwaps() const { return !textureSwaps.empty(); }

    // Export modified JSON
    [[nodiscard]] std::string getModifiedSkeletonJson() const;

private:
    void ensureFBO(int width, int height);
    void cleanupFBO();
    GLuint loadTextureFromRGBA(const unsigned char* data, int width, int height);
    void computeStableBounds();
    void screenToWorld(float sx, float sy, int vpW, int vpH, float& wx, float& wy) const;
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
