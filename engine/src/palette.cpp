#include "engine_internal.h"


//////////////// BIOME COLOURS
std::vector<float> getBiomeTintVals(const std::string& baseName, const std::string& biomeName) {
    if (biomeTints.find(biomeName) == biomeTints.end()) return { 1.0f, 1.0f, 1.0f };
    BiomeTint t = biomeTints[biomeName];

    // leaf litter takes the dry foliage map, not the leaves one -- sampled from dry_foliage.png per biome
    if (baseName.find("leaf_litter") == 0) {
        static const std::map<std::string, std::array<int, 3>> dryFoliage = {
            {"Plains", {163, 117, 70}}, {"Forest", {163, 109, 70}}, {"Jungle", {163, 99, 70}},
            {"Swamp", {123, 83, 52}}, {"Badlands", {163, 128, 70}}, {"Desert/Savanna", {163, 128, 70}},
            {"Taiga", {154, 118, 79}}, {"Dark Forest", {123, 83, 52}}
        };
        auto it = dryFoliage.find(biomeName);
        if (it != dryFoliage.end()) return { it->second[0] / 255.f, it->second[1] / 255.f, it->second[2] / 255.f };
    }

    bool is_foliage = (baseName.find("oak_leaves") == 0 || baseName.find("jungle_leaves") == 0 ||
        baseName.find("acacia_leaves") == 0 || baseName.find("dark_oak_leaves") == 0 ||
        baseName.find("mangrove_leaves") == 0 || baseName.find("vine") == 0);
    bool is_birch = (baseName.find("birch_leaves") == 0);
    bool is_spruce = (baseName.find("spruce_leaves") == 0);
    bool is_grass = (baseName == "grass" || baseName.find("grass_block") == 0 ||
        baseName.find("short_grass") == 0 || baseName.find("tall_grass") == 0 ||
        baseName.find("fern") == 0 || baseName.find("large_fern") == 0);

    if (is_birch) return { 128 / 255.f, 167 / 255.f, 85 / 255.f };    // birch + spruce leaves dont follow the biome
    if (is_spruce) return { 97 / 255.f, 153 / 255.f, 97 / 255.f };
    if (is_foliage) return { t.fR, t.fG, t.fB };
    if (is_grass) return { t.gR, t.gG, t.gB };

    return { 1.0f, 1.0f, 1.0f };
}

std::vector<int> getPreviewBgColor(int mode, const std::string& biomeName) {
    if (mode == 1) return { 220, 220, 220 };    // light
    if (mode == 3) return { 128, 128, 128 };    // checkerboard, grey underneath

    // grass at this biome
    if (mode == 2) {
        std::vector<float> tint = getBiomeTintVals("grass", biomeName);
        if (allFiles.count("grass_block_top.png")) {
            const TextureRaw& tr = allFiles["grass_block_top.png"];
            long long r = 0, g = 0, b = 0;
            int count = 0;
            for (size_t i = 0; i < tr.data.size(); i += 4) {
                if (tr.data[i + 3] > 128) {
                    r += tr.data[i];
                    g += tr.data[i + 1];
                    b += tr.data[i + 2];
                    count++;
                }
            }
            if (count > 0) {
                return { (int)((r / count) * tint[0]), (int)((g / count) * tint[1]), (int)((b / count) * tint[2]) };
            }
        }
        return { (int)(tint[0] * 255), (int)(tint[1] * 255), (int)(tint[2] * 255) };
    }
    return { 21, 21, 21 };    // dark
}


//////////////// DATA FILES
void loadBlockFaces(const std::string& filepath) {
    blockFaceRows.clear();
    if (!std::filesystem::exists(filepath)) return;

    std::ifstream file(filepath);
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const char delim = sniffDelim(line);

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = splitCSV(line, delim);
        if (t.size() < 5 || t[0].empty() || t[1].empty() || t[2].empty()) continue;

        FaceFix f;
        f.state = t[1];
        f.texture = t[2] + ".png";
        f.turn = (t[3] == "90") ? 90 : (t[3] == "180") ? 180 : (t[3] == "270") ? 270 : 0;
        f.mirror = (t[4] == "1");
        blockFaceRows[t[0] + "|" + t[1].substr(0, t[1].find('['))].push_back(f);    // key: place|block id
    }
}

// needs g_gameVersions built first: each rule names its release by index
void loadVersionRenames(const std::string& filepath) {
    versionRenames.clear();
    if (!std::filesystem::exists(filepath)) return;

    std::ifstream file(filepath);
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const char delim = sniffDelim(line);

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto t = splitCSV(line, delim);
        if (t.size() < 3 || t[0].empty() || t[1].empty()) continue;

        VersionRename r;
        r.from = t[0];
        r.to = t[1];
        r.sinceIdx = versionIndexOf(t[2]);    // unknown release = 0, nothing falls below it -- a typo turns the rule off, not on everywhere
        versionRenames.push_back(r);
    }
}

