#include "engine_internal.h"


//////////////// HELPERS
const char* biomeName(int idx) {
    if (idx < 0 || idx > 7) idx = 0;
    return kBiomeNames[idx];
}

void jsonEscape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", (unsigned)(unsigned char)c);
                out += b;
            }
            else out += c;
        }
    }
}

void jsonStr(const std::string& s, std::string& out) {
    out += '"';
    jsonEscape(s, out);
    out += '"';
}

std::set<std::string> splitIdSet(const char* csv) {
    std::set<std::string> out;
    if (!csv) return out;
    std::string cur;
    for (const char* p = csv; ; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
            if (*p == '\0') break;
        }
        else cur += *p;
    }
    return out;
}

void blitTile(std::vector<unsigned char>& atlas, int cols, int slot,
    const unsigned char* src16) {
    int cx = (slot % cols) * 16;
    int cy = (slot / cols) * 16;
    int stride = cols * 16 * 4;
    for (int y = 0; y < 16; ++y) {
        unsigned char* dst = atlas.data() + (size_t)(cy + y) * stride + (size_t)cx * 4;
        memcpy(dst, src16 + (size_t)y * 16 * 4, 16 * 4);
    }
}

void ensureAtlasSize(std::vector<unsigned char>& atlas, int cols, int count) {
    int rows = (count + cols - 1) / cols;
    if (rows < 1) rows = 1;
    atlas.assign((size_t)cols * 16 * (size_t)rows * 16 * 4, 0);
}


//////////////// ICON ATLASES
void buildBlockIconAtlas() {
    int n = (int)masterDatabase.size();
    ensureAtlasSize(g_blockAtlas, g_blockAtlasCols, n);
    for (int i = 0; i < n; ++i) {
        auto it = extBlockInfo.find(masterDatabase[i].id);
        if (it == extBlockInfo.end() || it->second.uiRgbaArray.size() < 16 * 16 * 4) continue;
        blitTile(g_blockAtlas, g_blockAtlasCols, i, it->second.uiRgbaArray.data());
    }
}

// overlay icons follow the biome tint, so this runs again when it changes
void buildOverlayIconAtlas(int biomeIdxLocal) {
    int n = (int)g_allOverlayIds.size();
    ensureAtlasSize(g_overlayAtlas, g_overlayAtlasCols, n);

    std::vector<unsigned char> tile(16 * 16 * 4, 0);
    for (int i = 0; i < n; ++i) {
        const std::string& id = g_allOverlayIds[i];

        std::string texName = id + ".png";
        if (allFiles.count(texName) == 0) texName = id + "_top.png";
        if (allFiles.count(texName) == 0 && id.find("redstone_wire") == 0) texName = "redstone_dust_dot.png";
        if (allFiles.count(texName) == 0) continue;

        const TextureRaw& tr = allFiles[texName];

        std::vector<float> tint = { 1.0f, 1.0f, 1.0f };
        if (id == "vine" || id == "leaf_litter") tint = getBiomeTintVals(id, biomeName(biomeIdxLocal));
        else if (id.find("redstone_wire") == 0) tint = { 75.0f / 255.0f, 0.0f, 0.0f };

        std::fill(tile.begin(), tile.end(), 0);
        for (int y = 0; y < 16 && y < tr.h; ++y) {
            for (int x = 0; x < 16 && x < tr.w; ++x) {
                int s = (y * tr.w + x) * 4;
                int d = (y * 16 + x) * 4;
                tile[d + 0] = (unsigned char)std::min(255.0f, tr.data[s + 0] * tint[0]);
                tile[d + 1] = (unsigned char)std::min(255.0f, tr.data[s + 1] * tint[1]);
                tile[d + 2] = (unsigned char)std::min(255.0f, tr.data[s + 2] * tint[2]);
                tile[d + 3] = tr.data[s + 3];
            }
        }
        blitTile(g_overlayAtlas, g_overlayAtlasCols, i, tile.data());
    }
}

// every overlay id once, with its release -- the split stays all disallowed, as loaded
void captureOverlayIds() {
    g_allOverlayIds.clear();
    for (const auto& id : allowedOverlayIds)    g_allOverlayIds.push_back(id);
    for (const auto& id : disallowedOverlayIds) g_allOverlayIds.push_back(id);
    std::sort(g_allOverlayIds.begin(), g_allOverlayIds.end());
    g_allOverlayIds.erase(std::unique(g_allOverlayIds.begin(), g_allOverlayIds.end()), g_allOverlayIds.end());

    g_overlayVersionIdx.assign(g_allOverlayIds.size(), 0);
    for (size_t i = 0; i < g_allOverlayIds.size(); ++i) {
        auto it = g_overlayVersionRaw.find(g_allOverlayIds[i]);
        if (it != g_overlayVersionRaw.end()) g_overlayVersionIdx[i] = versionIndexOf(it->second);
    }
}


