#pragma once
#include <vector>
#include <string>
#include <cstdint>


//////////////// TYPES
struct ColorLab { float l = 0.0f, a = 0.0f, b = 0.0f, alpha = 100.0f; };

// sums + sums of squares per channel: squared distance over a whole run in one step
struct ColorLabAccum {

    // double: float lost too much in sum2 - 2*v*sum + n*v*v (tested against the 256-term loop)
    double l = 0.0, l2 = 0.0;
    double a = 0.0, a2 = 0.0;
    double b = 0.0, b2 = 0.0;
    double alpha = 0.0, alpha2 = 0.0;
};

struct BlockData {
    std::string id;

    int order = 0;    // list order = the Order column of allowed_ids.csv

    int versionIdx = 0;    // release that added the block, index into the version list

    ColorLab meanLab;
    std::vector<ColorLab> flatLab;
    std::vector<ColorLab> sortedLab;
    std::vector<unsigned char> rgbArray;
    unsigned char r = 0, g = 0, b = 0;

    bool isEntity = false;
    bool emitsLight = false;
    bool obeysGravity = false;
    bool transparent = false;
};


//////////////// ENGINE
void loadBlockDatabase();
std::vector<ColorLab> convertRGBtoLAB(const unsigned char* rgbData, int width, int height);
void generateDatapack(const std::string& muralName);
void generateLitematica(const std::string& muralName);

// native builds only, the page uses mural_load_image()
void setDroppedImage(const char* path);


//////////////// BRIDGE

// a pointer getter reuses one buffer: read it before calling the same getter again
extern "C" {

    //////// lifecycle
    void mural_init();    // loads on a worker thread, mural_poll() says when its done
    int  mural_ready();
    int  mural_poll();    // bits: 1 db ready, 2 log, 4 preview, 8 processing
    int  mural_is_processing();
    int  mural_heartbeat();
    float mural_progress();

    //////// blocks
    const char* mural_get_blocks_json();
    const char* mural_get_block_groups_json();    // names for the groups field of the blocks json
    int            mural_block_count();
    unsigned char* mural_block_atlas();    // 16x16 rgba per block, csv order
    int            mural_block_atlas_cols();

    const char* mural_get_overlays_json();
    int            mural_overlay_count();
    unsigned char* mural_overlay_atlas();    // rebuilt when the biome changes
    int            mural_overlay_atlas_cols();

    //////// game versions

    // oldest first; blocks give their release as an index into it
    const char* mural_get_versions_json();
    const char* mural_get_overlay_versions_json();
    void mural_set_game_version(int versionIdx);    // also picks the id a renamed block exports under

    //////// palette
    void mural_set_allowed_blocks(const char* csvIds);    // comma separated ids
    void mural_set_allowed_overlays(const char* csvIds);

    //////// picture
    void mural_load_image(unsigned char* rgba, int w, int h, const char* name);    // rgba w*h*4, from _malloc
    int  mural_has_image();

    //////// settings, one call per ui group
    void mural_set_size(int w, int h, int lockAspect);
    void mural_set_name(const char* name);
    void mural_set_placement(int orientation, int dirVert, int dirHorz, int biome);
    void mural_set_bg_mode(int mode);
    void mural_set_tweaks(float brightness, float contrast, float saturation,
        float hue, float denoise, float sharpen);
    void mural_set_weights(float color, float spatial, float texture);
    void mural_set_dither(int method, float intensity);
    void mural_set_pass2(int enable, int chunkIdx, float threshold, float mse, float ssim, int passes, int candidates);

    // group 7: search amounts 0..1 + feathering; search and feather switch the two halves
    void mural_set_enhance(int enable, float grain, float flat, float protect, int feather, int cleanup, int search);
    void mural_set_use_overlays(int on);
    const char* mural_get_config_json();

    //////// preview
    void mural_request_preview();

    // once per preview event, then read the buffers
    void mural_snapshot();
    int      mural_preview_w();
    int      mural_preview_h();
    int32_t* mural_preview_grid();    // compacted palette index per cell, -1 = empty
    unsigned char* mural_preview_mean();    // one mean pixel per block (rgba)
    unsigned char* mural_preview_atlas();    // 16x16 rgba per palette entry in use
    int      mural_preview_atlas_cols();
    int      mural_preview_atlas_count();
    const char* mural_preview_ids_json();
    const char* mural_pick_and_disallow(int col, int row);
    const char* mural_bg_color(int mode);    // "r,g,b" at the current biome

    //////// console
    int  mural_log_count();
    const char* mural_log_line(int index);
    const char* mural_get_status_json();

    //////// exports

    // both run on the worker pool and end in a browser download
    void mural_generate_datapack();
    void mural_generate_litematic();
}