void loadOverlayDatabase() {
    if (std::filesystem::exists("resources/overlay_ids.csv")) {
        std::ifstream file("resources/overlay_ids.csv");
        std::string line;
        std::getline(file, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const char delim = sniffDelim(line);
        auto headers = splitCSV(line, delim);

        // every overlay starts disallowed
        for (size_t i = 2; i < headers.size(); ++i) {
            std::string oId = headers[i];
            if (std::find(disallowedOverlayIds.begin(), disallowedOverlayIds.end(), oId) == disallowedOverlayIds.end()) {
                disallowedOverlayIds.push_back(oId);
            }
        }
        std::sort(disallowedOverlayIds.begin(), disallowedOverlayIds.end());

        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto tokens = splitCSV(line, delim);
            if (tokens.size() < 3) continue;

            std::string face = tokens[0];
            std::string baseId = tokens[1];

            if (face == "[version]") {    // row with the release of each overlay column
                for (size_t i = 2; i < tokens.size() && i < headers.size(); ++i) {
                    if (!tokens[i].empty()) g_overlayVersionRaw[headers[i]] = tokens[i];
                }
                continue;
            }

            for (size_t i = 2; i < tokens.size() && i < headers.size(); ++i) {
                bool allowed = (tokens[i] == "1");
                if (face == "top") overlayMatrixTop[baseId][headers[i]] = allowed;
                else if (face == "side") overlayMatrixSide[baseId][headers[i]] = allowed;
                else if (face == "bottom") overlayMatrixBottom[baseId][headers[i]] = allowed;
            }
        }
    }

    if (std::filesystem::exists("resources/overlay_states.csv")) {
        std::ifstream file("resources/overlay_states.csv");
        std::string line;
        std::getline(file, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const char delim = sniffDelim(line);

        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto tokens = splitCSV(line, delim);
            if (tokens.size() < 7) continue;

            OverlayState os;
            os.overlay_id = tokens[0];
            std::string face = tokens[1];
            os.blockstate = tokens[2];
            os.textures = splitCSV(tokens[3], '|');

            auto rotStrs = splitCSV(tokens[4], '|');
            for (const auto& rs : rotStrs) {
                int rot = 0;    // junk = 0, not an abort
                try { rot = std::stoi(rs); }
                catch (...) { rot = 0; }
                os.rotations.push_back(rot);
            }

            os.family = tokens[5];

            auto conns = splitCSV(tokens[6], ',');
            os.mask = 0;
            if (conns.size() == 4) {
                if (!conns[0].empty() && conns[0][0] == '1') os.mask |= OV_N;
                if (!conns[1].empty() && conns[1][0] == '1') os.mask |= OV_S;
                if (!conns[2].empty() && conns[2][0] == '1') os.mask |= OV_E;
                if (!conns[3].empty() && conns[3][0] == '1') os.mask |= OV_W;
            }

            if (face == "top") overlayStatesTop.push_back(os);
            else if (face == "bottom") overlayStatesBottom.push_back(os);
            else {
                overlayStatesSide.push_back(os);
            }
        }
    }
}

void setDroppedImage(const char* path) {
#ifndef __EMSCRIPTEN__

    // native builds read the picture in calculatePreview
    snprintf(imagePath, sizeof(imagePath), "%s", path);
    lastChangeTime = std::chrono::steady_clock::now();
#else
    (void)path;
#endif
}