//////////////// PREVIEW SNAPSHOT

// a mean pixel per block for zoomed out, full tiles of the entries in use for zoomed in
// only entries in use go into the atlas: the palette has thousands of variants
void snapshotPreview() {
    std::lock_guard<std::mutex> lock(dataMutex);

    g_snapW = previewWidth;
    g_snapH = previewHeight;

    if (previewGridIndices.empty() || activePalette->empty() || g_snapW <= 0 || g_snapH <= 0) {
        g_snapW = g_snapH = 0;
        g_gridSnap.clear();
        g_meanSnap.clear();
        g_paletteAtlas.clear();
        g_paletteAtlasCount = 0;
        g_jsonPreviewIds = "[]";
        g_jsonPreviewOverlays = "[]";
        return;
    }

    bool transparentMode = (previewBgMode == 3);

    std::unordered_map<int, int> remap;
    std::vector<int> used;
    remap.reserve(1024);

    size_t cells = previewGridIndices.size();
    g_gridSnap.assign(cells, -1);

    for (size_t i = 0; i < cells; ++i) {
        int idx = previewGridIndices[i];
        if (idx < 0 || idx >= (int)activePalette->size()) continue;
        auto it = remap.find(idx);
        if (it == remap.end()) {
            int compact = (int)used.size();
            remap.emplace(idx, compact);
            used.push_back(idx);
            g_gridSnap[i] = compact;
        }
        else {
            g_gridSnap[i] = it->second;
        }
    }

    g_paletteAtlasCount = (int)used.size();
    ensureAtlasSize(g_paletteAtlas, g_paletteAtlasCols, g_paletteAtlasCount);

    g_jsonPreviewIds = "[";
    g_jsonPreviewOverlays = "[";
    std::vector<unsigned char> tile(16 * 16 * 4, 0);

    for (int slot = 0; slot < g_paletteAtlasCount; ++slot) {
        const ActiveBlock& b = (*activePalette)[used[slot]];
        bool isAir = (b.mc_id == "air");

        if (b.rgbArray.size() >= 16 * 16 * 4) {
            memcpy(tile.data(), b.rgbArray.data(), 16 * 16 * 4);
        }
        else {
            for (int p = 0; p < 16 * 16; ++p) {
                tile[p * 4 + 0] = b.meanR; tile[p * 4 + 1] = b.meanG;
                tile[p * 4 + 2] = b.meanB; tile[p * 4 + 3] = b.meanA;
            }
        }
        if (transparentMode && isAir) {
            for (int p = 0; p < 16 * 16; ++p) tile[p * 4 + 3] = 0;
        }
        blitTile(g_paletteAtlas, g_paletteAtlasCols, slot, tile.data());

        if (slot) { g_jsonPreviewIds += ','; g_jsonPreviewOverlays += ','; }
        std::string base = b.mc_id.substr(0, b.mc_id.find('['));
        jsonStr(base, g_jsonPreviewIds);
        jsonStr(b.ovId.empty() ? std::string() : overlayBlockString(b), g_jsonPreviewOverlays);
    }
    g_jsonPreviewIds += ']';
    g_jsonPreviewOverlays += ']';

    g_meanSnap.assign((size_t)g_snapW * g_snapH * 4, 0);
    for (size_t i = 0; i < cells && i < (size_t)g_snapW * g_snapH; ++i) {
        int compact = g_gridSnap[i];
        if (compact < 0) continue;
        const ActiveBlock& b = (*activePalette)[used[compact]];
        unsigned char a = b.meanA;
        if (transparentMode && b.mc_id == "air") a = 0;
        g_meanSnap[i * 4 + 0] = b.meanR;
        g_meanSnap[i * 4 + 1] = b.meanG;
        g_meanSnap[i * 4 + 2] = b.meanB;
        g_meanSnap[i * 4 + 3] = a;
    }
}