//////////////// BLOCK DATABASE
void loadBlockDatabase() {
    if (databaseLoaded) return;
    if (!std::filesystem::exists("resources/block_textures") || !std::filesystem::exists("resources/allowed_ids.csv")) {
        Log("CRITICAL ERROR: 'resources/block_textures' folder or 'resources/allowed_ids.csv' not found!");
        return;
    }
    Log("=== Moose's Minecraft Mural Maker ===");
    loadBlockFaces("resources/block_faces.csv");
    loadOverlayDatabase();

    for (const auto& entry : std::filesystem::directory_iterator("resources/block_textures")) {
        if (entry.path().extension() == ".png") {
            std::string filename = entry.path().filename().string();
            int w, h, c;
            unsigned char* img = stbi_load(entry.path().string().c_str(), &w, &h, &c, 4);
            if (img) {
                TextureRaw tr; tr.w = w; tr.h = h;
                tr.data.assign(img, img + (w * h * 4));
                allFiles[filename] = tr;
                stbi_image_free(img);
            }
        }
    }

#if MMMM_VERBOSE
    Log("Parsing Dynamic Rule Databases...");
#endif
    std::ifstream csvFile("resources/allowed_ids.csv");
    std::string line;
    std::getline(csvFile, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const char delim = sniffDelim(line);

    {

        // every column after Order is a filter group, named by its header -- a new group = a new column
        auto header = splitCSV(line, delim);
        g_blockGroupNames.clear();
        for (size_t i = kFirstGroupColumn; i < header.size() && i < kFirstGroupColumn + 32; ++i) {
            g_blockGroupNames.push_back(header[i]);
        }
    }

    // versions turn into indices once the whole list is known: a row may name one no earlier row did
    std::map<std::string, std::string> rowVersion;
    int nextOrder = 0;

    while (std::getline(csvFile, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto tokens = splitCSV(line, delim);
        if (tokens.empty()) continue;

        BlockData bd;
        bd.id = tokens[0];
        if (tokens.size() > 9) rowVersion[bd.id] = tokens[9];

        // the Order column, not the row position -- a row without a number lands after the one before it
        if (tokens.size() > 10) {
            const std::string& ordStr = tokens[10];
            bool numeric = !ordStr.empty() &&
                std::all_of(ordStr.begin(), ordStr.end(), [](unsigned char c) { return std::isdigit(c); });
            if (numeric) nextOrder = std::atoi(ordStr.c_str());
        }
        bd.order = nextOrder++;

        if (tokens.size() > 2) bd.isEntity = (tokens[2] == "1");
        if (tokens.size() > 3) bd.emitsLight = (tokens[3] == "1");
        if (tokens.size() > 4) bd.obeysGravity = (tokens[4] == "1");
        if (tokens.size() > 5) bd.transparent = (tokens[5] == "1");

        ExtendedBlockInfo extInfo;
        if (tokens.size() > 6) extInfo.isPainToGet = (tokens[6] == "1");
        if (tokens.size() > 7) extInfo.deteriorates = (tokens[7] == "1");
        if (tokens.size() > 8) extInfo.moosesChoice = (tokens[8] == "1");
        for (size_t g = 0; g < g_blockGroupNames.size() && kFirstGroupColumn + g < tokens.size(); ++g) {
            if (tokens[kFirstGroupColumn + g] == "1") extInfo.groups |= (1u << g);
        }

        std::vector<std::string> modifiers = { "front", "side", "top" };
        std::string ui_file = "";

        // shulker boxes have only the mob texture: top, bottom + side are cut out of it
        if (bd.id.find("shulker_box") != std::string::npos) {
            std::string shulkerColor = "shulker";
            if (bd.id != "shulker_box") shulkerColor = "shulker_" + bd.id.substr(0, bd.id.find("_shulker_box"));

            std::string shulkerFile = shulkerColor + ".png";
            if (allFiles.find(shulkerFile) == allFiles.end()) shulkerFile = "shulker.png";
            if (allFiles.find(shulkerFile) == allFiles.end()) shulkerFile = bd.id + ".png";

            if (allFiles.find(shulkerFile) != allFiles.end()) {
                const TextureRaw& tr = allFiles[shulkerFile];
                if (tr.w >= 64 && tr.h >= 64) {
                    extInfo.uiRgbaArray.resize(16 * 16 * 4, 0);
                    std::vector<float> tint = getBiomeTintVals(bd.id, "Plains");

                    TextureRaw tTr, bTr, sTr;
                    tTr.w = 16; tTr.h = 16; tTr.data.resize(16 * 16 * 4, 0);
                    bTr.w = 16; bTr.h = 16; bTr.data.resize(16 * 16 * 4, 0);
                    sTr.w = 16; sTr.h = 16; sTr.data.resize(16 * 16 * 4, 0);

                    for (int y = 0; y < 16; ++y) {
                        for (int x = 0; x < 16; ++x) {
                            int dstIdx = (y * 16 + x) * 4;

                            int srcT = ((y + 0) * tr.w + (x + 16)) * 4;
                            tTr.data[dstIdx] = tr.data[srcT]; tTr.data[dstIdx + 1] = tr.data[srcT + 1];
                            tTr.data[dstIdx + 2] = tr.data[srcT + 2]; tTr.data[dstIdx + 3] = tr.data[srcT + 3];

                            float alphaUI = tr.data[srcT + 3] / 255.0f;
                            extInfo.uiRgbaArray[dstIdx] = (unsigned char)(std::min(255.0f, tr.data[srcT] * tint[0] * alphaUI));
                            extInfo.uiRgbaArray[dstIdx + 1] = (unsigned char)(std::min(255.0f, tr.data[srcT + 1] * tint[1] * alphaUI));
                            extInfo.uiRgbaArray[dstIdx + 2] = (unsigned char)(std::min(255.0f, tr.data[srcT + 2] * tint[2] * alphaUI));
                            extInfo.uiRgbaArray[dstIdx + 3] = tr.data[srcT + 3];

                            int srcB = ((y + 28) * tr.w + (x + 32)) * 4;
                            bTr.data[dstIdx] = tr.data[srcB]; bTr.data[dstIdx + 1] = tr.data[srcB + 1];
                            bTr.data[dstIdx + 2] = tr.data[srcB + 2]; bTr.data[dstIdx + 3] = tr.data[srcB + 3];

                            int srcBg = ((y + 36) * tr.w + (x + 0)) * 4;
                            int srcFg = ((y + 16) * tr.w + (x + 0)) * 4;
                            float alphaFg = tr.data[srcFg + 3] / 255.0f;
                            sTr.data[dstIdx] = (unsigned char)((tr.data[srcFg] * alphaFg) + (tr.data[srcBg] * (1.0f - alphaFg)));
                            sTr.data[dstIdx + 1] = (unsigned char)((tr.data[srcFg + 1] * alphaFg) + (tr.data[srcBg + 1] * (1.0f - alphaFg)));
                            sTr.data[dstIdx + 2] = (unsigned char)((tr.data[srcFg + 2] * alphaFg) + (tr.data[srcBg + 2] * (1.0f - alphaFg)));
                            sTr.data[dstIdx + 3] = 255;
                        }
                    }
                    allFiles[bd.id + "_top.png"] = tTr;
                    allFiles[bd.id + "_bottom.png"] = bTr;
                    allFiles[bd.id + "_side.png"] = sTr;
                    allFiles[bd.id + "_front.png"] = sTr;
                    allFiles[bd.id + "_back.png"] = sTr;

                    masterDatabase.push_back(bd);
                    extBlockInfo[bd.id] = extInfo;
                    continue;
                }
            }
        }

        if (allFiles.count(bd.id + ".png")) ui_file = bd.id + ".png";
        else {
            for (const auto& m : modifiers) {
                if (allFiles.count(bd.id + "_" + m + ".png")) { ui_file = bd.id + "_" + m + ".png"; break; }
            }
        }

        // no texture under the blocks own name: the first one the face table shows, floor before walls before ceiling
        if (ui_file.empty()) {
            static const char* const places[] = { "floor", "wall north", "wall south", "wall east", "wall west", "ceiling" };
            for (const char* place : places) {
                auto rows = blockFaceRows.find(std::string(place) + "|" + bd.id);
                if (rows == blockFaceRows.end()) continue;

                // a block that can point up shows its top (sticky piston)
                const FaceFix* pick = &rows->second[0];
                for (const FaceFix& f : rows->second) if (f.state.find("facing=up") != std::string::npos) { pick = &f; break; }
                if (allFiles.count(pick->texture)) { ui_file = pick->texture; break; }
            }
        }

        if (!ui_file.empty()) {
            const TextureRaw& tr = allFiles[ui_file];
            extInfo.uiRgbaArray.resize(16 * 16 * 4, 0);

            std::vector<float> tint = getBiomeTintVals(bd.id, "Plains");

            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    int dstIdx = (y * 16 + x) * 4;
                    if (x < tr.w && y < tr.h) {
                        int srcIdx = (y * tr.w + x) * 4;
                        extInfo.uiRgbaArray[dstIdx] = (unsigned char)(std::min(255.0f, tr.data[srcIdx] * tint[0]));
                        extInfo.uiRgbaArray[dstIdx + 1] = (unsigned char)(std::min(255.0f, tr.data[srcIdx + 1] * tint[1]));
                        extInfo.uiRgbaArray[dstIdx + 2] = (unsigned char)(std::min(255.0f, tr.data[srcIdx + 2] * tint[2]));
                        extInfo.uiRgbaArray[dstIdx + 3] = tr.data[srcIdx + 3];
                    }
                    else {
                        extInfo.uiRgbaArray[dstIdx] = 0; extInfo.uiRgbaArray[dstIdx + 1] = 0; extInfo.uiRgbaArray[dstIdx + 2] = 0; extInfo.uiRgbaArray[dstIdx + 3] = 0;
                    }
                }
            }
            masterDatabase.push_back(bd);
            extBlockInfo[bd.id] = extInfo;
        }
    }

    {

        // one sorted version list for blocks + overlays, deduped on the parsed pair so order and identity agree
        std::set<std::pair<int, int>> seen;
        std::vector<std::pair<std::pair<int, int>, std::string>> ordered;
        auto add = [&](const std::string& v) {
            if (v.empty()) return;
            auto key = parseVersion(v);
            if (seen.insert(key).second) ordered.push_back({ key, v });
        };
        for (const auto& kv : rowVersion) add(kv.second);
        for (const auto& kv : g_overlayVersionRaw) add(kv.second);
        std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        g_gameVersions.clear();
        for (const auto& o : ordered) g_gameVersions.push_back(o.second);
        if (g_gameVersions.empty()) g_gameVersions.push_back("1.13");
        selectedVersionIdx = (int)g_gameVersions.size() - 1;
    }

    // a row without a version = index 0, the oldest -- the block stays available, not gone everywhere
    for (auto& bd : masterDatabase) {
        auto it = rowVersion.find(bd.id);
        if (it != rowVersion.end()) bd.versionIdx = versionIndexOf(it->second);
    }

    loadVersionRenames("resources/version_renames.csv");

    for (auto& bd : masterDatabase) allowedBlocks.push_back(&bd);
    sortBlockList(allowedBlocks);
    databaseLoaded = true;
    Log("Ready for configuration.");
}


//////////////// PALETTE OF ONE MURAL
std::vector<ActiveBlock> buildActivePalette(const std::string& ori, const std::string& drct, const std::string& biomeName, const std::vector<BlockData*>& activeBlocks, int bgMode, bool optUseOverlays, const std::vector<std::string>& optAllowedOverlays, bool isDelta) {
    std::vector<ActiveBlock> palette;
    std::vector<int> bg = getPreviewBgColor(bgMode, biomeName);
    const std::string placeName = (ori == "vertical") ? ("wall " + drct) : (drct == "bottom" ? "ceiling" : "floor");

    // one palette entry: a block side from the face table (mirrored, then turned clockwise), or ready-made pixels
    auto addToDatabase = [&](const std::string& mcString, const std::string& chosenFile, int rotDegrees, bool mirrored = false,
        const std::vector<unsigned char>* customRgba = nullptr, bool needsScoringData = true) {
        if (customRgba == nullptr && allFiles.find(chosenFile) == allFiles.end()) return;

        std::string finalMcString = mcString;

        // a slab shows a full block only doubled
        if (finalMcString.find("_slab") != std::string::npos && finalMcString.find("type=double") == std::string::npos) {
            if (finalMcString.find("[") == std::string::npos) finalMcString += "[type=double]";
            else finalMcString.insert(finalMcString.find("[") + 1, "type=double,");
        }

        std::string baseName = chosenFile.substr(0, chosenFile.length() - 4);
        std::vector<float> tint = getBiomeTintVals(baseName, biomeName);

        std::vector<unsigned char> uiRgba(16 * 16 * 4, 0);    // as it is, for the page
        std::vector<unsigned char> mathRgb(16 * 16 * 3, 0);    // over the background, for the solver

        const TextureRaw* tr = (customRgba == nullptr) ? &allFiles[chosenFile] : nullptr;

        std::vector<float> rawAlphas(256, 100.0f);
        float totalAlpha = 0.0f;

        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int mx = mirrored ? 15 - x : x;
                int nx = mx, ny = y;
                if (rotDegrees == 90) { nx = 15 - y; ny = mx; }
                else if (rotDegrees == 180) { nx = 15 - mx; ny = 15 - y; }
                else if (rotDegrees == 270) { nx = y; ny = 15 - mx; }

                int uiIdx = (ny * 16 + nx) * 4;
                int mathIdx = (ny * 16 + nx) * 3;

                float r_src = 0, g_src = 0, b_src = 0, alpha = 0;
                if (customRgba != nullptr) {
                    int srcIdx = (y * 16 + x) * 4;
                    r_src = (*customRgba)[srcIdx]; g_src = (*customRgba)[srcIdx + 1]; b_src = (*customRgba)[srcIdx + 2]; alpha = (*customRgba)[srcIdx + 3] / 255.0f;
                }
                else if (x < tr->w && y < tr->h) {
                    int srcIdx = (y * tr->w + x) * 4;
                    r_src = tr->data[srcIdx]; g_src = tr->data[srcIdx + 1]; b_src = tr->data[srcIdx + 2]; alpha = tr->data[srcIdx + 3] / 255.0f;
                }

                rawAlphas[ny * 16 + nx] = alpha * 100.0f;
                totalAlpha += (alpha * 100.0f);

                uiRgba[uiIdx] = (unsigned char)(std::min(255.0f, r_src * tint[0]));
                uiRgba[uiIdx + 1] = (unsigned char)(std::min(255.0f, g_src * tint[1]));
                uiRgba[uiIdx + 2] = (unsigned char)(std::min(255.0f, b_src * tint[2]));
                uiRgba[uiIdx + 3] = (unsigned char)(alpha * 255.0f);

                mathRgb[mathIdx] = (unsigned char)(std::min(255.0f, ((r_src * tint[0]) * alpha) + (bg[0] * (1.0f - alpha))));
                mathRgb[mathIdx + 1] = (unsigned char)(std::min(255.0f, ((g_src * tint[1]) * alpha) + (bg[1] * (1.0f - alpha))));
                mathRgb[mathIdx + 2] = (unsigned char)(std::min(255.0f, ((b_src * tint[2]) * alpha) + (bg[2] * (1.0f - alpha))));
            }
        }

        ActiveBlock ab;
        ab.mc_id = finalMcString; ab.rgbArray = std::move(uiRgba);
        ab.flatLab = convertRGBtoLAB(mathRgb.data(), 16, 16);
        for (int i = 0; i < 256; ++i) ab.flatLab[i].alpha = rawAlphas[i];

        // mean + sorted lab are only read by pass 1 scoring, which skips deduped variants (~20k overlay entries)
        // flatLab + the mips stay for all: the rail builder can land on any variant, pass 2 + the snapshot read them
        if (needsScoringData || g_legacyOverlayPrep.load() != 0) {
            float sumL = 0, sumA = 0, sumB = 0;
            for (int i = 0; i < 256; ++i) {
                sumL += ab.flatLab[i].l;
                sumA += ab.flatLab[i].a;
                sumB += ab.flatLab[i].b;
            }
            ab.meanLab = { sumL / 256.0f, sumA / 256.0f, sumB / 256.0f, totalAlpha / 256.0f };
            ab.sortedLab = ab.flatLab;
            std::sort(ab.sortedLab.begin(), ab.sortedLab.end(), [](const ColorLab& a, const ColorLab& b) { return a.l < b.l; });
        }
        else {
            ab.meanLab = { 0.0f, 0.0f, 0.0f, 0.0f };
        }

        long long sR = 0, sG = 0, sB = 0, sA = 0;
        for (int i = 0; i < 256; ++i) {
            sR += ab.rgbArray[i * 4]; sG += ab.rgbArray[i * 4 + 1];
            sB += ab.rgbArray[i * 4 + 2]; sA += ab.rgbArray[i * 4 + 3];
        }
        ab.meanR = (unsigned char)(sR / 256); ab.meanG = (unsigned char)(sG / 256);
        ab.meanB = (unsigned char)(sB / 256); ab.meanA = (unsigned char)(sA / 256);

        ab.rgbLOD8.resize(8 * 8 * 4);
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                for (int ir = 0; ir < 2; ++ir) {
                    for (int ic = 0; ic < 2; ++ic) {
                        int idx = ((r * 2 + ir) * 16 + (c * 2 + ic)) * 4;
                        sumR += ab.rgbArray[idx]; sumG += ab.rgbArray[idx + 1]; sumB += ab.rgbArray[idx + 2]; sumA += ab.rgbArray[idx + 3];
                    }
                }
                int dst = (r * 8 + c) * 4;
                ab.rgbLOD8[dst] = sumR / 4; ab.rgbLOD8[dst + 1] = sumG / 4;
                ab.rgbLOD8[dst + 2] = sumB / 4; ab.rgbLOD8[dst + 3] = sumA / 4;
            }
        }

        ab.rgbLOD4.resize(4 * 4 * 4);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                for (int ir = 0; ir < 4; ++ir) {
                    for (int ic = 0; ic < 4; ++ic) {
                        int idx = ((r * 4 + ir) * 16 + (c * 4 + ic)) * 4;
                        sumR += ab.rgbArray[idx]; sumG += ab.rgbArray[idx + 1]; sumB += ab.rgbArray[idx + 2]; sumA += ab.rgbArray[idx + 3];
                    }
                }
                int dst = (r * 4 + c) * 4;
                ab.rgbLOD4[dst] = sumR / 16; ab.rgbLOD4[dst + 1] = sumG / 16;
                ab.rgbLOD4[dst + 2] = sumB / 16; ab.rgbLOD4[dst + 3] = sumA / 16;
            }
        }
        palette.push_back(std::move(ab));
        };

    if (!isDelta) {    // a delta build adds to a palette that has air already
        ActiveBlock airBlock;
        airBlock.mc_id = "air";
        airBlock.rgbArray.assign(16 * 16 * 4, 0);
        airBlock.meanLab = { 0.0f, 0.0f, 0.0f, 0.0f };
        airBlock.flatLab.assign(256, { 0.0f, 0.0f, 0.0f, 0.0f });
        airBlock.sortedLab = airBlock.flatLab;
        airBlock.meanR = 0; airBlock.meanG = 0; airBlock.meanB = 0; airBlock.meanA = 0;
        airBlock.rgbLOD8.assign(8 * 8 * 4, 0);
        airBlock.rgbLOD4.assign(4 * 4 * 4, 0);
        palette.push_back(airBlock);
    }

    // every side a block can show here is a face table row, in palette order -- no rows = not offered here
    for (BlockData* bd : activeBlocks) {
        g_progressVal += wtPal;
        auto rows = blockFaceRows.find(placeName + "|" + bd->id);
        if (rows == blockFaceRows.end()) continue;
        for (const FaceFix& f : rows->second) addToDatabase(f.state, f.texture, f.turn, f.mirror);
    }

    //////// overlays
    if (optUseOverlays) {
        int basePaletteSize = palette.size();

        std::string visibleFace = "top";
        if (ori == "vertical") visibleFace = "side";
        else if (ori == "horizontal" && drct == "bottom") visibleFace = "bottom";

        auto& activeMatrix = (visibleFace == "top") ? overlayMatrixTop : ((visibleFace == "side") ? overlayMatrixSide : overlayMatrixBottom);
        auto& activeStates = (visibleFace == "top") ? overlayStatesTop : ((visibleFace == "side") ? overlayStatesSide : overlayStatesBottom);

        std::set<std::string> activeOv(optAllowedOverlays.begin(), optAllowedOverlays.end());

        // same key as the pass 1 dedup, so needsScoringData skips exactly what scoring skips
        // one map for the whole call is safe: a delta build only adds variants of new blocks
        std::unordered_map<std::string, size_t> familyRepBuild;

        for (int i = 0; i < basePaletteSize; ++i) {
            g_progressVal += wtPal;
            if (palette[i].mc_id == "air") continue;

            // own copies of the two fields in use -- the palette grows below, a reference into it would not last
            const std::string baseId = palette[i].mc_id;
            const std::vector<unsigned char> baseRgba = palette[i].rgbArray;

            // overlays allowed on this block, leaves + trapdoors by family, else the catch-all row
            std::map<std::string, bool>* validOverlays = nullptr;
            std::string rawId = baseId.substr(0, baseId.find('['));

            if (activeMatrix.count(rawId)) validOverlays = &activeMatrix[rawId];
            else if (rawId.find("_leaves") != std::string::npos && activeMatrix.count("LEAVES")) validOverlays = &activeMatrix["LEAVES"];
            else if (rawId.find("_trapdoor") != std::string::npos && activeMatrix.count("TRAPDOORS")) validOverlays = &activeMatrix["TRAPDOORS"];
            else if (activeMatrix.count("OTHER_BLOCKS")) validOverlays = &activeMatrix["OTHER_BLOCKS"];

            if (!validOverlays) continue;

            for (const auto& os : activeStates) {
                if (activeOv.count(os.overlay_id) && (*validOverlays)[os.overlay_id]) {

                    std::vector<unsigned char> compositeRgba = baseRgba;

                    // dust part by part as the game draws it (redstone_wire.json): a dot alone or where arms meet at a corner
                    // north + south = halves of line0, west + east = halves of line1 turned 270 -- the csv textures arent used
                    if (os.family == "redstone") {
                        const float rt[3] = { 75.0f / 255.0f, 0.0f, 0.0f };    // #4B0000, dust at power 0
                        auto stamp = [&](const char* file, int y0, int y1, bool turned) {
                            auto it = allFiles.find(file);
                            if (it == allFiles.end()) return;
                            const TextureRaw& tex = it->second;
                            for (int y = y0; y < y1; ++y) {
                                for (int x = 0; x < 16; ++x) {
                                    const size_t srcIdx = (size_t)(y * 16 + x) * 4;
                                    if (srcIdx + 3 >= tex.data.size()) continue;
                                    const float alpha = tex.data[srcIdx + 3] / 255.0f;
                                    if (alpha <= 0.01f) continue;
                                    const int nx = turned ? y : x, ny = turned ? 15 - x : y;
                                    const int dstIdx = (ny * 16 + nx) * 4;
                                    for (int k = 0; k < 3; ++k) {
                                        compositeRgba[dstIdx + k] = (unsigned char)((tex.data[srcIdx + k] * rt[k] * alpha)
                                            + (compositeRgba[dstIdx + k] * (1.0f - alpha)));
                                    }
                                    compositeRgba[dstIdx + 3] = 255;
                                }
                            }
                            };
                        const int m = os.mask;
                        const bool ns = (m & (OV_N | OV_S)) != 0, ew = (m & (OV_E | OV_W)) != 0;
                        if ((!ns && !ew) || (ns && ew)) stamp("redstone_dust_dot.png", 0, 16, false);
                        if (m & OV_N) stamp("redstone_dust_line0.png", 0, 8, false);
                        if (m & OV_S) stamp("redstone_dust_line0.png", 8, 16, false);
                        if (m & OV_W) stamp("redstone_dust_line1.png", 0, 8, true);
                        if (m & OV_E) stamp("redstone_dust_line1.png", 8, 16, true);
                    }

                    for (size_t tIdx = 0; tIdx < os.textures.size(); ++tIdx) {
                        if (os.family == "redstone") break;    // drawn above
                        if (allFiles.find(os.textures[tIdx]) == allFiles.end()) continue;
                        TextureRaw& overlayTex = allFiles[os.textures[tIdx]];
                        int rot = (tIdx < os.rotations.size()) ? os.rotations[tIdx] : 0;

                        // vines + leaf litter follow the biome
                        std::vector<float> ovTint = { 1.0f, 1.0f, 1.0f };
                        if (os.overlay_id == "vine" || os.overlay_id == "leaf_litter") {
                            ovTint = getBiomeTintVals(os.overlay_id, biomeName);
                        }

                        // petals, leaf litter and wildflowers fill one to four quarters of the cell
                        const bool quartered = os.overlay_id == "pink_petals" || os.overlay_id == "leaf_litter" || os.overlay_id == "wildflowers";
                        int amount = 4;
                        if (quartered) {

                            // "flower_amount=" and "segment_amount=" end the same way
                            const size_t pos = os.blockstate.find("amount=");
                            if (pos != std::string::npos) amount = os.blockstate[pos + 7] - '0';
                        }

                        for (int y = 0; y < 16; ++y) {
                            for (int x = 0; x < 16; ++x) {
                                bool keep = true;
                                if (quartered) {

                                    // the game order for facing=north: north-west, south-west, south-east, north-east (flowerbed_1..4)
                                    keep = false;
                                    if (amount >= 1 && x < 8 && y < 8)   keep = true;
                                    if (amount >= 2 && x < 8 && y >= 8)  keep = true;
                                    if (amount >= 3 && x >= 8 && y >= 8) keep = true;
                                    if (amount == 4 && x >= 8 && y < 8)  keep = true;
                                }

                                if (!keep) continue;

                                int nx = x, ny = y;
                                if (rot == 90) { nx = 15 - y; ny = x; }
                                else if (rot == 180) { nx = 15 - x; ny = 15 - y; }
                                else if (rot == 270) { nx = y; ny = 15 - x; }

                                int srcIdx = (y * 16 + x) * 4;
                                int dstIdx = (ny * 16 + nx) * 4;

                                float alpha = (keep && srcIdx < overlayTex.data.size()) ? (overlayTex.data[srcIdx + 3] / 255.0f) : 0.0f;

                                if (alpha > 0.01f) {
                                    float oR = overlayTex.data[srcIdx] * ovTint[0];
                                    float oG = overlayTex.data[srcIdx + 1] * ovTint[1];
                                    float oB = overlayTex.data[srcIdx + 2] * ovTint[2];

                                    compositeRgba[dstIdx] = (unsigned char)((oR * alpha) + (compositeRgba[dstIdx] * (1.0f - alpha)));
                                    compositeRgba[dstIdx + 1] = (unsigned char)((oG * alpha) + (compositeRgba[dstIdx + 1] * (1.0f - alpha)));
                                    compositeRgba[dstIdx + 2] = (unsigned char)((oB * alpha) + (compositeRgba[dstIdx + 2] * (1.0f - alpha)));
                                    compositeRgba[dstIdx + 3] = 255;
                                }
                            }
                        }
                    }

                    std::string vMcId = baseId;

                    // no scoring data for a variant whose pixels match the first of its key -- pass 1 skips it anyway
                    bool needsScoringData = true;
                    std::string familyKey = vMcId + "\x1f" + os.overlay_id;
                    auto repIt = familyRepBuild.find(familyKey);
                    if (repIt != familyRepBuild.end()) {
                        bool renderIsIdentical = palette[repIt->second].rgbArray == compositeRgba;
                        if (renderIsIdentical) needsScoringData = false;
                    }

                    size_t sizeBefore = palette.size();
                    addToDatabase(vMcId, "", 0, false, &compositeRgba, needsScoringData);
                    if (palette.size() == sizeBefore) continue;

                    if (repIt == familyRepBuild.end()) familyRepBuild.emplace(familyKey, palette.size() - 1);

                    ActiveBlock& newBlock = palette.back();

                    // wall overlays take their facing from the wall direction
                    std::string finalState = os.blockstate;
                    if (ori == "vertical") {
                        bool isBooleanFace = (os.overlay_id.find("vine") != std::string::npos ||
                            os.overlay_id.find("resin_clump") != std::string::npos ||
                            os.overlay_id.find("glow_lichen") != std::string::npos ||
                            os.overlay_id.find("sculk_vein") != std::string::npos);

                        bool isLadder = (os.overlay_id.find("ladder") != std::string::npos);

                        if (isBooleanFace) {
                            if (drct == "north") finalState = "south=true";
                            else if (drct == "south") finalState = "north=true";
                            else if (drct == "east")  finalState = "west=true";
                            else if (drct == "west")  finalState = "east=true";
                        }
                        else if (isLadder) {
                            if (drct == "north") finalState = "facing=north";
                            else if (drct == "south") finalState = "facing=south";
                            else if (drct == "east")  finalState = "facing=east";
                            else if (drct == "west")  finalState = "facing=west";
                        }
                    }

                    newBlock.ovId = os.overlay_id;
                    newBlock.ovState = finalState;
                    newBlock.family = os.family;
                    newBlock.ovMask = os.mask;
                    newBlock.ovBaseIdx = i;    // palette[0..basePaletteSize) are the bare blocks
                }
            }
        }
    }

#if defined(__EMSCRIPTEN__) && MMMM_VERBOSE
    size_t currentHeapSize = EM_ASM_INT({ return HEAP8.length; });
    DBG_PRINT("[MEMORY STATS] Current WASM Heap Size: %.2f MB\n", (float)currentHeapSize / (1024.0f * 1024.0f));
#endif

    return palette;
}