//////////////// API
extern "C" {

    //////// lifecycle

    // worker thread, so the page can show progress while ~1000 textures decode
    void EMSCRIPTEN_KEEPALIVE mural_init() {
        static bool started = false;
        if (started) return;
        started = true;

        std::thread([]() {
            loadBlockDatabase();
            captureOverlayIds();
            buildBlockIconAtlas();
            buildOverlayIconAtlas(0);
            g_dbReady = true;
            }).detach();
    }

    int EMSCRIPTEN_KEEPALIVE mural_ready() { return g_dbReady.load() ? 1 : 0; }

    int EMSCRIPTEN_KEEPALIVE mural_poll() {
        int flags = 0;

        if (g_dbReady.load() && !g_dbAnnounced) { g_dbAnnounced = true; flags |= MD_DB_READY; }

        bool proc = engineBusy();
        if (proc != g_lastProcessing) { g_lastProcessing = proc; flags |= MD_PROCESSING; }

        int serial = previewSerial.load();
        if (serial != g_lastPreviewSerial) { g_lastPreviewSerial = serial; flags |= MD_PREVIEW; }

        size_t logN;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            logN = consoleLog.size();
        }
        if (logN != g_lastLogCount) { g_lastLogCount = logN; flags |= MD_LOG; }

        return flags;
    }

    int EMSCRIPTEN_KEEPALIVE mural_is_processing() { return engineBusy() ? 1 : 0; }

    //////// blocks

    // csv row order = icon atlas slots, the page sorts on ord for the list
    // ver = index into the versions json, bit n of groups = group n of the groups json
    const char* EMSCRIPTEN_KEEPALIVE mural_get_blocks_json() {
        g_jsonBlocks = "[";
        for (size_t i = 0; i < masterDatabase.size(); ++i) {
            const BlockData& bd = masterDatabase[i];

            // find, not []: workers may read extBlockInfo while this runs on the main thread
            static const ExtendedBlockInfo kEmpty;
            auto extIt = extBlockInfo.find(bd.id);
            const ExtendedBlockInfo& ext = (extIt == extBlockInfo.end()) ? kEmpty : extIt->second;
            if (i) g_jsonBlocks += ',';
            g_jsonBlocks += "{\"id\":";
            jsonStr(bd.id, g_jsonBlocks);
            char flags[256];
            snprintf(flags, sizeof(flags),
                ",\"entity\":%d,\"light\":%d,\"gravity\":%d,\"transparent\":%d,\"pain\":%d,\"deteriorates\":%d,\"moose\":%d,\"ver\":%d,\"ord\":%d,\"groups\":%u}",
                bd.isEntity ? 1 : 0, bd.emitsLight ? 1 : 0, bd.obeysGravity ? 1 : 0,
                bd.transparent ? 1 : 0, ext.isPainToGet ? 1 : 0, ext.deteriorates ? 1 : 0,
                ext.moosesChoice ? 1 : 0, bd.versionIdx, bd.order, ext.groups);
            g_jsonBlocks += flags;
        }
        g_jsonBlocks += ']';
        return g_jsonBlocks.c_str();
    }

    const char* EMSCRIPTEN_KEEPALIVE mural_get_block_groups_json() {
        static std::string out;
        out = "[";
        for (size_t i = 0; i < g_blockGroupNames.size(); ++i) {
            if (i) out += ',';
            jsonStr(g_blockGroupNames[i], out);
        }
        out += ']';
        return out.c_str();
    }

    int EMSCRIPTEN_KEEPALIVE mural_block_count() { return (int)masterDatabase.size(); }
    unsigned char* EMSCRIPTEN_KEEPALIVE mural_block_atlas() { return g_blockAtlas.data(); }
    int EMSCRIPTEN_KEEPALIVE mural_block_atlas_cols() { return g_blockAtlasCols; }

    const char* EMSCRIPTEN_KEEPALIVE mural_get_overlays_json() {
        g_jsonOverlays = "[";
        for (size_t i = 0; i < g_allOverlayIds.size(); ++i) {
            if (i) g_jsonOverlays += ',';
            jsonStr(g_allOverlayIds[i], g_jsonOverlays);
        }
        g_jsonOverlays += ']';
        return g_jsonOverlays.c_str();
    }

    int EMSCRIPTEN_KEEPALIVE mural_overlay_count() { return (int)g_allOverlayIds.size(); }
    unsigned char* EMSCRIPTEN_KEEPALIVE mural_overlay_atlas() { return g_overlayAtlas.data(); }
    int EMSCRIPTEN_KEEPALIVE mural_overlay_atlas_cols() { return g_overlayAtlasCols; }

    //////// game versions

    // every other version crossing this bridge is an index into this list
    const char* EMSCRIPTEN_KEEPALIVE mural_get_versions_json() {
        g_jsonVersions = "[";
        for (size_t i = 0; i < g_gameVersions.size(); ++i) {
            if (i) g_jsonVersions += ',';
            jsonStr(g_gameVersions[i], g_jsonVersions);
        }
        g_jsonVersions += ']';
        return g_jsonVersions.c_str();
    }

    // in step with the overlays json, which stays a flat id list
    const char* EMSCRIPTEN_KEEPALIVE mural_get_overlay_versions_json() {
        g_jsonVersions = "[";
        for (size_t i = 0; i < g_overlayVersionIdx.size(); ++i) {
            if (i) g_jsonVersions += ',';
            g_jsonVersions += std::to_string(g_overlayVersionIdx[i]);
        }
        g_jsonVersions += ']';
        return g_jsonVersions.c_str();
    }

    // only picks the id a block exports under, the page decides what a version offers
    void EMSCRIPTEN_KEEPALIVE mural_set_game_version(int idx) {
        std::lock_guard<std::mutex> l(dataMutex);
        if (g_gameVersions.empty()) return;
        selectedVersionIdx = std::clamp(idx, 0, (int)g_gameVersions.size() - 1);
    }

    //////// palette

    // both lists rebuilt in csv order, whatever order the page sends
    void EMSCRIPTEN_KEEPALIVE mural_set_allowed_blocks(const char* csv) {
        std::set<std::string> allow = splitIdSet(csv);
        std::lock_guard<std::mutex> lock(dataMutex);
        allowedBlocks.clear();
        disallowedBlocks.clear();
        for (auto& bd : masterDatabase) {
            if (allow.count(bd.id)) allowedBlocks.push_back(&bd);
            else disallowedBlocks.push_back(&bd);
        }
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_allowed_overlays(const char* csv) {
        std::set<std::string> allow = splitIdSet(csv);
        std::lock_guard<std::mutex> lock(dataMutex);
        allowedOverlayIds.clear();
        disallowedOverlayIds.clear();
        for (const auto& id : g_allOverlayIds) {
            if (allow.count(id)) allowedOverlayIds.push_back(id);
            else disallowedOverlayIds.push_back(id);
        }
    }

    //////// picture

    // rgba from _malloc, the page frees it after
    void EMSCRIPTEN_KEEPALIVE mural_load_image(unsigned char* rgba, int w, int h, const char* name) {
        std::lock_guard<std::mutex> lock(dataMutex);
        snprintf(imagePath, sizeof(imagePath), "%s", name ? name : "image");
        cachedImagePath = imagePath;
        origW = w; origH = h;

        std::vector<unsigned char>().swap(origData);
        origData.assign(rgba, rgba + (size_t)w * h * 4);
        srcW = w; srcH = h;
        g_sourceWanted = false;

        cachedTW = 0;
        aspectR = 0.0f;    // calculatePreview works the default size out again
        tweaksChanged = true;
    }

    // the same picture again for another mural size, size + settings stay
    // no picture (the page couldnt read it): the kept copy becomes the picture
    void EMSCRIPTEN_KEEPALIVE mural_resend_image(unsigned char* rgba, int w, int h) {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (rgba && w == origW && h == origH) {
            std::vector<unsigned char>().swap(origData);
            origData.assign(rgba, rgba + (size_t)w * h * 4);
            srcW = w; srcH = h;
        }
        else if (srcW > 0 && srcH > 0) {
            origW = srcW; origH = srcH;
            aspectR = (float)origW / (float)origH;
        }
        cachedTW = 0;
        tweaksChanged = true;
        g_sourceWanted = false;
    }

    int EMSCRIPTEN_KEEPALIVE mural_source_wanted() { return g_sourceWanted.load() ? 1 : 0; }

    int EMSCRIPTEN_KEEPALIVE mural_has_image() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return origData.empty() ? 0 : 1;
    }

    //////// settings

    // none of these start a calculation: the page waits a moment, then calls mural_request_preview()
    void EMSCRIPTEN_KEEPALIVE mural_set_size(int w, int h, int lock) {
        std::lock_guard<std::mutex> l(dataMutex);
        targetWidth = std::clamp(w, 1, 1000);    // tested: bigger doesnt fit in browser memory (blocks per side)
        targetHeight = std::clamp(h, 1, 1000);
        lockAspect = (lock != 0);
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_name(const char* s) {
        std::lock_guard<std::mutex> l(dataMutex);
        snprintf(muralName, sizeof(muralName), "%s", s ? s : "my_mural");

        // a long name in another alphabet can be cut in the middle of a letter
        size_t n = strlen(muralName);
        size_t tail = 0;
        while (tail < n && tail < 3 && ((unsigned char)muralName[n - 1 - tail] & 0xC0) == 0x80) ++tail;
        if (tail < n) {
            const unsigned char lead = (unsigned char)muralName[n - 1 - tail];
            const size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
            if (lead >= 0x80 && need != tail) muralName[n - 1 - tail] = 0;
        }
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_placement(int ori, int dirVert, int dirHorz, int biome) {
        bool biomeChanged;
        {
            std::lock_guard<std::mutex> l(dataMutex);

            // a config file can hold any number, these index fixed tables
            ori = std::clamp(ori, 0, 1);
            dirVert = std::clamp(dirVert, 0, 3);
            dirHorz = std::clamp(dirHorz, 0, 1);
            biome = std::clamp(biome, 0, 7);
            biomeChanged = (biome != biomeIdx);
            orientationIdx = ori;
            directionVertIdx = dirVert;
            directionHorzIdx = dirHorz;
            directionIdx = (ori == 0) ? dirVert : dirHorz;
            biomeIdx = biome;
        }
        if (biomeChanged) buildOverlayIconAtlas(std::clamp(biome, 0, 7));
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_bg_mode(int mode) {
        std::lock_guard<std::mutex> l(dataMutex);
        previewBgMode = std::clamp(mode, 0, 3);
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_tweaks(float bright, float contrast, float sat,
        float hue, float denoise, float sharpen) {
        std::lock_guard<std::mutex> l(dataMutex);
        auto sane = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        tweakBrightness = sane(bright, 0.0f); tweakContrast = sane(contrast, 1.0f); tweakSaturation = sane(sat, 1.0f);
        tweakHue = sane(hue, 0.0f); tweakDenoise = std::max(0.0f, sane(denoise, 0.0f)); tweakSharpen = std::max(0.0f, sane(sharpen, 0.0f));
        tweaksChanged = true;
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_weights(float color, float spatial, float texture) {
        std::lock_guard<std::mutex> l(dataMutex);
        auto sane = [](float v, float fallback) { return std::isfinite(v) ? std::max(0.0f, v) : fallback; };
        weightColor = sane(color, 5.0f); weightSpatial = sane(spatial, 5.0f); weightTexture = sane(texture, 5.0f);
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_dither(int method, float intensity) {
        std::lock_guard<std::mutex> l(dataMutex);
        ditherMethod = std::clamp(method, 0, 5);
        ditherIntensity = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 0.3f;
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_pass2(int enable, int chunkIdx, float threshold,
        float mse, float ssim, int passes, int candidates) {
        std::lock_guard<std::mutex> l(dataMutex);
        enablePass2 = (enable != 0);
        pass2ChunkSizeIdx = std::clamp(chunkIdx, 0, kPass2ChunkChoices - 1);
        pass2Passes = std::clamp(passes, 1, 10);
        pass2Candidates = std::clamp(candidates, 3, kMaxPass2Candidates);
        pass2Threshold = threshold;
        weightMSE = mse; weightSSIM = ssim;
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_enhance(int enable, float grain, float flat, float protect,
        int feather, int cleanup, int search) {
        std::lock_guard<std::mutex> l(dataMutex);
        enableEnhance = (enable != 0);
        enhanceSettings.search = (search != 0);
        enhanceSettings.grain = grain; enhanceSettings.flat = flat; enhanceSettings.protect = protect;
        enhanceSettings.feather = (feather != 0); enhanceSettings.cleanup = (cleanup != 0);
    }

    // lab units an overlay has to win by: 0 = free, below 0 = preferred
    void EMSCRIPTEN_KEEPALIVE mural_set_overlay_bias(float bias) {
        g_overlayBias = bias;
    }

    //////// test switches

    // for measuring + checking from a test page, the app never calls them
    void EMSCRIPTEN_KEEPALIVE mural_set_cull_margin(float margin) {
        g_cullMargin = margin;
    }

    void EMSCRIPTEN_KEEPALIVE mural_set_debug_cell(int row, int col) {
        g_debugRow = row; g_debugCol = col;
    }

    // [ms, units] per phase of the last run: a good calibration has the same ms per unit everywhere
    const char* EMSCRIPTEN_KEEPALIVE mural_progress_diag_json() {
        static std::string out;
        std::lock_guard<std::mutex> l(dataMutex);
        static const char* const names[6] = { "before", "pass1", "rebuild", "rank", "refine", "experimental" };
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"laterScale\":%.4f,\"machine\":%.4f,\"threads\":%d", g_laterScale, g_machineFactor, getOptimalThreadCount());
        out = buf;
        for (int i = 0; i < 6; ++i) {
            snprintf(buf, sizeof(buf), ",\"%s\":[%.2f,%lld]", names[i], g_phaseMs[i], g_phaseUnits[i]);
            out += buf;
        }
        out += '}';
        return out.c_str();
    }

    // bumped on every published preview -- isProcessing() cant tell a new result from an old or skipped run
    int EMSCRIPTEN_KEEPALIVE mural_preview_serial() {
        return previewSerial.load();
    }

    // on, one calculation, then disagree / cells = share of cells the cull changed
    void EMSCRIPTEN_KEEPALIVE mural_set_cull_audit(int on) {
        g_cullAudit = on;
        g_cullAuditCells = 0;
        g_cullAuditDisagree = 0;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_legacy_overlay_prep(int on) {
        g_legacyOverlayPrep = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_legacy_texture_loop(int on) {
        g_legacyTextureLoop = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_legacy_upscale(int on) {
        g_legacyUpscale = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_legacy_full_composite(int on) {
        g_legacyFullComposite = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_dither_whole_cell(int on) {
        g_ditherWholeCell = on ? 1 : 0;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_max_fast_regions(int n) {
        g_maxFastRegions = n;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_early_exit(int on) {
        g_earlyExit = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_mean_floor(int on) {
        g_meanFloor = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_coarse_bound(int on) {
        g_coarseBound = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_best_first(int on) {
        g_bestFirst = on;
    }
    void EMSCRIPTEN_KEEPALIVE mural_set_packed_sweep(int on) {
        g_packedSweep = on;
    }
    double EMSCRIPTEN_KEEPALIVE mural_cull_audit_cells() {
        return (double)g_cullAuditCells.load();
    }
    double EMSCRIPTEN_KEEPALIVE mural_cull_audit_disagree() {
        return (double)g_cullAuditDisagree.load();
    }

    //////// overlays and config
    void EMSCRIPTEN_KEEPALIVE mural_set_use_overlays(int on) {
        std::lock_guard<std::mutex> l(dataMutex);
        useOverlays = (on != 0);
    }

    // with the aspect locked a new picture resizes the mural, so the page reads this back after each preview
    const char* EMSCRIPTEN_KEEPALIVE mural_get_config_json() {
        std::lock_guard<std::mutex> l(dataMutex);
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"width\":%d,\"height\":%d,\"lockAspect\":%d,\"aspect\":%.6f,\"origW\":%d,\"origH\":%d,\"name\":\"",
            targetWidth, targetHeight, lockAspect ? 1 : 0, aspectR, origW, origH);
        g_jsonConfig = buf;
        jsonEscape(muralName, g_jsonConfig);
        g_jsonConfig += "\",\"path\":\"";
        jsonEscape(imagePath, g_jsonConfig);
        g_jsonConfig += "\"}";
        return g_jsonConfig.c_str();
    }

    //////// preview
    void EMSCRIPTEN_KEEPALIVE mural_request_preview() {
        if (!g_dbReady.load()) return;
        currentCalcId++;
        initSupervisorThread();
        {
            std::lock_guard<std::mutex> lock(supervisorMutex);
            previewBusy = true;
            pendingPreview = true;
        }
        supervisorCv.notify_one();
    }

    // once per preview event, before reading any buffer
    void EMSCRIPTEN_KEEPALIVE mural_snapshot() { snapshotPreview(); }

    int EMSCRIPTEN_KEEPALIVE mural_preview_w() { return g_snapW; }
    int EMSCRIPTEN_KEEPALIVE mural_preview_h() { return g_snapH; }
    int32_t* EMSCRIPTEN_KEEPALIVE mural_preview_grid() { return g_gridSnap.data(); }
    unsigned char* EMSCRIPTEN_KEEPALIVE mural_preview_mean() { return g_meanSnap.data(); }
    unsigned char* EMSCRIPTEN_KEEPALIVE mural_preview_atlas() { return g_paletteAtlas.data(); }
    int EMSCRIPTEN_KEEPALIVE mural_preview_atlas_cols() { return g_paletteAtlasCols; }
    int EMSCRIPTEN_KEEPALIVE mural_preview_atlas_count() { return g_paletteAtlasCount; }
    const char* EMSCRIPTEN_KEEPALIVE mural_preview_ids_json() { return g_jsonPreviewIds.c_str(); }
    const char* EMSCRIPTEN_KEEPALIVE mural_preview_overlays_json() { return g_jsonPreviewOverlays.c_str(); }

    // the moves that build the rails + dust on screen, ops = [type, kind, arg, x, z] as railbuild::Op
    // input = what the builder was asked for, three numbers per cell (kind, shape, flags)
    const char* EMSCRIPTEN_KEEPALIVE mural_rail_plan_json() {
        static std::string out;
        std::lock_guard<std::mutex> lock(dataMutex);
        const RailPlan& p = previewRailPlan;
        out = "{\"w\":" + std::to_string(p.w) + ",\"h\":" + std::to_string(p.h)
            + ",\"rails\":" + std::to_string(p.rails) + ",\"dust\":" + std::to_string(p.dust)
            + ",\"reshaped\":" + std::to_string(p.reshaped) + ",\"dropped\":" + std::to_string(p.dropped)
            + ",\"helpers\":" + std::to_string(p.helpers) + ",\"levers\":" + std::to_string(p.levers) + ",\"leverVisits\":" + std::to_string(p.leverVisits)
            + ",\"mismatched\":" + std::to_string(p.mismatched) + ",\"ops\":[";
        for (size_t i = 0; i < p.ops.size(); ++i) {
            const railbuild::Op& op = p.ops[i];
            if (i) out += ",";
            out += "[" + std::to_string(op.type) + "," + std::to_string(op.kind) + "," + std::to_string(op.arg)
                + "," + std::to_string(op.x) + "," + std::to_string(op.z) + "]";
        }
        out += "],\"input\":[";
        for (size_t i = 0; i < p.wishKind.size(); ++i) {
            if (i) out += ",";
            out += std::to_string(p.wishKind[i]) + "," + std::to_string(p.wishShape[i]) + "," + std::to_string(p.flags[i]);
        }
        out += "]}";
        return out.c_str();
    }

    // every entry of the last palette as {id, ov, base, hex}: overlay tile checks, small palettes only
    const char* EMSCRIPTEN_KEEPALIVE mural_palette_dump_json() {
        static std::string out;
        static const char* hexd = "0123456789abcdef";
        std::lock_guard<std::mutex> lock(dataMutex);
        out = "[";
        for (size_t i = 0; i < activePalette->size(); ++i) {
            const ActiveBlock& b = (*activePalette)[i];
            if (i) out += ',';
            out += "{\"id\":";
            jsonStr(b.mc_id, out);
            out += ",\"ov\":";
            jsonStr(b.ovId.empty() ? std::string() : overlayBlockString(b), out);
            out += ",\"base\":" + std::to_string(b.ovBaseIdx) + ",\"hex\":\"";
            if (b.rgbArray.size() >= 16 * 16 * 4) {
                for (int k = 0; k < 16 * 16 * 4; ++k) { out += hexd[b.rgbArray[k] >> 4]; out += hexd[b.rgbArray[k] & 15]; }
            }
            out += "\"}";
        }
        out += "]";
        return out.c_str();
    }

    // ctrl+click on the canvas: the block under it goes to disallowed, "" = empty cell or already out
    const char* EMSCRIPTEN_KEEPALIVE mural_pick_and_disallow(int col, int row) {
        static std::string result;
        result.clear();
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            if (previewWidth <= 0 || col < 0 || row < 0 || col >= previewWidth || row >= previewHeight)
                return result.c_str();

            size_t idx = (size_t)row * previewWidth + col;
            if (idx >= previewGridIndices.size()) return result.c_str();

            int palIdx = previewGridIndices[idx];
            if (palIdx < 0 || palIdx >= (int)activePalette->size()) return result.c_str();

            std::string mc_id = (*activePalette)[palIdx].mc_id;
            std::string base = mc_id.substr(0, mc_id.find('['));

            auto it = std::find_if(allowedBlocks.begin(), allowedBlocks.end(),
                [&](BlockData* b) { return b->id == base; });
            if (it == allowedBlocks.end()) return result.c_str();

            disallowedBlocks.push_back(*it);
            allowedBlocks.erase(it);
            sortBlockList(disallowedBlocks);
            result = base;
        }

        // after the lock: Log() takes dataMutex itself
        Log(result + " is now disallowed.");
        return result.c_str();
    }

    const char* EMSCRIPTEN_KEEPALIVE mural_bg_color(int mode) {
        int b;
        {
            std::lock_guard<std::mutex> l(dataMutex);
            b = biomeIdx;
        }
        std::vector<int> c = getPreviewBgColor(mode, biomeName(b));
        char buf[48];
        snprintf(buf, sizeof(buf), "%d,%d,%d", c[0], c[1], c[2]);
        g_bgColor = buf;
        return g_bgColor.c_str();
    }

    //////// console and status

    // the page writes its own lines into the engine log
    void EMSCRIPTEN_KEEPALIVE mural_log(const char* msg) {
        if (msg && *msg) Log(msg);
    }

    int EMSCRIPTEN_KEEPALIVE mural_log_count() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return (int)consoleLog.size();
    }

    const char* EMSCRIPTEN_KEEPALIVE mural_log_line(int i) {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (i < 0 || i >= (int)consoleLog.size()) { g_logLine.clear(); return g_logLine.c_str(); }
        g_logLine = consoleLog[i];
        return g_logLine.c_str();
    }

    // cells per overlay, and per connecting family the masks used + how many touch their own family
    const char* EMSCRIPTEN_KEEPALIVE mural_overlay_census_json() {
        static std::string out;
        std::lock_guard<std::mutex> lock(dataMutex);
        out = "{";
        const int w = previewWidth, h = previewHeight;
        if (previewGridIndices.empty() || activePalette->empty() || w <= 0 || h <= 0) {
            out += "\"cells\":0}";
            return out.c_str();
        }
        std::map<std::string, long long> byOverlay;
        std::map<std::string, std::map<int, std::pair<long long, long long> > > masks;    // family, mask: {total, beside the same family}
        auto famAt = [&](int r, int c) -> const std::string* {
            if (r < 0 || c < 0 || r >= h || c >= w) return nullptr;
            int i = previewGridIndices[(size_t)r * w + c];
            if (i < 0 || i >= (int)activePalette->size()) return nullptr;
            const ActiveBlock& b = (*activePalette)[i];
            if (b.ovMask < 0 || b.family == "none") return nullptr;
            return &b.family;
            };
        for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
                int i = previewGridIndices[(size_t)r * w + c];
                if (i < 0 || i >= (int)activePalette->size()) continue;
                const ActiveBlock& b = (*activePalette)[i];
                if (b.ovId.empty()) continue;
                byOverlay[b.ovId]++;
                if (b.ovMask < 0 || b.family == "none") continue;
                const std::string* n[4] = { famAt(r - 1,c), famAt(r + 1,c), famAt(r,c + 1), famAt(r,c - 1) };
                bool touching = false;
                for (int k = 0; k < 4; ++k) if (n[k] && *n[k] == b.family) touching = true;
                auto& slot = masks[b.family][b.ovMask & 15];
                slot.first++;
                if (touching) slot.second++;
            }
        }
        out += "\"cells\":" + std::to_string((long long)w * h) + ",\"byOverlay\":{";
        bool first = true;
        for (auto& kv : byOverlay) {
            if (!first) out += ',';
            first = false;
            jsonStr(kv.first, out);
            out += ':' + std::to_string(kv.second);
        }
        out += "},\"masks\":{";
        first = true;
        for (auto& fam : masks) {
            if (!first) out += ',';
            first = false;
            jsonStr(fam.first, out);
            out += ":{";
            bool f2 = true;
            for (auto& m : fam.second) {
                if (!f2) out += ',';
                f2 = false;
                out += '"' + std::to_string(m.first) + "\":[" + std::to_string(m.second.first)
                    + ',' + std::to_string(m.second.second) + ']';
            }
            out += '}';
        }
        out += "}}";
        return out.c_str();
    }

    // {"has":0|1,"lines":[4 strings],"warning":".."}
    const char* EMSCRIPTEN_KEEPALIVE mural_get_status_json() {
        std::lock_guard<std::mutex> lock(dataMutex);
        g_jsonStatus = "{\"has\":";
        g_jsonStatus += hasCalcStatus ? "1" : "0";
        g_jsonStatus += ",\"lines\":[";
        for (int i = 0; i < 4; ++i) {
            if (i) g_jsonStatus += ',';
            jsonStr(calcStatus[i], g_jsonStatus);
        }
        g_jsonStatus += "],\"warning\":";
        const std::string oldGame = oldGameWarning();
        jsonStr(deteriorationWarning + (deteriorationWarning.empty() || oldGame.empty() ? "" : " ") + oldGame, g_jsonStatus);
        g_jsonStatus += '}';
        return g_jsonStatus.c_str();
    }

    //////// exports
    void EMSCRIPTEN_KEEPALIVE mural_generate_datapack() {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            name = muralName;
        }
        generateDatapack(name);
    }

    void EMSCRIPTEN_KEEPALIVE mural_generate_litematic() {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            name = muralName;
        }
        generateLitematica(name);
    }

    //////// progress
    float EMSCRIPTEN_KEEPALIVE mural_progress() {
        const long long m = g_progressMax.load();
        return (float)((double)g_progressVal.load() / (double)(m > 0 ? m : 1));
    }

    // changes while the engine is doing anything at all, also where the bar stands still
    int EMSCRIPTEN_KEEPALIVE mural_heartbeat() {
        return (int)((g_progressVal.load() + g_alive.load()) & 0x7fffffff);
    }

}    // extern "C"
