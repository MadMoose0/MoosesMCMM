#include "engine_internal.h"


//////////////// ABANDONED RUN

// nothing usable: the preview outputs go too, or canvas + exports keep a mural these settings dont make
// the caches stay, they are the delta baseline -- an empty grid already means rebuild from scratch
void abandonPreview(const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        activePalette = std::make_shared<const std::vector<ActiveBlock>>();
        previewGridIndices.clear();
        engineGridIndices.clear();
        committedRailWishes.clear();
        previewRailPlan = RailPlan();
        deteriorationWarning = why;
        g_oldGameCells = OldGameCells();

        // the status lines went out before the palette was known to work, they would describe the last mural
        hasCalcStatus = false;
        previewSerial++;
    }
}


//////////////// SETTINGS
bool PreviewRun::readSettings() {
    t_preview_start = std::chrono::high_resolution_clock::now();
    myCalcId = currentCalcId.load();
    DBG_PRINT("\n[ENGINE] === STARTING PREVIEW CALCULATION (calcId=%d) ===\n", myCalcId);
    DBG_PRINT("[ENGINE] Thread pool size: %d\n", getOptimalThreadCount());
    DBG_HEAP("0/5 start");
    std::string imgPath;

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        imgPath = imagePath;
        tW = targetWidth; tH = targetHeight;
        wC = weightColor; wS = weightSpatial; wT = weightTexture;
        dMeth = ditherMethod; dInt = ditherIntensity;
        oIdx = orientationIdx; dIdx = directionIdx; bIdx = biomeIdx;
        currentBgMode = previewBgMode;
        localAllowedBlocks = allowedBlocks;
        localUseOverlays = useOverlays;
        localAllowedOverlays = allowedOverlayIds;

        p2Enabled = enablePass2; p2Chunk = pass2ChunkSizeIdx; p2Thresh = pass2Threshold;
        p2MSE = weightMSE; p2SSIM = weightSSIM;
        p2Passes = pass2Passes;
        p2Cands = pass2Candidates;
        enhOn = enableEnhance; enhSet = enhanceSettings;
    }

    // no picture yet is the normal start, said nothing -- an empty allow list is a choice the user gets told about
    if (imgPath.empty() || tW <= 0 || tH <= 0) {
        return false;
    }

    if (localAllowedBlocks.empty()) {
        abandonPreview("No blocks are allowed. Move at least one into the allowed list.");
        return false;
    }

    if (imgPath != cachedImagePath || origData.empty()) {
#ifdef __EMSCRIPTEN__
        return false;
#else
        DBG_PRINT("[SUPERVISOR] Native Desktop: Decoding image via stb_image...\n");
        int channels;
        unsigned char* imgData = stbi_load(imgPath.c_str(), &origW, &origH, &channels, 4);
        if (!imgData) {
            DBG_PRINT("[SUPERVISOR] Failed to load image!\n");
            return false;
        }
        std::vector<unsigned char>().swap(origData);
        origData.assign(imgData, imgData + (origW * origH * 4));
        srcW = origW; srcH = origH;
        stbi_image_free(imgData);
        cachedImagePath = imgPath;
        cachedTW = 0;
#endif
    }
    return true;
}


//////////////// PICTURE
bool PreviewRun::prepareImage() {
    float currentAspect = (float)origW / (float)origH;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (aspectR != currentAspect) {
            aspectR = currentAspect;
            if (lockAspect) {

                // new picture with the aspect locked: 70 blocks on the longer side
                if (origW >= origH) {
                    targetWidth = 70;
                    targetHeight = static_cast<int>(std::round(70.0 * origH / origW));
                }
                else {
                    targetHeight = 70;
                    targetWidth = static_cast<int>(std::round(70.0 * origW / origH));
                }
                if (targetHeight <= 0) targetHeight = 1;
                if (targetWidth <= 0) targetWidth = 1;
                if (targetWidth > 1000) targetWidth = 1000;
                if (targetHeight > 1000) targetHeight = 1000;

                tH = targetHeight;
                tW = targetWidth;
            }
        }
    }

    targetW_px = tW * 16, targetH_px = tH * 16;

    // lab samples per block = real source pixels per block, up to 16 -- upscaling would invent detail
    // per axis, source and mural aspect can differ
    const bool legacyUpscale = (g_legacyUpscale.load() != 0);
    Neff_x = legacyUpscale ? 16 : std::clamp((int)std::lround((double)origW / tW), 1, 16);
    Neff_y = legacyUpscale ? 16 : std::clamp((int)std::lround((double)origH / tH), 1, 16);
    labW_px = tW * Neff_x, labH_px = tH * Neff_y;

    {
        std::lock_guard<std::mutex> lock(dataMutex);

        // over 16 px per block on a side: kept only at the size the mural reads, made from the whole picture each time
        const bool reduce = origW > 16 * tW || origH > 16 * tH;
        const int needW = reduce ? labW_px : origW, needH = reduce ? labH_px : origH;
        if (srcW != needW || srcH != needH) {
            if (srcW != origW || srcH != origH || origData.size() != (size_t)origW * origH * 4) {
                g_sourceWanted = true;
                return false;
            }
            std::vector<unsigned char> reduced((size_t)needW * needH * 4);
            stbir_resize_uint8_linear(origData.data(), origW, origH, 0, reduced.data(), needW, needH, 0, STBIR_RGBA);
            origData.swap(reduced);
            srcW = needW; srcH = needH;
            std::vector<unsigned char>().swap(processedData);
            tweaksChanged = true;
        }
    }

#ifdef __EMSCRIPTEN__

    //////// memory check

    // every big allocation runs on this thread, running out kills the supervisor -- only here can it be caught
    // only the terms that grow with the mural, against the whole ceiling: same-size buffers get reused
    {

        // any size, so the size suggested below is always one this check accepts
        auto estimateBytesFor = [&](int w, int h) -> long long {
            if (w < 1 || h < 1) return 0;
            const int nx = legacyUpscale ? 16 : std::clamp((int)std::lround((double)origW / w), 1, 16);
            const int ny = legacyUpscale ? 16 : std::clamp((int)std::lround((double)origH / h), 1, 16);
            const long long tPx = (long long)(w * 16) * (h * 16);
            const long long lPx = (long long)(w * nx) * (h * ny);
            const long long lab = (long long)sizeof(ColorLabPacked);

            long long pixelBytes = lPx * 4    // resizedData (rgba)
                + lPx * lab    // smallLabData
                + tPx * lab;    // cachedLabData

            // compositeLab: mural-sized in the old mode or with one tile for the whole picture, else two tiles per thread
            if (p2Enabled && (g_legacyFullComposite.load() != 0 || pass2WholeImage(p2Chunk))) pixelBytes += tPx * lab;
            else if (p2Enabled) {
                const long long tilePx = (long long)pass2ChunkBlocks(p2Chunk, w, h) * 16;
                pixelBytes += tilePx * tilePx * lab * 2 * getOptimalThreadCount();
            }

            // tested: a growing vector holds old + new, plus fragmentation -- a 650 run peaked ~15% over its parts
            long long need = (long long)(pixelBytes * 1.5);
            need += (long long)w * h * 52;    // per-cell grids, scores, candidates
            need += (long long)w * h * 8 * (p2Cands - 3);    // a longer candidate list, held twice

            // experimental stage: a few arrays per cell for feathering, the photo again at full size for the search
            if (enhOn) {
                const EnhanceSettings e = enhanceEffective(enhSet, w * h);
                if (e.search) need += (long long)w * h * 1900;
                else if (e.feather) need += (long long)w * h * 130;
            }

            // ~9.8 KB per palette entry, two palettes alive while one is built, ~60 variants per block with overlays
            // no mural size fixes a huge allow list, hence the other two suggestions
            const long long entries = (long long)localAllowedBlocks.size()
                * ((localUseOverlays && !localAllowedOverlays.empty()) ? 60 : 1);
            need += entries * 9800 * 2;
            return need;
            };

        const long long needed = estimateBytesFor(tW, tH);
        const long long ceiling = (long long)(emscripten_get_heap_max() * 0.90);
        if (needed > ceiling) {

            // biggest uniform shrink that fits, by bisection -- Neff depends on the size, the estimate still only falls
            int bestW = 0, bestH = 0;
            int lo = 1, hi = 1000;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                const int cw = std::max(1, (int)((long long)tW * mid / 1000));
                const int ch = std::max(1, (int)((long long)tH * mid / 1000));
                if (estimateBytesFor(cw, ch) <= ceiling) { bestW = cw; bestH = ch; lo = mid + 1; }
                else hi = mid - 1;
            }

            char msg[320];
            if (bestW > 0 && (bestW < tW || bestH < tH)) {
                snprintf(msg, sizeof(msg),
                    "This mural is too large to fit in your browser's memory - try %dx%d or smaller. "
                    "You can also allow fewer blocks, or turn overlays off.", bestW, bestH);
            }
            else {

                // even the smallest mural doesnt fit: the allow list is the problem
                snprintf(msg, sizeof(msg),
                    "This mural is too large to fit in your browser's memory. "
                    "Try allowing fewer blocks, or turning overlays off.");
            }
            DBG_PRINT("[ENGINE] PRE-FLIGHT REFUSED: need ~%.0f MB, ceiling %.0f MB (%dx%d, Neff=%d,%d) -> suggest %dx%d\n",
                needed / 1048576.0, ceiling / 1048576.0, tW, tH, Neff_x, Neff_y, bestW, bestH);
            abandonPreview(msg);
            return false;
        }
    }
#endif

    {

        // size changed: all four big buffers go at once, so the new set fits the freed space, the heap doesnt grow
        // tested: old + new together peaked at 2x (1,554 to 3,544 MB on 550 to 600), one at a time was worse still
        // same size = buffers untouched, a recalculation allocates nothing
        static size_t lastTargetPx = 0, lastLabPx = 0;
        const size_t curTargetPx = (size_t)targetW_px * targetH_px;
        const size_t curLabPx = (size_t)labW_px * labH_px;
        if (curTargetPx != lastTargetPx || curLabPx != lastLabPx) {
            DBG_PRINT("[ENGINE] 0/5: size changed - releasing all big buffers before reallocation.\n");
            std::vector<unsigned char>().swap(resizedData);
            std::vector<ColorLabPacked>().swap(smallLabData);
            std::vector<ColorLabPacked>().swap(cachedLabData);
            std::vector<ColorLabPacked>().swap(compositeLab);
            lastTargetPx = curTargetPx;
            lastLabPx = curLabPx;
            DBG_HEAP("0/5 after coordinated release");
        }
    }

    // picture tweaks
    if (tweaksChanged || processedData.empty() || processedData.size() != origData.size()) {
        auto t_tw = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 1/5: Image Pre-processing started...\n");

        processedData = applyImageTweaks(origData, srcW, srcH);
        tweaksChanged = false;
        cachedTW = 0;

        auto t_tw_end = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 1/5: Image Pre-processing finished in %.2f ms.\n", std::chrono::duration<double, std::milli>(t_tw_end - t_tw).count());
        DBG_HEAP("1/5 after Image Pre-processing");
    }
    else {
        DBG_PRINT("[ENGINE] 1/5: Image Pre-processing skipped (no changes).\n");
    }
    return true;
}


//////////////// PLAN
bool PreviewRun::planWork() {
    const char* checkBiomes[] = { "Plains", "Forest", "Jungle", "Swamp", "Badlands", "Desert/Savanna", "Taiga", "Dark Forest" };
    biomeStr = checkBiomes[bIdx];
    activeBiomeStr = biomeStr;

    oriStr = (oIdx == 0) ? "vertical" : "horizontal";
    drctStr = "north";
    if (oIdx == 0) { const char* v[] = { "north", "south", "east", "west" }; drctStr = v[dIdx]; }
    else { const char* h[] = { "top", "bottom" }; drctStr = h[dIdx]; }

    // pass 1 weight follows the palette size and the candidate count
    wtPass1 = localAllowedBlocks.size() / 4;
    if (wtPass1 < 2) wtPass1 = 2;
    wtPass1 = (int)std::lround(wtPass1 * (1.0 + WT_P1_PER_EXTRA_CANDIDATE * (p2Cands - 3)));

    // for mural_progress_diag_json()
    phaseClock = std::chrono::high_resolution_clock::now();
    phaseVal = 0;

    const bool previousRunLost = !lastRunCommitted;
    lastRunCommitted = false;

    long long estimatedWork = 0;
    needsLab = (tW != cachedTW || tH != cachedTH || cachedLabData.empty() || currentBgMode != cachedBgMode || activeBiomeStr != cachedBiome
        || previousRunLost);

    // tested: the resize before the per-row lab ticks is ~37% of the phase, one lump tick after it
    labResizeTickWeight = (int)(labH_px * wtLab * 0.37f);
    labRowTickWeight = std::max(1, (labH_px * wtLab - labResizeTickWeight) / std::max(1, labH_px));
    needsLabExpand = (Neff_x < 16 || Neff_y < 16);
    labExpandTickWeight = std::max(1, wtLab / 50);    // a copy, no colour maths
    if (needsLab) {
        estimatedWork += labH_px * wtLab;
        if (needsLabExpand) estimatedWork += targetH_px * labExpandTickWeight;
    }

    bool envChanged = (oriStr != cachedOri || drctStr != cachedDrct || biomeStr != cachedBiome || currentBgMode != cachedBgMode);
    bool effectiveOverlaysNow = localUseOverlays && !localAllowedOverlays.empty();
    bool effectiveOverlaysCached = cachedUseOverlays && !cachedAllowedOverlays.empty();
    bool overlaysChanged = (effectiveOverlaysNow != effectiveOverlaysCached) || (effectiveOverlaysNow && (localAllowedOverlays != cachedAllowedOverlays));
    bool blocksChanged = (localAllowedBlocks != cachedAllowedBlocks);

    // a changed allow list rebuilds all too: a palette grown click by click has another order
    // and ties go to the lower place (tested: 1-2% of cells)
    fullPaletteRebuild = envChanged || overlaysChanged || cachedPalette->empty() || blocksChanged;

    if (fullPaletteRebuild) {
        estimatedWork += localAllowedBlocks.size() * wtPal;
        if (effectiveOverlaysNow) estimatedWork += localAllowedBlocks.size() * wtPal;
    }

    // known here, before the bar is sized: a recalculation that changes nothing for pass 1 + 2 costs them nothing
    // a resize or palette mismatch can still force a full pass later, the estimate stays as it is
    bool weightsChanged = (wC != lastWC || wS != lastWS || wT != lastWT);
    lastWC = wC; lastWS = wS; lastWT = wT;

    bool p2SettingsChanged = (p2Enabled != lastP2Enabled || p2Chunk != lastP2Chunk || p2Passes != lastP2Passes ||
        p2Thresh != lastP2Thresh || p2MSE != lastP2MSE || p2SSIM != lastP2SSIM);
    lastP2Passes = p2Passes;
    lastP2Enabled = p2Enabled; lastP2Chunk = p2Chunk; lastP2Thresh = p2Thresh;
    lastP2MSE = p2MSE; lastP2SSIM = p2SSIM;

    ditherWholeCell = g_ditherWholeCell.load();
    bool ditherChanged = (dMeth != lastDMeth || dInt != lastDInt || ditherWholeCell != lastDitherWholeCell);
    lastDMeth = dMeth; lastDInt = dInt; lastDitherWholeCell = ditherWholeCell;

    // pass 2, dither or weights moved = solve the grid from a clean slate
    fullGridRecalc = fullPaletteRebuild || needsLab || weightsChanged || p2SettingsChanged || ditherChanged
        || p2Cands != g_committedCands || previousRunLost;

    // kept apart, so the rescale after pass 1 knows what it corrects
    pass1BudgetUnits = 0;
    if (fullGridRecalc) {
        pass1BudgetUnits = (long long)tW * tH * wtPass1;
    }
    estimatedWork += pass1BudgetUnits;

    // refinement budget in one place: estimate, correction after pass 1 + ticks use the same numbers
    const int p2Tile = pass2ChunkBlocks(p2Chunk, tW, tH);
    p2TilesX = (tW + p2Tile - 1) / p2Tile, p2TilesY = (tH + p2Tile - 1) / p2Tile;
    p2TilesToRefine = (int)((long long)p2TilesX * p2TilesY * p2Thresh);
    if (p2TilesToRefine < 1 && p2Thresh > 0.0f) p2TilesToRefine = 1;

    // until the tiles are ranked: every block of a refined tile has its other candidates tried
    p2CellsGuess = std::min((long long)tW * tH, (long long)p2TilesToRefine * p2Tile * p2Tile);
    p2ScansGuess = p2CellsGuess * (p2Cands - 1);
    p2w = pass2WeightsAt(g_laterScale);
    wEnh = std::max(1, (int)std::lround(wtEnhance * g_laterScale));

    pass2BudgetUnits = 0;
    if (p2Enabled && fullGridRecalc) {
        pass2BudgetUnits = pass2Units(p2w, p2ScansGuess, p2CellsGuess);
        estimatedWork += pass2BudgetUnits;
    }

    // feathering steps back as dithering gets stronger, dither looks like edges to it
    if (ditherWholeCell != 0 && dMeth != 0) enhSet.featherKeep = std::clamp(1.0f - dInt, 0.0f, 1.0f);

    // what the stage really does at this size (the search has a size limit)
    enhEff = enhanceEffective(enhSet, tW * tH);
    enhRuns = enhOn && (enhEff.search || enhEff.feather);
    enhanceBudgetUnits = 0;
    if (enhRuns) {
        enhanceBudgetUnits = (long long)enhanceTicks(tW * tH, enhEff) * wEnh;
        estimatedWork += enhanceBudgetUnits;
    }

    g_progressMax = estimatedWork > 0 ? estimatedWork : 1;
    g_progressVal = 0;
    return true;
}


//////////////// LAB
bool PreviewRun::convertLab() {
    if (needsLab) {
        auto t_lab = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 2/5: Resizing & LAB Conversion started...\n");

        resizeReclaiming(resizedData, (size_t)labW_px * labH_px * 4);    // stbir_resize rewrites all of it

        // a reduced copy has this size already, the same resize made it
        if (srcW == labW_px && srcH == labH_px && (srcW != origW || srcH != origH)) memcpy(resizedData.data(), processedData.data(), (size_t)labW_px * labH_px * 4);
        else stbir_resize_uint8_linear(processedData.data(), srcW, srcH, 0, resizedData.data(), labW_px, labH_px, 0, STBIR_RGBA);
        g_progressVal += labResizeTickWeight;
        DBG_PRINT("[ENGINE] 2/5-resize: stbir_resize finished at +%.2f ms.\n", std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_lab).count());

        resizeReclaiming(smallLabData, (size_t)labW_px * labH_px);    // the lab conversion rewrites all of it

        int totalPixels = labW_px * labH_px;
        int numThreads = getOptimalThreadCount();

        std::vector<int> currentBgRgb = getPreviewBgColor(currentBgMode, activeBiomeStr);
        std::mutex syncMutex;
        std::condition_variable syncCv;
        int labTasksCompleted = 0;

        // threads take a share each, this thread the remainder
        for (int t = 0; t < numThreads - 1; ++t) {
            getPool()->enqueue([&, t, numThreads, totalPixels, myCalcId = myCalcId]() {
                int startIdx = t * (totalPixels / numThreads);
                int endIdx = (t + 1) * (totalPixels / numThreads);
                for (int i = startIdx; i < endIdx; ++i) {
                    if (currentCalcId.load() != myCalcId) break;
                    float r = resizedData[i * 4] / 255.0f;
                    float g = resizedData[i * 4 + 1] / 255.0f;
                    float b = resizedData[i * 4 + 2] / 255.0f;
                    float a = resizedData[i * 4 + 3] / 255.0f;

                    if (currentBgMode != 3) {
                        r = (r * a) + ((currentBgRgb[0] / 255.0f) * (1.0f - a));
                        g = (g * a) + ((currentBgRgb[1] / 255.0f) * (1.0f - a));
                        b = (b * a) + ((currentBgRgb[2] / 255.0f) * (1.0f - a));
                        a = 1.0f;
                    }

                    ColorLab lab;
                    convertPixelToLAB(r, g, b, lab.l, lab.a, lab.b);
                    lab.alpha = a * 100.0f;
                    smallLabData[i] = packLab(lab);
                    if (i % labW_px == 0) g_progressVal += labRowTickWeight;
                }

                {
                    std::lock_guard<std::mutex> lock(syncMutex);
                    labTasksCompleted++;
                    syncCv.notify_one();
                }
                });
        }

        int last_t = numThreads - 1;
        int startIdx = last_t * (totalPixels / numThreads);
        int endIdx = totalPixels;
        for (int i = startIdx; i < endIdx; ++i) {
            if (currentCalcId.load() != myCalcId) break;
            float r = resizedData[i * 4] / 255.0f, g = resizedData[i * 4 + 1] / 255.0f, b = resizedData[i * 4 + 2] / 255.0f;
            float a = resizedData[i * 4 + 3] / 255.0f;

            if (currentBgMode != 3) {
                r = (r * a) + ((currentBgRgb[0] / 255.0f) * (1.0f - a));
                g = (g * a) + ((currentBgRgb[1] / 255.0f) * (1.0f - a));
                b = (b * a) + ((currentBgRgb[2] / 255.0f) * (1.0f - a));
                a = 1.0f;
            }

            ColorLab lab;
            convertPixelToLAB(r, g, b, lab.l, lab.a, lab.b);
            lab.alpha = a * 100.0f;
            smallLabData[i] = packLab(lab);
            if (i % labW_px == 0) g_progressVal += labRowTickWeight;
        }

        {
            std::unique_lock<std::mutex> lock(syncMutex);
            syncCv.wait(lock, [&]() { return labTasksCompleted == numThreads - 1; });
            if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
        }

        if (!needsLabExpand) {

            // 16 samples per block: the small buffer is the full one already
            cachedLabData.swap(smallLabData);
        }
        else {

            // each real sample copied over its share of the block -- interpolating would invent detail across blocks
            resizeReclaiming(cachedLabData, (size_t)targetW_px * targetH_px);
            DBG_PRINT("[ENGINE] 2/5-expand: replicating %dx%d LAB samples up to %dx%d (Neff=%d,%d)...\n", labW_px, labH_px, targetW_px, targetH_px, Neff_x, Neff_y);

            std::mutex expMutex;
            std::condition_variable expCv;
            int expTasksCompleted = 0;
            int numExpandThreads = getOptimalThreadCount();
            int rowsPerThread = (targetH_px + numExpandThreads - 1) / std::max(1, numExpandThreads);
            int expandJobs = 0;
            for (int t = 0; t < numExpandThreads; ++t) {
                int rs = t * rowsPerThread, re = std::min(targetH_px, rs + rowsPerThread);
                if (rs >= re) continue;
                expandJobs++;
                getPool()->enqueue([&, rs, re, myCalcId = myCalcId]() {
                    for (int py = rs; py < re; ++py) {
                        if (currentCalcId.load() != myCalcId) break;
                        int blockRow = py / 16, r = py % 16;
                        int sy = blockRow * Neff_y + (r * Neff_y / 16);
                        const ColorLabPacked* srcRow = &smallLabData[(size_t)sy * labW_px];
                        ColorLabPacked* dstRow = &cachedLabData[(size_t)py * targetW_px];
                        for (int px = 0; px < targetW_px; ++px) {
                            int blockCol = px / 16, c = px % 16;
                            int sx = blockCol * Neff_x + (c * Neff_x / 16);
                            dstRow[px] = srcRow[sx];
                        }
                        if (py % 4 == 0) g_progressVal += labExpandTickWeight * 4;
                    }
                    {
                        std::lock_guard<std::mutex> lock(expMutex);
                        expTasksCompleted++;
                        expCv.notify_one();
                    }
                    });
            }
            {
                std::unique_lock<std::mutex> lock(expMutex);
                expCv.wait(lock, [&]() { return expTasksCompleted == expandJobs; });
            }
            if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight during LAB expand (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
        }

        cachedTW = tW;
        cachedTH = tH;

        auto t_lab_end = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 2/5: Resizing & LAB Conversion finished in %.2f ms.\n", std::chrono::duration<double, std::milli>(t_lab_end - t_lab).count());
        DBG_HEAP("2/5 after LAB Conversion");
    }
    else {
        DBG_PRINT("[ENGINE] 2/5: Resizing & LAB Conversion skipped (no changes).\n");
    }
    return true;
}


//////////////// STATUS LINES
bool PreviewRun::publishStatus() {
    const char* ditherItems[] = { "None", "Floyd-Steinberg", "Atkinson", "Bayer 4x4", "Bayer 8x8", "Random Noise" };

    char line1[256], line2[256], line3[256], line4[512], expLine[256];
    float totalW = wC + wS + wT;
    if (totalW <= 0.0f) totalW = 1.0f;
    float pC = (wC / totalW) * 100.0f;
    float pS = (wS / totalW) * 100.0f;
    float pT = (wT / totalW) * 100.0f;

    snprintf(line1, sizeof(line1), "Matrix: %dx%d | %s facing %s | Biome: %s | Overlays: %s",
        tW, tH, oriStr.c_str(), drctStr.c_str(), biomeStr.c_str(), localUseOverlays ? "ON" : "OFF");

    snprintf(line2, sizeof(line2),
        "Pre-Process -> Denoise: %.1f | Sharpen: %.1f | Contrast: %.2f | Bright: %+.2f | Sat: %.2f | Hue: %+.0f",
        tweakDenoise, tweakSharpen, tweakContrast, tweakBrightness, tweakSaturation, tweakHue);

    snprintf(line3, sizeof(line3), "Weights -> Color: %.1f%% | Spatial: %.1f%% | Texture: %.1f%% | Dither: %s, %.2f",
        pC, pS, pT, ditherItems[dMeth], dInt);

    if (enhOn && enhSet.search) {
        snprintf(expLine, sizeof(expLine), "Grain: %.2f | Colour over texture: %.2f | Protect texture: %.2f | Feathering: %s",
            enhSet.grain, enhSet.flat, enhSet.protect, enhSet.feather ? "ON" : "OFF");
    }
    else {
        snprintf(expLine, sizeof(expLine), "Feathering: %s", (enhOn && enhSet.feather) ? "ON" : "OFF");
    }

    if (p2Enabled) {
        char tileLabel[24];
        if (pass2WholeImage(p2Chunk)) snprintf(tileLabel, sizeof(tileLabel), "Whole image");
        else snprintf(tileLabel, sizeof(tileLabel), "%dx%d", pass2ChunkBlocks(p2Chunk, tW, tH), pass2ChunkBlocks(p2Chunk, tW, tH));

        // mse + ssim on the 0-1 scale the sliders show; one tile has no worst share, so no threshold
        char targetLabel[32] = "";
        if (!pass2WholeImage(p2Chunk)) snprintf(targetLabel, sizeof(targetLabel), " | Target: Worst %.0f%%", p2Thresh * 100.0f);
        snprintf(line4, sizeof(line4), "Pass 2: ON | Chunk: %s | Passes: %d | Candidates: %d%s | MSE: %.2f | SSIM: %.2f | %s",
            tileLabel, p2Passes, p2Cands, targetLabel, p2MSE * 0.1f, p2SSIM * 0.1f, expLine);
    }
    else {
        snprintf(line4, sizeof(line4), "Pass 2: OFF | %s", expLine);
    }

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        calcStatus[0] = line1; calcStatus[1] = line2;
        calcStatus[2] = line3; calcStatus[3] = line4;
        hasCalcStatus = true;
    }
    return true;
}


//////////////// GRID
bool PreviewRun::prepareGrid() {
    errW = tW + 3;

    if (lastGridW != tW || lastGridH != tH || tempGridIndices.empty() || fullGridRecalc || previewGridIndices.empty()
        || candStride != p2Cands) {
        tempGridIndices.assign(tW * tH, 0);
        tempRailWishes.assign((size_t)tW * tH, 0);
        bestScores.assign(tW * tH, 999999.0f);
        cellCandidates.assign((size_t)tW * tH * p2Cands, -1);
        previewCellCandidates.assign((size_t)tW * tH * p2Cands, -1);
        candStride = p2Cands;
        errorGrid.assign((tH + 2) * errW, { 0,0,0 });
        ditherShift.assign((size_t)tW * tH * 3, 0);
        lastGridW = tW; lastGridH = tH;
        fullGridRecalc = true;
    }
    else {
        std::lock_guard<std::mutex> lock(dataMutex);

        // the engine grid, not the enhanced one on screen
        tempGridIndices = (engineGridIndices.size() == previewGridIndices.size()) ? engineGridIndices : previewGridIndices;
        tempRailWishes = committedRailWishes;
        if (tempRailWishes.size() != (size_t)tW * tH) tempRailWishes.assign((size_t)tW * tH, 0);
        cellCandidates = previewCellCandidates;    // of the committed run, not leftovers of a cancelled one
    }

    // the cached palette as it is, or a new one of its own -- nothing shared is written before the run commits
    paletteRef = cachedPalette;

    if (fullPaletteRebuild) {
        auto t_pal = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 3/5: FULL Palette Generation started...\n");
        paletteRef = std::make_shared<const std::vector<ActiveBlock>>(
            buildActivePalette(oriStr, drctStr, biomeStr, localAllowedBlocks, currentBgMode, localUseOverlays, localAllowedOverlays, false));
        fullGridRecalc = true;
        auto t_pal_end = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 3/5: FULL Palette Generation finished in %.2f ms. (palette size = %zu)\n", std::chrono::duration<double, std::milli>(t_pal_end - t_pal).count(), paletteRef->size());
    }

    const std::vector<ActiveBlock>& tempPalette = *paletteRef;

    // only air: no allowed block has a face for this placement
    if (tempPalette.size() <= 1) {
        abandonPreview("None of the allowed blocks can be used in this orientation. Try another block, or face the mural a different way.");
        return false;
    }
    return true;
}


//////////////// PASS 1
bool PreviewRun::firstPass() {
    const std::vector<ActiveBlock>& tempPalette = *paletteRef;
    const std::vector<ColorLabPacked>& labData = cachedLabData;
    float tWeight = wC + wS + wT;
    if (tWeight <= 0) tWeight = 1.0f;
    wC /= tWeight; wS /= tWeight; wT /= tWeight;

    // fixed for the run, so out of the loops (the per-texel test blocked vectorising)
    const bool transparentMode = (currentBgMode == 3);
    const float alphaWeight = transparentMode ? 4.0f : 1.5f;

    // atomics read once per run, not per candidate
    const float cullMargin = g_cullMargin.load();
    const bool cullAudit = (g_cullAudit.load() != 0);
    const bool earlyExit = (g_earlyExit.load() != 0);

    // both bounds need an unweighted sum of squares: transparent mode weights texels by alpha, so off there
    const bool meanFloor = (g_meanFloor.load() != 0) && earlyExit && !transparentMode;
    const bool coarseBound = (g_coarseBound.load() != 0) && earlyExit && !transparentMode;
    const bool bestFirst = (g_bestFirst.load() != 0) && earlyExit;
    const bool packedSweep = (g_packedSweep.load() != 0);
    const float overlayBias = g_overlayBias.load();

    const float bayer4x4[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    const float bayer8x8[8][8] = { {0,32,8,40,2,34,10,42}, {48,16,56,24,50,18,58,26}, {12,44,4,36,14,46,6,38}, {60,28,52,20,62,30,54,22},
                                   {3,35,11,43,1,33,9,41}, {51,19,59,27,49,17,57,25}, {15,47,7,39,13,45,5,37}, {63,31,55,23,61,29,53,21} };

    //////// candidate set

    // one scored entry per block + overlay, a variant only when its pixels differ -- same rule as buildActivePalette
    std::vector<int> scoringIndices;
    scoringIndices.reserve(tempPalette.size());
    {
        std::unordered_map<std::string, size_t> familyRep;    // key to the first entry seen
        for (size_t i = 0; i < tempPalette.size(); ++i) {
            const ActiveBlock& ab = tempPalette[i];
            if (ab.ovMask < 0) {
                scoringIndices.push_back((int)i);
                continue;
            }

            // never score an entry without scoring data: zero mean + out of bounds in tDist
            if (ab.sortedLab.size() < 256) continue;

            std::string key = ab.mc_id + "\x1f" + ab.ovId;
            auto it = familyRep.find(key);
            if (it == familyRep.end()) {
                familyRep.emplace(key, i);
                scoringIndices.push_back((int)i);
                continue;
            }

            bool renderIsIdentical = tempPalette[it->second].rgbArray == ab.rgbArray;
            if (renderIsIdentical) continue;

            scoringIndices.push_back((int)i);
        }
    }
    DBG_PRINT("[ENGINE] Pass1 scoring dedup: %zu palette entries -> %zu scored candidates.\n", tempPalette.size(), scoringIndices.size());

    //////// colour sweep arrays

    // every candidate gets a cDist in every cell, so this sweep never shrinks: means in compact arrays (~96 KB)
    const size_t nScored = scoringIndices.size();

    // tested: 0.97-0.99x on ~730-candidate plain palettes, 1.04-1.14x on ~5.9k overlay ones -- 1500 splits them
    const bool useCompactSweep = packedSweep && nScored >= 1500;
    std::vector<float> scMeanL, scMeanA, scMeanB, scMeanAlpha;
    std::vector<uint8_t> scOverlay(nScored);
    if (useCompactSweep) { scMeanL.resize(nScored); scMeanA.resize(nScored); scMeanB.resize(nScored); scMeanAlpha.resize(nScored); }

    // the overlay flag always: a byte per candidate keeps the penalty off the ActiveBlock stride
    for (size_t i = 0; i < nScored; ++i) {
        const ActiveBlock& ab = tempPalette[scoringIndices[i]];
        scOverlay[i] = (ab.ovMask >= 0) ? 1 : 0;
        if (!useCompactSweep) continue;
        {
            scMeanL[i] = ab.meanLab.l; scMeanA[i] = ab.meanLab.a;
            scMeanB[i] = ab.meanLab.b; scMeanAlpha[i] = ab.meanLab.alpha;
        }
    }

    //////// region fast path

    // a source coarser than 16 px per block repeats each real sample over a region of the cell
    // sums per region turn 256 texel terms into Neff_x*Neff_y; too many regions lose to the streaming loop
    const int kMaxFastRegions = g_maxFastRegions.load();
    int fastRegionCount = Neff_x * Neff_y;
    bool useRegionFastPath = (Neff_x < 16 || Neff_y < 16)
        && (fastRegionCount <= kMaxFastRegions)
        && (g_legacyTextureLoop.load() == 0);
    std::vector<int> regionRowBoundary, regionColBoundary;

    // per run and per scored slot: on ActiveBlock the tables were ~97 MB, copied with every palette
    std::vector<int> fastSlot;    // palette index to compact slot, -1 = not scored
    std::vector<ColorLabAccum> blockRegionSums;    // stride fastRegionCount (sDist)
    std::vector<ColorLabAccum> blockSortedPrefix;    // stride 257 (tDist)

    if (useRegionFastPath) {
        regionRowBoundary.assign(Neff_y + 1, 0);
        { int k = 0; for (int r = 0; r < 16; ++r) { int region = r * Neff_y / 16; if (region != k) { regionRowBoundary[region] = r; k = region; } } regionRowBoundary[Neff_y] = 16; }
        regionColBoundary.assign(Neff_x + 1, 0);
        { int k = 0; for (int c = 0; c < 16; ++c) { int region = c * Neff_x / 16; if (region != k) { regionColBoundary[region] = c; k = region; } } regionColBoundary[Neff_x] = 16; }

        fastSlot.assign(tempPalette.size(), -1);
        int slotCount = 0;
        for (int b : scoringIndices) fastSlot[b] = slotCount++;

        if (wS > 0.001f) {
            blockRegionSums.assign((size_t)slotCount * fastRegionCount, ColorLabAccum{});
            for (int b : scoringIndices) {
                ColorLabAccum* sums = blockRegionSums.data() + (size_t)fastSlot[b] * fastRegionCount;
                const std::vector<ColorLab>& fl = tempPalette[b].flatLab;
                for (int ry = 0; ry < Neff_y; ++ry) {
                    for (int rx = 0; rx < Neff_x; ++rx) {
                        ColorLabAccum& acc = sums[ry * Neff_x + rx];
                        for (int r = regionRowBoundary[ry]; r < regionRowBoundary[ry + 1]; ++r) {
                            for (int c = regionColBoundary[rx]; c < regionColBoundary[rx + 1]; ++c) {
                                const ColorLab& v = fl[r * 16 + c];
                                acc.l += v.l; acc.l2 += v.l * v.l;
                                acc.a += v.a; acc.a2 += v.a * v.a;
                                acc.b += v.b; acc.b2 += v.b * v.b;
                                acc.alpha += v.alpha; acc.alpha2 += v.alpha * v.alpha;
                            }
                        }
                    }
                }
            }
        }
        if (wT > 0.001f) {
            blockSortedPrefix.assign((size_t)slotCount * 257, ColorLabAccum{});
            for (int b : scoringIndices) {
                const std::vector<ColorLab>& sl = tempPalette[b].sortedLab;
                if (sl.size() < 256) { fastSlot[b] = -1; continue; }    // no sorted lab: the 256-term loop takes it
                ColorLabAccum* pre = blockSortedPrefix.data() + (size_t)fastSlot[b] * 257;
                for (int i = 0; i < 256; ++i) {
                    const ColorLab& c = sl[i];
                    const ColorLabAccum& prev = pre[i];
                    ColorLabAccum& cur = pre[i + 1];
                    cur.l = prev.l + c.l;             cur.l2 = prev.l2 + (double)c.l * c.l;
                    cur.a = prev.a + c.a;             cur.a2 = prev.a2 + (double)c.a * c.a;
                    cur.b = prev.b + c.b;             cur.b2 = prev.b2 + (double)c.b * c.b;
                    cur.alpha = prev.alpha + c.alpha; cur.alpha2 = prev.alpha2 + (double)c.alpha * c.alpha;
                }
            }
        }
    }

    //////// coarse bound

    // the mean floor over four quadrants is tighter for 4x the sums -- only where the 256-term loops run
    // sorted arrays split by index into four runs: tDist compares position by position, any shared split bounds it
    const bool useCoarse = coarseBound && !useRegionFastPath && !transparentMode
        && (wS > 0.001f || wT > 0.001f);
    constexpr int kCoarse = 4;    // 2x2 quadrants / 4 quartiles
    constexpr float kCoarseInv = 1.0f / 64.0f;    // 1/n per region, n = 256/kCoarse
    std::vector<int> coarseSlot;
    std::vector<float> blockCoarseFlat;    // stride kCoarse*4: l, a, b, alpha per region
    std::vector<float> blockCoarseSorted;
    if (useCoarse) {
        coarseSlot.assign(tempPalette.size(), -1);
        int n = 0;
        for (int b : scoringIndices) coarseSlot[b] = n++;
        if (wS > 0.001f) blockCoarseFlat.assign((size_t)n * kCoarse * 4, 0.0f);
        if (wT > 0.001f) blockCoarseSorted.assign((size_t)n * kCoarse * 4, 0.0f);
        for (int b : scoringIndices) {
            const int slot = coarseSlot[b];
            if (wS > 0.001f) {
                const std::vector<ColorLab>& fl = tempPalette[b].flatLab;
                float* dst = blockCoarseFlat.data() + (size_t)slot * kCoarse * 4;
                for (int i = 0; i < 256; ++i) {
                    const int r = ((i >> 7) << 1) | ((i >> 3) & 1);    // row/8*2 + col/8
                    float* a = dst + r * 4;
                    a[0] += fl[i].l; a[1] += fl[i].a; a[2] += fl[i].b; a[3] += fl[i].alpha;
                }
            }
            if (wT > 0.001f) {
                const std::vector<ColorLab>& sl = tempPalette[b].sortedLab;
                if (sl.size() < 256) { coarseSlot[b] = -1; continue; }
                float* dst = blockCoarseSorted.data() + (size_t)slot * kCoarse * 4;
                for (int i = 0; i < 256; ++i) {
                    float* a = dst + (i >> 6) * 4;
                    a[0] += sl[i].l; a[1] += sl[i].a; a[2] += sl[i].b; a[3] += sl[i].alpha;
                }
            }
        }
    }

    //////// one cell
    auto processPixel = [&](int row, int col, bool isPartial, size_t evalStart, size_t evalEnd) {
        ColorLab ditherOffset = { 0, 0, 0 };

        // ordered + noise dither are lightness patterns: pushing a and b too tinted the whole mural one way
        const float abShare = ditherWholeCell != 0 ? 0.0f : 1.0f;
        if (!isPartial || (ditherWholeCell != 0 && dMeth >= 3)) {
            if (dMeth == 1 || dMeth == 2) {
                ditherOffset = { errorGrid[row * errW + (col + 1)].l * dInt,
                                 errorGrid[row * errW + (col + 1)].a * dInt,
                                 errorGrid[row * errW + (col + 1)].b * dInt };
            }
            else if (dMeth == 3) {
                float bVal = (bayer4x4[row % 4][col % 4] / 16.0f) - 0.5f;
                ditherOffset = { bVal * 28.0f * dInt, bVal * 28.0f * dInt * abShare, bVal * 28.0f * dInt * abShare };
            }
            else if (dMeth == 4) {
                float bVal = (bayer8x8[row % 8][col % 8] / 64.0f) - 0.5f;
                ditherOffset = { bVal * 28.0f * dInt, bVal * 28.0f * dInt * abShare, bVal * 28.0f * dInt * abShare };
            }
            else if (dMeth == 5) {
                float rVal = ((float)((row * 1337 + col * 9277) % 1000) / 1000.0f) - 0.5f;
                ditherOffset = { rVal * 22.0f * dInt, rVal * 22.0f * dInt * abShare, rVal * 22.0f * dInt * abShare };
            }
        }
        const bool shiftCell = ditherWholeCell != 0
            && (ditherOffset.l != 0.0f || ditherOffset.a != 0.0f || ditherOffset.b != 0.0f);
        const ColorLab cellShift = shiftCell ? ditherOffset : ColorLab{ 0, 0, 0 };

        // the offset is part of the cell now: the mean target needs nothing added, the floor stays exact
        if (shiftCell) ditherOffset = { 0, 0, 0 };
        if (!ditherShift.empty()) {
            int32_t* ds = &ditherShift[(size_t)(row * tW + col) * 3];
            ds[0] = (int32_t)std::lround(cellShift.l * LAB_L_SCALE);
            ds[1] = (int32_t)std::lround(cellShift.a * LAB_AB_SCALE);
            ds[2] = (int32_t)std::lround(cellShift.b * LAB_AB_SCALE);
        }

        ColorLab segmentFlat[256];
        float sumL = 0, sumA = 0, sumB = 0, sumAlpha = 0; int idx = 0;

        // cell half of the coarse bound, in the pass that reads every texel anyway
        float cellCoarseFlat[kCoarse * 4] = { 0 };
        for (int r = 0; r < 16; ++r) {
            for (int c = 0; c < 16; ++c) {
                int py = (row * 16) + r, px = (col * 16) + c;
                segmentFlat[idx] = unpackLab(labData[py * targetW_px + px]);
                sumL += segmentFlat[idx].l; sumA += segmentFlat[idx].a; sumB += segmentFlat[idx].b;
                sumAlpha += segmentFlat[idx].alpha;
                if (useCoarse) {
                    float* a = cellCoarseFlat + (((r >> 3) << 1) | (c >> 3)) * 4;
                    a[0] += segmentFlat[idx].l; a[1] += segmentFlat[idx].a;
                    a[2] += segmentFlat[idx].b; a[3] += segmentFlat[idx].alpha;
                }
                idx++;
            }
        }
        if (shiftCell) {

            // every texel moves by the same amount, and so do the sums
            for (int i = 0; i < 256; ++i) { segmentFlat[i].l += cellShift.l; segmentFlat[i].a += cellShift.a; segmentFlat[i].b += cellShift.b; }
            sumL += 256.0f * cellShift.l; sumA += 256.0f * cellShift.a; sumB += 256.0f * cellShift.b;
            if (useCoarse) {
                for (int r = 0; r < kCoarse; ++r) {
                    cellCoarseFlat[r * 4 + 0] += 64.0f * cellShift.l;
                    cellCoarseFlat[r * 4 + 1] += 64.0f * cellShift.a;
                    cellCoarseFlat[r * 4 + 2] += 64.0f * cellShift.b;
                }
            }
        }
        ColorLab segmentMean = { sumL / 256.0f, sumA / 256.0f, sumB / 256.0f, sumAlpha / 256.0f };

        int cellIdx = row * tW + col;

        // transparent mode: an almost see-through cell = air
        if (currentBgMode == 3 && segmentMean.alpha < 5.0f) {
            tempGridIndices[cellIdx] = 0;
            tempRailWishes[cellIdx] = 0;
            return;
        }

        ColorLab segmentSorted[256];
        for (int i = 0; i < 256; ++i) segmentSorted[i] = segmentFlat[i];
        if (wT > 0.001f) {
#if MMMM_VERBOSE
            auto t_sort0 = std::chrono::high_resolution_clock::now();
#endif

            // sorted only when texture counts (tested: the sort showed up in colour-only runs)
            std::sort(std::begin(segmentSorted), std::end(segmentSorted), [](const ColorLab& a, const ColorLab& b) { return a.l < b.l; });
#if MMMM_VERBOSE
            tl_sortTimeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t_sort0).count();
#endif
        }

        // sorted half of the coarse bound, after the sort
        float cellCoarseSorted[kCoarse * 4] = { 0 };
        if (useCoarse && wT > 0.001f) {
            for (int i = 0; i < 256; ++i) {
                float* a = cellCoarseSorted + (i >> 6) * 4;
                a[0] += segmentSorted[i].l; a[1] += segmentSorted[i].a;
                a[2] += segmentSorted[i].b; a[3] += segmentSorted[i].alpha;
            }
        }

        ColorLab meanAdj = { segmentMean.l + ditherOffset.l, segmentMean.a + ditherOffset.a, segmentMean.b + ditherOffset.b };

        // mean floor: by cauchy-schwarz sDist and tDist >= the distance of the two means (sorted = same values)
        // cDist is that norm against meanAdj, off by the dither offset: floor = cDist - |offset|, one sqrt per cell
        const float dOff = (ditherOffset.l == 0.0f && ditherOffset.a == 0.0f && ditherOffset.b == 0.0f)
            ? 0.0f
            : std::sqrt(ditherOffset.l * ditherOffset.l + ditherOffset.a * ditherOffset.a + ditherOffset.b * ditherOffset.b);

        // the floor claims only weights the score uses (over 0.001), or it would prune possible winners
        const float floorWs = (meanFloor && wS > 0.001f) ? wS : 0.0f;
        const float floorWt = (meanFloor && wT > 0.001f) ? wT : 0.0f;
        const float floorW = floorWs + floorWt;

        // fast path per cell: each region as (value, size) -- sum of (x-v)^2 = sum x^2 - 2v sum x + n v^2
        // exact in double: matches a double reference bit for bit, the float 256-term loop is the one off (~4e-6)
        ColorLab cellRegionValue[256];
        int cellRegionSize[256];
        struct SortedRegion { float l, a, b, alpha; int size, lo, hi; };
        SortedRegion sortedCellRegions[256];
        if (useRegionFastPath && (wS > 0.001f || wT > 0.001f)) {
            for (int ry = 0; ry < Neff_y; ++ry) {
                int repR = regionRowBoundary[ry];
                int rSize = regionRowBoundary[ry + 1] - regionRowBoundary[ry];
                for (int rx = 0; rx < Neff_x; ++rx) {
                    int repC = regionColBoundary[rx];
                    int cSize = regionColBoundary[rx + 1] - regionColBoundary[rx];
                    int k = ry * Neff_x + rx;
                    cellRegionValue[k] = segmentFlat[repR * 16 + repC];
                    cellRegionSize[k] = rSize * cSize;
                }
            }
            if (wT > 0.001f) {
                for (int k = 0; k < fastRegionCount; ++k) {
                    const ColorLab& v = cellRegionValue[k];
                    sortedCellRegions[k] = { v.l, v.a, v.b, v.alpha, cellRegionSize[k], 0, 0 };
                }

                // regions by lightness, only Neff_x*Neff_y of them
                std::sort(sortedCellRegions, sortedCellRegions + fastRegionCount,
                    [](const SortedRegion& a, const SortedRegion& b) { return a.l < b.l; });
                int pos = 0;
                for (int k = 0; k < fastRegionCount; ++k) {
                    sortedCellRegions[k].lo = pos;
                    pos += sortedCellRegions[k].size;
                    sortedCellRegions[k].hi = pos;
                }
            }
        }

        // the best p2Cands so far, best first
        struct ScoredCandidate { int index; float score; };
        ScoredCandidate topK[kMaxPass2Candidates];
        for (int k = 0; k < p2Cands; ++k) topK[k] = { -1, 999999.0f };
        int* const cellCands = &cellCandidates[(size_t)cellIdx * p2Cands];

        if (isPartial && tempGridIndices[cellIdx] != -1) {
            topK[0] = { tempGridIndices[cellIdx], bestScores[cellIdx] };
            cellCands[0] = tempGridIndices[cellIdx];
            for (int k = 1; k < p2Cands; ++k) cellCands[k] = -1;    // stale ones out
        }

        // ties go to the lower palette index, so the visiting order cant change the result
        auto beats = [](float sA, int iA, float sB, int iB) {
            if (iB < 0) return true;    // empty slot
            if (sA != sB) return sA < sB;
            return iA < iB;
            };

        // sorted insert, the last one drops off
        auto insertScore = [&](int idx, float score) {
            int pos = 0;
            while (pos < p2Cands && !beats(score, idx, topK[pos].score, topK[pos].index)) ++pos;
            if (pos >= p2Cands) return;
            for (int j = p2Cands - 1; j > pos; --j) topK[j] = topK[j - 1];
            topK[pos] = { idx, score };
            };

        float min_cDist = 99999.0f;
        int auditIdxCulled = -1, auditIdxUncapped = -1;
        float auditBestCulled = 3.4e38f, auditBestUncapped = 3.4e38f;

        // scored entries in [evalStart, evalEnd), ascending, so a binary search
        auto scoreBegin = std::lower_bound(scoringIndices.begin(), scoringIndices.end(), (int)evalStart);
        auto scoreEndIt = std::lower_bound(scoringIndices.begin(), scoringIndices.end(), (int)evalEnd);
        const bool mayPrune = earlyExit && !cullAudit;

        // the position in scoringIndices indexes the compact arrays, the palette index doesnt
        const size_t posBegin = (size_t)(scoreBegin - scoringIndices.begin());

        // one cDist for every sweep, so compact and plain reads cant drift apart
        const float cImp = transparentMode ? (segmentMean.alpha / 100.0f) : 1.0f;
        auto cDistAt = [&](size_t pos) -> float {
            float ml, ma, mb, malpha;
            if (!useCompactSweep) {
                const ColorLab& m = tempPalette[(size_t)scoringIndices[pos]].meanLab;
                ml = m.l; ma = m.a; mb = m.b; malpha = m.alpha;
            }
            else {
                ml = scMeanL[pos]; ma = scMeanA[pos]; mb = scMeanB[pos]; malpha = scMeanAlpha[pos];
            }
            const float dAl = malpha - segmentMean.alpha;
            const float dL = (ml - meanAdj.l) * cImp;
            const float dA = (ma - meanAdj.a) * cImp;
            const float dB = (mb - meanAdj.b) * cImp;
            return std::sqrt((dL * dL) + (dA * dA) + (dB * dB) + (dAl * dAl * alphaWeight));
            };
        auto penaltyAt = [&](size_t pos) -> float { return scOverlay[pos] ? overlayBias : 0.0f; };

        const size_t sweepCount = (size_t)(scoreEndIt - scoreBegin);

        // best first: by lower bound, and once it passes the worst kept score nothing left can get in -- stop
        // exact: the cull is replayed in palette order (running minimum), ties on palette index
        // a counting sort into 32 buckets, O(N); order inside a bucket doesnt matter, each gets its own floor test
        constexpr int kBuckets = 32;
        static thread_local std::vector<int>   tl_survIdx;
        static thread_local std::vector<float> tl_survCDist;
        static thread_local std::vector<float> tl_survFloor;
        static thread_local std::vector<int>   tl_bucketOf;
        static thread_local std::vector<int>   tl_order;
        int bucketStart[kBuckets + 1];
        float bucketSuffixMin[kBuckets + 1];
        size_t nSurv = 0;
        const bool ordered = bestFirst && mayPrune && sweepCount > 0;
        if (ordered) {
            if (tl_survIdx.size() < sweepCount) {
                tl_survIdx.resize(sweepCount); tl_survCDist.resize(sweepCount);
                tl_survFloor.resize(sweepCount); tl_bucketOf.resize(sweepCount);
                tl_order.resize(sweepCount);
            }
            float runMin = 99999.0f, fMin = 3.4e38f, fMax = -3.4e38f;
            for (size_t k = 0; k < sweepCount; ++k) {
                const size_t pos = posBegin + k;
                float cd = cDistAt(pos);
#if MMMM_VERBOSE
                tl_cullTotal++;
#endif
                if (cd < runMin) runMin = cd;
                if (cd > runMin + cullMargin) continue;    // exactly the linear sweep decision
#if MMMM_VERBOSE
                tl_cullSurvivors++;
#endif
                float pen = penaltyAt(pos);
                float f = (wC * cd) + (wC * pen) + floorW * std::max(0.0f, cd - dOff);
                tl_survIdx[nSurv] = (int)pos; tl_survCDist[nSurv] = cd; tl_survFloor[nSurv] = f;
                if (f < fMin) fMin = f;
                if (f > fMax) fMax = f;
                ++nSurv;
            }
            min_cDist = runMin;    // what the linear sweep ends with too

            int counts[kBuckets] = { 0 };
            float bucketMin[kBuckets];
            for (int k = 0; k < kBuckets; ++k) bucketMin[k] = 3.4e38f;
            const float bw = (fMax > fMin) ? ((fMax - fMin) / kBuckets) : 1.0f;
            for (size_t i = 0; i < nSurv; ++i) {
                int bi = (int)((tl_survFloor[i] - fMin) / bw);
                if (bi < 0) bi = 0; else if (bi >= kBuckets) bi = kBuckets - 1;
                tl_bucketOf[i] = bi; counts[bi]++;
                if (tl_survFloor[i] < bucketMin[bi]) bucketMin[bi] = tl_survFloor[i];
            }
            bucketStart[0] = 0;
            for (int k = 0; k < kBuckets; ++k) bucketStart[k + 1] = bucketStart[k] + counts[k];
            int fill[kBuckets];
            for (int k = 0; k < kBuckets; ++k) fill[k] = bucketStart[k];
            for (size_t i = 0; i < nSurv; ++i) tl_order[fill[tl_bucketOf[i]]++] = (int)i;

            // stop on the smallest floor really left, not a computed bucket edge -- rounding could skip a viable one
            bucketSuffixMin[kBuckets] = 3.4e38f;
            for (int k = kBuckets - 1; k >= 0; --k)
                bucketSuffixMin[k] = std::min(bucketMin[k], bucketSuffixMin[k + 1]);
        }

        // seed pass taken out (tested: slower in 8 of 9 cases), seedCount stays 0
        constexpr int kSeedCount = 4;
        int seedIdx[kSeedCount] = { -1, -1, -1, -1 };
        int seedCount = 0;
        static thread_local std::vector<float> tl_cDistCache;
        bool cDistCached = false;

        // one loop over seeds + sweep, so the scoring body exists once
        const size_t totalIter = ordered ? nSurv : ((size_t)seedCount + sweepCount);
        int curBucket = 0;
        for (size_t iter = 0; iter < totalIter; ++iter) {
            size_t b, pos; size_t sweepSlot = 0; bool isSeed = false;
            if (ordered) {
                while (curBucket < kBuckets && iter >= (size_t)bucketStart[curBucket + 1]) ++curBucket;

                // everything unvisited has a floor at least this large: nothing left can get in
                if (bucketSuffixMin[curBucket] >= topK[p2Cands - 1].score) break;
                pos = (size_t)tl_survIdx[tl_order[iter]];
            }
            else {
                isSeed = (iter < (size_t)seedCount);
                sweepSlot = iter - (size_t)seedCount;
                pos = isSeed ? (size_t)seedIdx[iter] : (posBegin + sweepSlot);
            }
            b = (size_t)scoringIndices[pos];
            const ActiveBlock& block = tempPalette[b];
#if MMMM_VERBOSE
            if (!isSeed && !ordered) tl_cullTotal++;
#endif

            // overlays pay a flat amount, so they win only when better on colour -- the shape settles later
            float netPenalty = penaltyAt(pos);

            float cDist;
            if (ordered) {
                cDist = tl_survCDist[tl_order[iter]];
            }
            else if (cDistCached && !isSeed) {
                cDist = tl_cDistCache[sweepSlot];
            }
            else {
                cDist = cDistAt(pos);
            }

            // seeds stay out of the running minimum the cull hangs off; ordered mode did both already
            if (!isSeed && !ordered && cDist < min_cDist) min_cDist = cDist;

            // fast cull: colour far worse than the best seen = no texture maths (this one can lose a winner, see cullAudit)
            const bool culled = !isSeed && !ordered && (cDist > min_cDist + cullMargin);
            if (culled && !cullAudit) continue;

            if (!isSeed && !ordered && seedCount > 0) {
                bool dup = false;
                for (int s = 0; s < seedCount; ++s) { if (seedIdx[s] == (int)pos) { dup = true; break; } }
                if (dup) continue;
            }
#if MMMM_VERBOSE
            if (!isSeed && !ordered) tl_cullSurvivors++;
#endif

            // exact prune: colour + the mean floor already reach the worst kept score = skip both texture loops
            // only candidates that cant get in are skipped, the grid stays bit-identical
            const float scoreBase = (wC * cDist) + (wC * netPenalty);
            const float mFloor = (floorW > 0.0f) ? std::max(0.0f, cDist - dOff) : 0.0f;
            const float floorT = floorWt * mFloor;
            const float scoreFloor = scoreBase + (floorWs * mFloor) + floorT;
            if (mayPrune && scoreFloor >= topK[p2Cands - 1].score) continue;

            float sDist = 0.0f, tDist = 0.0f;
            bool prunedMidLoop = false;

            if (wS > 0.001f) {
                if (useRegionFastPath && fastSlot[b] >= 0) {
                    const ColorLabAccum* regionSums = blockRegionSums.data() + (size_t)fastSlot[b] * fastRegionCount;
                    double sDistAccum = 0.0;
                    for (int k = 0; k < fastRegionCount; ++k) {
                        const ColorLabAccum& rs = regionSums[k];
                        const ColorLab& v = cellRegionValue[k];
                        double n = (double)cellRegionSize[k];
                        double vl = v.l, va = v.a, vb = v.b, valpha = v.alpha;
                        double termL = rs.l2 - 2.0 * vl * rs.l + n * vl * vl;
                        double termA = rs.a2 - 2.0 * va * rs.a + n * va * va;
                        double termB = rs.b2 - 2.0 * vb * rs.b + n * vb * vb;
                        double termAlpha = rs.alpha2 - 2.0 * valpha * rs.alpha + n * valpha * valpha;
                        if (transparentMode) {
                            double colorImp2 = (valpha / 100.0) * (valpha / 100.0);
                            sDistAccum += (termL + termA + termB) * colorImp2 + termAlpha * (double)alphaWeight;
                        }
                        else {
                            sDistAccum += termL + termA + termB + termAlpha * (double)alphaWeight;
                        }
                    }
                    sDist = (float)sDistAccum;
                }
                else {

                    // the sum only grows: past this limit the rest cant help -- checked per 64 texels, kept squared to skip a sqrt
                    // tDist isnt known yet but costs at least floorT, so less is left for sDist
                    const float mS = mayPrune ? ((topK[p2Cands - 1].score - scoreBase - floorT) / wS) : 0.0f;
                    const float sAccLimit = mS * mS * 256.0f;

                    // coarse bound: four region sums bound the sum below, in its own units -- ~16 flops to skip ~2000
                    if (mayPrune && useCoarse && coarseSlot[b] >= 0) {
                        const float* bc = blockCoarseFlat.data() + (size_t)coarseSlot[b] * kCoarse * 4;
                        float lb = 0.0f;
                        for (int r = 0; r < kCoarse; ++r) {
                            const float dl = bc[r * 4] - cellCoarseFlat[r * 4];
                            const float da = bc[r * 4 + 1] - cellCoarseFlat[r * 4 + 1];
                            const float db = bc[r * 4 + 2] - cellCoarseFlat[r * 4 + 2];
                            const float dA = bc[r * 4 + 3] - cellCoarseFlat[r * 4 + 3];
                            lb += (dl * dl) + (da * da) + (db * db) + (dA * dA * alphaWeight);
                        }
                        if (lb * kCoarseInv >= sAccLimit) prunedMidLoop = true;
                    }
                    if (prunedMidLoop) {  }
                    else if (transparentMode) {
                        for (int p0 = 0; p0 < 256; p0 += 64) {
                            sDist += sumTexelBlock64Transparent(&block.flatLab[p0], &segmentFlat[p0], alphaWeight);
                            if (mayPrune && sDist >= sAccLimit) { prunedMidLoop = true; break; }
                        }
                    }
                    else {
                        for (int p0 = 0; p0 < 256; p0 += 64) {
                            sDist += sumTexelBlock64(&block.flatLab[p0], &segmentFlat[p0], alphaWeight);
                            if (mayPrune && sDist >= sAccLimit) { prunedMidLoop = true; break; }
                        }
                    }
                }
                if (prunedMidLoop) continue;
                sDist = std::sqrt(sDist / 256.0f);
            }
            if (wT > 0.001f) {
                if (useRegionFastPath && fastSlot[b] >= 0) {

                    // a cell region in sorted order = [lo, hi) of the block prefix row, one subtraction
                    const ColorLabAccum* prefix = blockSortedPrefix.data() + (size_t)fastSlot[b] * 257;
                    double tDistAccum = 0.0;
                    for (int k = 0; k < fastRegionCount; ++k) {
                        const SortedRegion& reg = sortedCellRegions[k];
                        const ColorLabAccum& hiP = prefix[reg.hi];
                        const ColorLabAccum& loP = prefix[reg.lo];
                        double n = (double)reg.size;
                        double rl = reg.l, ra = reg.a, rb = reg.b, ralpha = reg.alpha;
                        double sumL = hiP.l - loP.l, sumL2 = hiP.l2 - loP.l2;
                        double sumA = hiP.a - loP.a, sumA2 = hiP.a2 - loP.a2;
                        double sumB = hiP.b - loP.b, sumB2 = hiP.b2 - loP.b2;
                        double sumAlpha = hiP.alpha - loP.alpha, sumAlpha2 = hiP.alpha2 - loP.alpha2;
                        double termL = sumL2 - 2.0 * rl * sumL + n * rl * rl;
                        double termA = sumA2 - 2.0 * ra * sumA + n * ra * ra;
                        double termB = sumB2 - 2.0 * rb * sumB + n * rb * rb;
                        double termAlpha = sumAlpha2 - 2.0 * ralpha * sumAlpha + n * ralpha * ralpha;
                        if (transparentMode) {
                            double colorImp2 = (ralpha / 100.0) * (ralpha / 100.0);
                            tDistAccum += (termL + termA + termB) * colorImp2 + termAlpha * (double)alphaWeight;
                        }
                        else {
                            tDistAccum += termL + termA + termB + termAlpha * (double)alphaWeight;
                        }
                    }
                    tDist = (float)tDistAccum;
                }
                else {

                    // the same bail for tDist, with sDist now in the bound
                    const float floorWithS = scoreBase + wS * sDist;
                    const float mT = mayPrune ? ((topK[p2Cands - 1].score - floorWithS) / wT) : 0.0f;
                    const float tAccLimit = mT * mT * 256.0f;
                    if (mayPrune && floorWithS >= topK[p2Cands - 1].score) { prunedMidLoop = true; }

                    // coarse bound on the sorted arrays, split by sorted position
                    if (!prunedMidLoop && mayPrune && useCoarse && coarseSlot[b] >= 0) {
                        const float* bc = blockCoarseSorted.data() + (size_t)coarseSlot[b] * kCoarse * 4;
                        float lb = 0.0f;
                        for (int r = 0; r < kCoarse; ++r) {
                            const float dl = bc[r * 4] - cellCoarseSorted[r * 4];
                            const float da = bc[r * 4 + 1] - cellCoarseSorted[r * 4 + 1];
                            const float db = bc[r * 4 + 2] - cellCoarseSorted[r * 4 + 2];
                            const float dA = bc[r * 4 + 3] - cellCoarseSorted[r * 4 + 3];
                            lb += (dl * dl) + (da * da) + (db * db) + (dA * dA * alphaWeight);
                        }
                        if (lb * kCoarseInv >= tAccLimit) prunedMidLoop = true;
                    }
                    if (prunedMidLoop) {  }
                    else if (transparentMode) {
                        for (int p0 = 0; p0 < 256; p0 += 64) {
                            tDist += sumTexelBlock64Transparent(&block.sortedLab[p0], &segmentSorted[p0], alphaWeight);
                            if (mayPrune && tDist >= tAccLimit) { prunedMidLoop = true; break; }
                        }
                    }
                    else {
                        for (int p0 = 0; p0 < 256; p0 += 64) {
                            tDist += sumTexelBlock64(&block.sortedLab[p0], &segmentSorted[p0], alphaWeight);
                            if (mayPrune && tDist >= tAccLimit) { prunedMidLoop = true; break; }
                        }
                    }
                }
                if (prunedMidLoop) continue;
                tDist = std::sqrt(tDist / 256.0f);
            }

            const float finalScore = (wC * cDist) + (wS * sDist) + (wT * tDist) + (wC * netPenalty);

            if (cullAudit) {
                if (finalScore < auditBestUncapped) { auditBestUncapped = finalScore; auditIdxUncapped = (int)b; }
                if (!culled && finalScore < auditBestCulled) { auditBestCulled = finalScore; auditIdxCulled = (int)b; }
                if (culled) continue;
            }

            insertScore((int)b, finalScore);
        }

        if (cullAudit) {
            g_cullAuditCells++;
            if (auditIdxCulled != auditIdxUncapped) g_cullAuditDisagree++;
        }

        // a fresh pick is a fresh wish, a block that kept its cell keeps the wish it was built from
        if (!isPartial || tempGridIndices[cellIdx] != topK[0].index) tempRailWishes[cellIdx] = 0;
        tempGridIndices[cellIdx] = topK[0].index;
        bestScores[cellIdx] = topK[0].score;    // kept for delta runs
        for (int k = 0; k < p2Cands; ++k) cellCands[k] = topK[k].index;

#if MMMM_VERBOSE

        // full scores of every candidate for this one cell, cull bypassed
        if (row == g_debugRow.load() && col == g_debugCol.load()) {
            struct DebugCand { int idx; float cDist, sDist, tDist, score; };
            std::vector<DebugCand> dbg;
            for (auto scoreIt2 = scoreBegin; scoreIt2 != scoreEndIt; ++scoreIt2) {
                size_t b2 = (size_t)*scoreIt2;
                const ActiveBlock& block2 = tempPalette[b2];
                float netPenalty2 = (block2.ovMask >= 0) ? g_overlayBias.load() : 0.0f;
                float distAlpha2 = block2.meanLab.alpha - segmentMean.alpha;
                float colorImp2 = transparentMode ? (segmentMean.alpha / 100.0f) : 1.0f;
                float distL2 = (block2.meanLab.l - meanAdj.l) * colorImp2;
                float distA2 = (block2.meanLab.a - meanAdj.a) * colorImp2;
                float distB2 = (block2.meanLab.b - meanAdj.b) * colorImp2;
                float cDist2 = std::sqrt((distL2 * distL2) + (distA2 * distA2) + (distB2 * distB2) + (distAlpha2 * distAlpha2 * alphaWeight));
                float sDist2 = 0.0f, tDist2 = 0.0f;
                for (int p = 0; p < 256; ++p) {
                    float dl = block2.flatLab[p].l - segmentFlat[p].l, da = block2.flatLab[p].a - segmentFlat[p].a, db = block2.flatLab[p].b - segmentFlat[p].b, dA = block2.flatLab[p].alpha - segmentFlat[p].alpha;
                    sDist2 += (dl * dl) + (da * da) + (db * db) + (dA * dA * alphaWeight);
                }
                sDist2 = std::sqrt(sDist2 / 256.0f);
                for (int p = 0; p < 256; ++p) {
                    float dl = block2.sortedLab[p].l - segmentSorted[p].l, da = block2.sortedLab[p].a - segmentSorted[p].a, db = block2.sortedLab[p].b - segmentSorted[p].b, dA = block2.sortedLab[p].alpha - segmentSorted[p].alpha;
                    tDist2 += (dl * dl) + (da * da) + (db * db) + (dA * dA * alphaWeight);
                }
                tDist2 = std::sqrt(tDist2 / 256.0f);
                float score2 = (wC * cDist2) + (wS * sDist2) + (wT * tDist2) + (wC * netPenalty2);
                dbg.push_back({ (int)b2, cDist2, sDist2, tDist2, score2 });
            }
            std::sort(dbg.begin(), dbg.end(), [](const DebugCand& a, const DebugCand& b) { return a.score < b.score; });
            DBG_PRINT("[DEBUG CELL %d,%d] %zu candidates, mean L=%.2f a=%.2f b=%.2f alpha=%.2f, min_cDist(culled)=%.3f\n",
                row, col, dbg.size(), segmentMean.l, segmentMean.a, segmentMean.b, segmentMean.alpha, min_cDist);
            for (size_t k = 0; k < dbg.size() && k < 15; ++k) {
                auto& d = dbg[k];
                const ActiveBlock& blk = tempPalette[d.idx];
                DBG_PRINT("  #%zu idx=%d mc_id=%s ov=%s cDist=%.3f sDist=%.3f tDist=%.3f score=%.3f\n",
                    k, d.idx, blk.mc_id.c_str(), blk.ovId.empty() ? "-" : blk.ovId.c_str(), d.cDist, d.sDist, d.tDist, d.score);
            }
        }
#endif

        // error diffusion moves on only on a full pass, so its order stays the same
        if (!isPartial && (dMeth == 1 || dMeth == 2)) {
            const ActiveBlock& bestBlock = tempPalette[topK[0].index];
            ColorLab qErr = { meanAdj.l - bestBlock.meanLab.l, meanAdj.a - bestBlock.meanLab.a, meanAdj.b - bestBlock.meanLab.b };
            if (dMeth == 1) {
                errorGrid[row * errW + (col + 2)].l += qErr.l * 0.4375f; errorGrid[row * errW + (col + 2)].a += qErr.a * 0.4375f; errorGrid[row * errW + (col + 2)].b += qErr.b * 0.4375f;
                errorGrid[(row + 1) * errW + col].l += qErr.l * 0.1875f; errorGrid[(row + 1) * errW + col].a += qErr.a * 0.1875f; errorGrid[(row + 1) * errW + col].b += qErr.b * 0.1875f;
                errorGrid[(row + 1) * errW + (col + 1)].l += qErr.l * 0.3125f; errorGrid[(row + 1) * errW + (col + 1)].a += qErr.a * 0.3125f; errorGrid[(row + 1) * errW + (col + 1)].b += qErr.b * 0.3125f;
                errorGrid[(row + 1) * errW + (col + 2)].l += qErr.l * 0.0625f; errorGrid[(row + 1) * errW + (col + 2)].a += qErr.a * 0.0625f; errorGrid[(row + 1) * errW + (col + 2)].b += qErr.b * 0.0625f;
            }
            else if (dMeth == 2) {
                float err = 0.125f;    // atkinson: 6 x 1/8, a quarter of the error is dropped
                errorGrid[row * errW + (col + 2)].l += qErr.l * err; errorGrid[row * errW + (col + 2)].a += qErr.a * err; errorGrid[row * errW + (col + 2)].b += qErr.b * err;
                errorGrid[row * errW + (col + 3)].l += qErr.l * err; errorGrid[row * errW + (col + 3)].a += qErr.a * err; errorGrid[row * errW + (col + 3)].b += qErr.b * err;
                errorGrid[(row + 1) * errW + col].l += qErr.l * err; errorGrid[(row + 1) * errW + col].a += qErr.a * err; errorGrid[(row + 1) * errW + col].b += qErr.b * err;
                errorGrid[(row + 1) * errW + (col + 1)].l += qErr.l * err; errorGrid[(row + 1) * errW + (col + 1)].a += qErr.a * err; errorGrid[(row + 1) * errW + (col + 1)].b += qErr.b * err;
                errorGrid[(row + 1) * errW + (col + 2)].l += qErr.l * err; errorGrid[(row + 1) * errW + (col + 2)].a += qErr.a * err; errorGrid[(row + 1) * errW + (col + 2)].b += qErr.b * err;
                errorGrid[(row + 2) * errW + (col + 1)].l += qErr.l * err; errorGrid[(row + 2) * errW + (col + 1)].a += qErr.a * err; errorGrid[(row + 2) * errW + (col + 1)].b += qErr.b * err;
            }
        }
        };

    // ticks what the cell really cost: the budget counts the same way, or the bar over- or undershoots
    auto dispatchPixel = [&](int row, int col) -> int {
        int cellIdx = row * tW + col;
        if (fullGridRecalc || tempGridIndices[cellIdx] == -1) {
            processPixel(row, col, false, 1, tempPalette.size());
            return wtPass1;
        }
        return 0;
        };

    markPhase(0);
    auto t_p1 = std::chrono::high_resolution_clock::now();
    DBG_PRINT("[ENGINE] 4/5: Pass 1 (Main Matrix Dispatch) started...\n");
#if MMMM_VERBOSE
    g_cullTotal = 0; g_cullSurvivors = 0;
#endif

    // error diffusion needs row order: one thread
    if (dMeth == 1 || dMeth == 2) {
#if MMMM_VERBOSE
        tl_cullTotal = 0; tl_cullSurvivors = 0; tl_sortTimeNs = 0;
#endif
        for (int row = 0; row < tH; ++row) {
            if (currentCalcId.load() != myCalcId) break;
            for (int col = 0; col < tW; ++col) {
                g_progressVal += dispatchPixel(row, col);
            }
        }
#if MMMM_VERBOSE
        g_cullTotal += tl_cullTotal; g_cullSurvivors += tl_cullSurvivors; g_sortTimeNs += tl_sortTimeNs;
#endif
    }
    else {
        int numThreads = getOptimalThreadCount();
        std::mutex syncMutex;
        std::condition_variable syncCv;
        int tasksCompleted = 0;

        // rows in shares per thread, this thread the remainder
        for (int t = 0; t < numThreads - 1; ++t) {
            getPool()->enqueue([&, t, numThreads, myCalcId = myCalcId]() {
#if MMMM_VERBOSE
                tl_cullTotal = 0; tl_cullSurvivors = 0; tl_sortTimeNs = 0;
#endif
                int startRow = t * (tH / numThreads);
                int endRow = (t + 1) * (tH / numThreads);
                for (int row = startRow; row < endRow; ++row) {
                    if (currentCalcId.load() != myCalcId) break;
                    for (int col = 0; col < tW; ++col) {
                        g_progressVal += dispatchPixel(row, col);
                    }
                }
#if MMMM_VERBOSE
                g_cullTotal += tl_cullTotal; g_cullSurvivors += tl_cullSurvivors; g_sortTimeNs += tl_sortTimeNs;
#endif
                {
                    std::lock_guard<std::mutex> lock(syncMutex);
                    tasksCompleted++;
                    syncCv.notify_one();
                }
                });
        }

        int last_t = numThreads - 1;
        int startRow = last_t * (tH / numThreads);
        int endRow = tH;
#if MMMM_VERBOSE
        tl_cullTotal = 0; tl_cullSurvivors = 0; tl_sortTimeNs = 0;
#endif
        for (int row = startRow; row < endRow; ++row) {
            if (currentCalcId.load() != myCalcId) break;
            for (int col = 0; col < tW; ++col) {
                g_progressVal += dispatchPixel(row, col);
            }
        }
#if MMMM_VERBOSE
        g_cullTotal += tl_cullTotal; g_cullSurvivors += tl_cullSurvivors; g_sortTimeNs += tl_sortTimeNs;
#endif

        {
            std::unique_lock<std::mutex> lock(syncMutex);
            syncCv.wait(lock, [&]() { return tasksCompleted == numThreads - 1; });
        }
        if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
    }

    auto t_p1_end = std::chrono::high_resolution_clock::now();
    DBG_PRINT("[ENGINE] 4/5: Pass 1 finished in %.2f ms.\n", std::chrono::duration<double, std::milli>(t_p1_end - t_p1).count());
    DBG_HEAP("4/5 after Pass 1");
#if MMMM_VERBOSE
    DBG_PRINT("[ENGINE] Pass1 cull: %lld / %lld candidates survived the color cull into the texture loop (%.1f%%).\n",
        g_cullSurvivors.load(), g_cullTotal.load(), g_cullTotal.load() > 0 ? (100.0 * g_cullSurvivors.load() / g_cullTotal.load()) : 0.0);
    DBG_PRINT("[ENGINE] Pass1 Finding-3 audit: %.2f ms summed across all threads was the per-cell 256-item sort (compare against Pass 1's wall time x thread count for the real fraction).\n",
        g_sortTimeNs.load() / 1e6);
#endif

    // the weights after pass 1 scale with how fast it really ran, budget and ticks alike
    markPhase(1);
    {
        const double pass1RealMs = std::chrono::duration<double, std::milli>(t_p1_end - t_p1).count();

        // only a full pass long enough to time is a fair measure
        if (fullGridRecalc && pass1BudgetUnits > 0 && pass1RealMs >= 20.0) {
            const double firstPassPace = (pass1RealMs / (double)pass1BudgetUnits) / WT_ANCHOR_MS_PER_UNIT;
            g_laterScale = std::clamp(g_machineFactor / firstPassPace, 0.05, 20.0);
        }
        long long shift = 0;
        p2w = pass2WeightsAt(g_laterScale);
        if (pass2BudgetUnits > 0) {
            const long long now = pass2Units(p2w, p2ScansGuess, p2CellsGuess);
            shift += now - pass2BudgetUnits;
            pass2BudgetUnits = now;
        }
        wEnh = std::max(1, (int)std::lround(wtEnhance * g_laterScale));
        if (enhanceBudgetUnits > 0) {
            const long long now = (long long)enhanceTicks(tW * tH, enhEff) * wEnh;
            shift += now - enhanceBudgetUnits;
            enhanceBudgetUnits = now;
        }
        g_progressMax += shift;
        if (g_progressMax.load() < g_progressVal.load()) g_progressMax = g_progressVal.load();
    }
    return true;
}


//////////////// LAYOUT
bool PreviewRun::resolveLayout() {
    const std::vector<ActiveBlock>& tempPalette = *paletteRef;

    // settled before pass 2, so it scores the shapes the game will really make
    enhReused = false;
    powerCls = powerClasses(tempPalette);
    forbidPoweredBlocks(tempGridIndices, tW, tH, tempPalette, powerCls, cellCandidates, p2Cands);
    if (localUseOverlays) {
        ovIndex = buildOverlayVariantIndex(tempPalette);
        resolveOverlayGeometry(tempGridIndices, tW, tH, tempPalette, ovIndex, tempRailWishes, tempPlan);
    }
    return true;
}


//////////////// REFINEMENT
bool PreviewRun::refine() {
    const std::vector<ActiveBlock>& tempPalette = *paletteRef;
    const std::vector<ColorLabPacked>& labData = cachedLabData;

    // only when pass 1 solved the grid again
    bool runPass2 = p2Enabled && fullGridRecalc;

    if (runPass2 && !tempGridIndices.empty() && currentCalcId.load() == myCalcId) {
        auto t_p2 = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 5/5: Pass 2 (Iterative Refinement) started...\n");

        // forced to run after the estimate: budget it now
        if (pass2BudgetUnits == 0) {
            pass2BudgetUnits = pass2Units(p2w, p2ScansGuess, p2CellsGuess);
            g_progressMax += pass2BudgetUnits;
        }
        int chunkSizeBlocks = pass2ChunkBlocks(p2Chunk, tW, tH);
        int chunkSizePx = chunkSizeBlocks * 16;

        int numChunksX = (tW + chunkSizeBlocks - 1) / chunkSizeBlocks;
        int numChunksY = (tH + chunkSizeBlocks - 1) / chunkSizeBlocks;

        // the composite is read tile by tile, so each tile is built in a small scratch per thread (~1 GB less at 750)
        // one tile for the whole picture would need two mural-sized scratches, the single old buffer is half that
        const bool fullComposite = (g_legacyFullComposite.load() != 0) || pass2WholeImage(p2Chunk);
        const size_t chunkScratchPx = (size_t)chunkSizePx * chunkSizePx;

        // thread scratch lives on: a buffer far bigger than this run needs goes back first
        auto sizeScratch = [chunkScratchPx](std::vector<ColorLabPacked>& buf) {
            if (buf.capacity() > chunkScratchPx * 2) std::vector<ColorLabPacked>().swap(buf);
            if (buf.size() != chunkScratchPx) buf.assign(chunkScratchPx, ColorLabPacked{});
            };

        // one tile of blocks into dst at its own top-left, for ranking + refinement
        auto fillChunkComposite = [&](ColorLabPacked* dst, int stride, int cx, int cy,
            int width_px, int height_px) {
                const int startBlockX = cx * chunkSizeBlocks, startBlockY = cy * chunkSizeBlocks;
                const int endBlockX = std::min(startBlockX + chunkSizeBlocks, tW);
                const int endBlockY = std::min(startBlockY + chunkSizeBlocks, tH);
                const int startX = cx * chunkSizePx, startY = cy * chunkSizePx;
                for (int br = startBlockY; br < endBlockY; ++br) {
                    for (int bc = startBlockX; bc < endBlockX; ++bc) {
                        int blockIdx = tempGridIndices[br * tW + bc];
                        if (blockIdx < 0 || blockIdx >= (int)tempPalette.size()) blockIdx = 0;
                        const ActiveBlock& block = tempPalette[blockIdx];
                        const int ly0 = br * 16 - startY, lx0 = bc * 16 - startX;
                        for (int pr = 0; pr < 16; ++pr) {
                            const int ly = ly0 + pr;
                            if (ly < 0 || ly >= height_px) continue;
                            for (int pc = 0; pc < 16; ++pc) {
                                const int lx = lx0 + pc;
                                if (lx < 0 || lx >= width_px) continue;
                                dst[(size_t)ly * stride + lx] = packLab(block.flatLab[pr * 16 + pc]);
                            }
                        }
                    }
                }
            };

        if (fullComposite) resizeReclaiming(compositeLab, (size_t)targetW_px * targetH_px);
        else std::vector<ColorLabPacked>().swap(compositeLab);    // release it outright

        // threaded + ticked: this rebuild was 85% of pass 2 and stalled the bar (tested: 2,023 of 2,386 ms at 550)
        int compositeTickWeight = p2w.rebuildCell;
        auto rebuildRow = [&](int r) {
            for (int c = 0; c < tW; ++c) {
                int blockIdx = tempGridIndices[r * tW + c];
                if (blockIdx < 0 || blockIdx >= tempPalette.size()) blockIdx = 0;
                const ActiveBlock& block = tempPalette[blockIdx];
                for (int pr = 0; pr < 16; ++pr) {
                    for (int pc = 0; pc < 16; ++pc) {
                        int py = r * 16 + pr, px = c * 16 + pc;
                        compositeLab[py * targetW_px + px] = packLab(block.flatLab[pr * 16 + pc]);
                    }
                }
                g_progressVal += compositeTickWeight;    // per cell, so the bar moves smoothly
            }
            };
        if (fullComposite) {
            int numThreads = std::max(1, std::min(getOptimalThreadCount(), tH));
            if (numThreads <= 1) {
                for (int r = 0; r < tH; ++r) {
                    if (currentCalcId.load() != myCalcId) break;
                    rebuildRow(r);
                }
            }
            else {
                std::mutex syncMutex;
                std::condition_variable syncCv;
                int tasksCompleted = 0;

                for (int t = 0; t < numThreads - 1; ++t) {
                    getPool()->enqueue([&, t, numThreads, myCalcId = myCalcId]() {
                        for (int r = t; r < tH; r += numThreads) {
                            if (currentCalcId.load() != myCalcId) break;
                            rebuildRow(r);
                        }
                        {
                            std::lock_guard<std::mutex> lock(syncMutex);
                            tasksCompleted++;
                            syncCv.notify_one();
                        }
                        });
                }
                int last_t = numThreads - 1;
                for (int r = last_t; r < tH; r += numThreads) {
                    if (currentCalcId.load() != myCalcId) break;
                    rebuildRow(r);
                }
                {
                    std::unique_lock<std::mutex> lock(syncMutex);
                    syncCv.wait(lock, [&]() { return tasksCompleted == numThreads - 1; });
                }
            }
        }
        if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
        DBG_PRINT("[ENGINE] 5/5-prep: compositeLab rebuild finished at +%.2f ms.\n", std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_p2).count());
        markPhase(2);

        struct ChunkError { int chunkX, chunkY; float score; };

        // a slot per tile, threads write their own: no lock, no push_back race
        std::vector<ChunkError> chunkErrors((size_t)numChunksX * numChunksY);

        // with dithering, colour error is against the photo plus the cell shift, or refinement undoes the dither
        const int32_t* const p2Shift = (ditherWholeCell != 0 && dMeth != 0 && ditherShift.size() == (size_t)tW * tH * 3)
            ? ditherShift.data() : nullptr;

        // structure stays against the photo, and counts less the stronger the dithering -- none at full, 1 without
        const float ssimKeep = p2Shift ? std::clamp(1.0f - dInt, 0.0f, 1.0f) : 1.0f;

        float totalWeight = p2MSE + p2SSIM * ssimKeep;
        float wMSE_norm = (totalWeight > 0.0f) ? (p2MSE / totalWeight) : (p2Shift ? 1.0f : 0.5f);
        float wSSIM_norm = (totalWeight > 0.0f) ? (p2SSIM * ssimKeep / totalWeight) : (p2Shift ? 0.0f : 0.5f);

        int cellsPerChunk = chunkSizeBlocks * chunkSizeBlocks;
        const int rankTickPerCell = p2w.rankCell;

        // rank the tiles of one row
        auto scoreChunkRow = [&](int cy) {
            thread_local std::vector<ColorLabPacked> scratch;
            if (!fullComposite) sizeScratch(scratch);

            for (int cx = 0; cx < numChunksX; ++cx) {
                int startX = cx * chunkSizePx;
                int startY = cy * chunkSizePx;
                int width_px = std::min(chunkSizePx, targetW_px - startX);
                int height_px = std::min(chunkSizePx, targetH_px - startY);

                const ColorLabPacked* base;
                int stride;
                if (fullComposite) {
                    base = compositeLab.data() + (size_t)startY * targetW_px + startX;
                    stride = targetW_px;
                }
                else {
                    fillChunkComposite(scratch.data(), chunkSizePx, cx, cy, width_px, height_px);
                    base = scratch.data();
                    stride = chunkSizePx;
                    const int cellsW = std::min(chunkSizeBlocks, tW - cx * chunkSizeBlocks);
                    const int cellsH = std::min(chunkSizeBlocks, tH - cy * chunkSizeBlocks);

                    // the stamping the global rebuild would have done happens here, so its ticks too
                    g_progressVal += compositeTickWeight * std::max(0, cellsW) * std::max(0, cellsH);
                }

                // a big tile is scored long on one thread: a third of each row of blocks ticked in each of the three sweeps
                const bool tickInside = chunkSizeBlocks >= 8;
                const int rowUnits = rankTickPerCell * (width_px / 16);
                float mse = calculateSubMatrixMSE(labData, base, stride, targetW_px, startX, startY, width_px, height_px, currentBgMode,
                    tickInside ? rowUnits / 3 : 0, p2Shift, tW);
                float ssim = calculateSubMatrixSSIM(labData, base, stride, targetW_px, startX, startY, width_px, height_px,
                    tickInside ? rowUnits / 3 : 0, tickInside ? rowUnits - 2 * (rowUnits / 3) : 0);

                float compositeScore = (wMSE_norm * mse) + (wSSIM_norm * (1.0f - ssim));
                chunkErrors[(size_t)cy * numChunksX + cx] = { cx, cy, compositeScore };
                if (!tickInside) g_progressVal += rankTickPerCell * (width_px / 16) * (height_px / 16);
            }
            };

        {
            int numThreads = std::max(1, std::min(getOptimalThreadCount(), numChunksY));
            if (numThreads <= 1) {
                for (int cy = 0; cy < numChunksY; ++cy) {
                    if (currentCalcId.load() != myCalcId) break;
                    scoreChunkRow(cy);
                }
            }
            else {
                std::mutex syncMutex;
                std::condition_variable syncCv;
                int tasksCompleted = 0;

                // strided rows: a sky band and a busy band would unbalance contiguous ranges
                for (int t = 0; t < numThreads - 1; ++t) {
                    getPool()->enqueue([&, t, numThreads, myCalcId = myCalcId]() {
                        for (int cy = t; cy < numChunksY; cy += numThreads) {
                            if (currentCalcId.load() != myCalcId) break;
                            scoreChunkRow(cy);
                        }
                        {
                            std::lock_guard<std::mutex> lock(syncMutex);
                            tasksCompleted++;
                            syncCv.notify_one();
                        }
                        });
                }
                int last_t = numThreads - 1;
                for (int cy = last_t; cy < numChunksY; cy += numThreads) {
                    if (currentCalcId.load() != myCalcId) break;
                    scoreChunkRow(cy);
                }
                {
                    std::unique_lock<std::mutex> lock(syncMutex);
                    syncCv.wait(lock, [&]() { return tasksCompleted == numThreads - 1; });
                }
            }
        }
        if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
        DBG_PRINT("[ENGINE] 5/5a: Pass 2 scoring finished at +%.2f ms.\n", std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_p2).count());

        // in the tile-by-tile mode the rebuild units were ticked during ranking
        markPhase(3);
        {
            const double threads = getOptimalThreadCount();
            const double rankSpread = threads / std::max(1.0, std::min(threads, (double)p2TilesY));

            // rebuild + ranking cost the same per cell on any picture: the speed of this machine, for the next run
            const double refUnits = (double)tW * tH * (wtPass2Rebuild + WT_P2_RANK * rankSpread);
            const double ms = phaseMs[2] + phaseMs[3];
            if (ms >= 20.0 && refUnits > 0) g_machineFactor = std::clamp(ms / (refUnits * WT_ANCHOR_MS_PER_UNIT), 0.25, 4.0);
        }

        std::sort(chunkErrors.begin(), chunkErrors.end(), [](const ChunkError& a, const ChunkError& b) {
            return a.score > b.score;
            });

        int targetRefineCount = (int)(chunkErrors.size() * p2Thresh);

        // big tiles: the share can round to zero tiles, and the pass would do nothing
        if (targetRefineCount < 1 && p2Thresh > 0.0f && !chunkErrors.empty()) targetRefineCount = 1;

        // real candidates of the chosen tiles, counted -- the worst-case guess stalled the bar, then jumped
        long long realCandidateCount = 0, refinedCellCount = 0;
        std::vector<int> packedAt(tempPalette.size(), -1);    // palette entry to its slot in candPacked
        int packedCount = 0;
        for (int i = 0; i < targetRefineCount; ++i) {
            int cx = chunkErrors[i].chunkX;
            int cy = chunkErrors[i].chunkY;
            int startBlockX = cx * chunkSizeBlocks;
            int startBlockY = cy * chunkSizeBlocks;
            int endBlockX = std::min(startBlockX + chunkSizeBlocks, tW);
            int endBlockY = std::min(startBlockY + chunkSizeBlocks, tH);
            for (int br = startBlockY; br < endBlockY; ++br) {
                for (int bc = startBlockX; bc < endBlockX; ++bc) {
                    int cellIdx = br * tW + bc;
                    const int* candidates = &cellCandidates[(size_t)cellIdx * p2Cands];
                    int originalChoice = tempGridIndices[cellIdx];
                    if (originalChoice < 0 || originalChoice >= tempPalette.size()) originalChoice = 0;
                    ++refinedCellCount;
                    for (int k = 0; k < p2Cands; ++k) {
                        int candidateIdx = candidates[k];
                        if (candidateIdx <= 0 || candidateIdx >= tempPalette.size()) continue;

                        // any listed block can come up for a trial once a neighbouring choice has changed
                        if (packedAt[candidateIdx] < 0) packedAt[candidateIdx] = packedCount++;
                        if (candidateIdx == originalChoice) continue;
                        realCandidateCount++;
                    }
                }
            }
        }

        // every block that can be tried, packed once -- repacking per trial was most of its cost
        std::vector<ColorLabPacked> candPacked((size_t)packedCount * 256);
        for (size_t b = 0; b < packedAt.size(); ++b) {
            if (packedAt[b] < 0) continue;
            ColorLabPacked* dst = &candPacked[(size_t)packedAt[b] * 256];
            const ActiveBlock& block = tempPalette[b];
            for (int k = 0; k < 256; ++k) dst[k] = packLab(block.flatLab[k]);
        }

        long long oldRefinementEstimate = p2ScansGuess * p2w.refineScan * p2Passes + p2CellsGuess * p2w.setupCell;
        long long newRefinementEstimate = realCandidateCount * p2w.refineScan * p2Passes + refinedCellCount * p2w.setupCell;
        g_progressMax += (newRefinementEstimate - oldRefinementEstimate);
        if (g_progressMax.load() < g_progressVal.load()) g_progressMax = g_progressVal.load();
        DBG_PRINT("[ENGINE] 5/5b: real candidate count = %lld (targetRefineCount=%d, cellsPerChunk=%d) at +%.2f ms.\n",
            realCandidateCount, targetRefineCount, cellsPerChunk, std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_p2).count());

        // tiles own disjoint cells, so no lock beyond the progress counter
        struct CellStatDelta { long long sumY = 0, sumY2 = 0, sumXY = 0; double sumErrRaw = 0.0; };

        // what one cell adds to the tile sums, read from the composite as it stands
        auto cellContrib = [&](int br, int bc, const ColorLabPacked* comp, int compStride,
            int compOffX, int compOffY) -> CellStatDelta {
            CellStatDelta d;
            float alphaW = (currentBgMode == 3) ? 4.0f : 1.5f;
            const int32_t* const cs = p2Shift ? p2Shift + (size_t)(br * tW + bc) * 3 : nullptr;
            if (!cs) {
                for (int pr = 0; pr < 16; ++pr) {
                    for (int pc = 0; pc < 16; ++pc) {
                        int idx = (br * 16 + pr) * targetW_px + (bc * 16 + pc);
                        const ColorLabPacked& o = labData[idx];
                        const ColorLabPacked& r = comp[(size_t)(br * 16 + pr - compOffY) * compStride
                                                       + (bc * 16 + pc - compOffX)];
                        d.sumY += r.l;
                        d.sumY2 += (long long)r.l * r.l;
                        d.sumXY += (long long)o.l * r.l;
                        float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                        float dL = (o.l - r.l) * LAB_L_INV_SCALE * colorImp;
                        float dA = (o.a - r.a) * LAB_AB_INV_SCALE * colorImp;
                        float dB = (o.b - r.b) * LAB_AB_INV_SCALE * colorImp;
                        float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                        d.sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                    }
                }
                return d;
            }

            // the same sums against the dithered picture: colour error takes the cell shift
            const int sL = cs[0], sA = cs[1], sB = cs[2];
            for (int pr = 0; pr < 16; ++pr) {
                for (int pc = 0; pc < 16; ++pc) {
                    int idx = (br * 16 + pr) * targetW_px + (bc * 16 + pc);
                    const ColorLabPacked& o = labData[idx];
                    const ColorLabPacked& r = comp[(size_t)(br * 16 + pr - compOffY) * compStride
                                                   + (bc * 16 + pc - compOffX)];
                    d.sumY += r.l;
                    d.sumY2 += (long long)r.l * r.l;
                    d.sumXY += (long long)o.l * r.l;
                    float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                    float dL = (o.l + sL - r.l) * LAB_L_INV_SCALE * colorImp;
                    float dA = (o.a + sA - r.a) * LAB_AB_INV_SCALE * colorImp;
                    float dB = (o.b + sB - r.b) * LAB_AB_INV_SCALE * colorImp;
                    float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                    d.sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                }
            }
            return d;
            };

        // the same for a block not written yet -- term for term, so trying and writing agree
        auto cellContribOf = [&](int br, int bc, const ColorLabPacked* tile) -> CellStatDelta {
            CellStatDelta d;
            float alphaW = (currentBgMode == 3) ? 4.0f : 1.5f;
            const int32_t* const cs = p2Shift ? p2Shift + (size_t)(br * tW + bc) * 3 : nullptr;
            if (!cs) {
                for (int pr = 0; pr < 16; ++pr) {
                    for (int pc = 0; pc < 16; ++pc) {
                        int idx = (br * 16 + pr) * targetW_px + (bc * 16 + pc);
                        const ColorLabPacked& o = labData[idx];
                        const ColorLabPacked& r = tile[pr * 16 + pc];
                        d.sumY += r.l;
                        d.sumY2 += (long long)r.l * r.l;
                        d.sumXY += (long long)o.l * r.l;
                        float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                        float dL = (o.l - r.l) * LAB_L_INV_SCALE * colorImp;
                        float dA = (o.a - r.a) * LAB_AB_INV_SCALE * colorImp;
                        float dB = (o.b - r.b) * LAB_AB_INV_SCALE * colorImp;
                        float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                        d.sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                    }
                }
                return d;
            }

            // the same sums against the dithered picture: colour error takes the cell shift
            const int sL = cs[0], sA = cs[1], sB = cs[2];
            for (int pr = 0; pr < 16; ++pr) {
                for (int pc = 0; pc < 16; ++pc) {
                    int idx = (br * 16 + pr) * targetW_px + (bc * 16 + pc);
                    const ColorLabPacked& o = labData[idx];
                    const ColorLabPacked& r = tile[pr * 16 + pc];
                    d.sumY += r.l;
                    d.sumY2 += (long long)r.l * r.l;
                    d.sumXY += (long long)o.l * r.l;
                    float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                    float dL = (o.l + sL - r.l) * LAB_L_INV_SCALE * colorImp;
                    float dA = (o.a + sA - r.a) * LAB_AB_INV_SCALE * colorImp;
                    float dB = (o.b + sB - r.b) * LAB_AB_INV_SCALE * colorImp;
                    float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                    d.sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                }
            }
            return d;
            };

        auto refineChunk = [&](int i) {
            int cx = chunkErrors[i].chunkX;
            int cy = chunkErrors[i].chunkY;

            int startBlockX = cx * chunkSizeBlocks;
            int startBlockY = cy * chunkSizeBlocks;
            int endBlockX = std::min(startBlockX + chunkSizeBlocks, tW);
            int endBlockY = std::min(startBlockY + chunkSizeBlocks, tH);

            int startX = cx * chunkSizePx;
            int startY = cy * chunkSizePx;
            int width_px = std::min(chunkSizePx, targetW_px - startX);
            int height_px = std::min(chunkSizePx, targetH_px - startY);
            int totalPx = width_px * height_px;

            float currentBestScore = chunkErrors[i].score;

            // the ranking scratch arrangement; in full-composite mode the mural buffer
            thread_local std::vector<ColorLabPacked> refineScratch;
            ColorLabPacked* comp;
            int compStride, compOffX, compOffY;
            if (fullComposite) {
                comp = compositeLab.data();
                compStride = targetW_px; compOffX = 0; compOffY = 0;
            }
            else {
                sizeScratch(refineScratch);
                fillChunkComposite(refineScratch.data(), chunkSizePx, cx, cy, width_px, height_px);
                comp = refineScratch.data();
                compStride = chunkSizePx; compOffX = startX; compOffY = startY;
            }

            // tile sums once, then per trial only the cell delta -- O(256), not O(tile) (tested: 2x2 to 4x4 was 4x slower)
            long long sumX = 0, sumX2 = 0;
            long long sumY = 0, sumY2 = 0, sumXY = 0;
            double sumErrRaw = 0.0;
            {
                float alphaW = (currentBgMode == 3) ? 4.0f : 1.5f;
                for (int dy = 0; dy < height_px; ++dy) {
                    if (!p2Shift) {
                        for (int dx = 0; dx < width_px; ++dx) {
                            int idx = (startY + dy) * targetW_px + (startX + dx);
                            const ColorLabPacked& o = labData[idx];
                            const ColorLabPacked& r = comp[(size_t)dy * compStride + dx
                                + (fullComposite ? (size_t)startY * targetW_px + startX : 0)];
                            sumX += o.l; sumX2 += (long long)o.l * o.l;
                            sumY += r.l; sumY2 += (long long)r.l * r.l; sumXY += (long long)o.l * r.l;
                            float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                            float dL = (o.l - r.l) * LAB_L_INV_SCALE * colorImp;
                            float dA = (o.a - r.a) * LAB_AB_INV_SCALE * colorImp;
                            float dB = (o.b - r.b) * LAB_AB_INV_SCALE * colorImp;
                            float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                            sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                        }
                    }
                    else {
                        const int32_t* rowShift = p2Shift + 3 * ((size_t)((startY + dy) >> 4) * tW);
                        for (int dx = 0; dx < width_px; ++dx) {
                            int idx = (startY + dy) * targetW_px + (startX + dx);
                            const ColorLabPacked& o = labData[idx];
                            const ColorLabPacked& r = comp[(size_t)dy * compStride + dx
                                + (fullComposite ? (size_t)startY * targetW_px + startX : 0)];
                            const int32_t* s = rowShift + 3 * ((startX + dx) >> 4);
                            sumX += o.l; sumX2 += (long long)o.l * o.l;
                            sumY += r.l; sumY2 += (long long)r.l * r.l; sumXY += (long long)o.l * r.l;
                            float colorImp = (currentBgMode == 3) ? (o.alpha / 255.0f) : 1.0f;
                            float dL = (o.l + s[0] - r.l) * LAB_L_INV_SCALE * colorImp;
                            float dA = (o.a + s[1] - r.a) * LAB_AB_INV_SCALE * colorImp;
                            float dB = (o.b + s[2] - r.b) * LAB_AB_INV_SCALE * colorImp;
                            float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;
                            sumErrRaw += (double)(dL * dL + dA * dA + dB * dB + dAlpha * dAlpha * alphaW);
                        }
                    }
                    if ((dy & 15) == 15) g_progressVal += (endBlockX - startBlockX) * p2w.setupCell;
                }
            }

            // the closed form of calculateSubMatrixMSE + SSIM, from the sums
            auto scoreFromSums = [&](long long sY, long long sY2, long long sXY, double sErr) -> float {
                double meanXd = (double)sumX / totalPx, meanYd = (double)sY / totalPx;
                double varXd = (double)sumX2 / totalPx - meanXd * meanXd;
                double varYd = (double)sY2 / totalPx - meanYd * meanYd;
                double covXYd = (double)sXY / totalPx - meanXd * meanYd;
                double num = (2.0 * meanXd * meanYd + SSIM_C1_PACKED) * (2.0 * covXYd + SSIM_C2_PACKED);
                double den = (meanXd * meanXd + meanYd * meanYd + SSIM_C1_PACKED) * (varXd + varYd + SSIM_C2_PACKED);
                float ssim = (float)(num / den);
                float mse = (float)(sErr / (totalPx * 4.0 * 10000.0));
                return (wMSE_norm * mse) + (wSSIM_norm * (1.0f - ssim));
                };

            // each sweep starts where the last left; one that swaps nothing would repeat exactly, so stop
            for (int sweep = 0; sweep < p2Passes; ++sweep) {
            bool swappedThisSweep = false;
            long long scansThisSweep = 0, pendingTicks = 0;
            for (int br = startBlockY; br < endBlockY; ++br) {

                // one add per row of blocks: every thread adding after each trial slowed the trials down
                if (pendingTicks) { g_progressVal += pendingTicks; pendingTicks = 0; }

                // a tile can be the whole mural, so a cancelled run has to be able to leave it
                if (currentCalcId.load() != myCalcId) return;
                for (int bc = startBlockX; bc < endBlockX; ++bc) {
                    int cellIdx = br * tW + bc;
                    const int* candidates = &cellCandidates[(size_t)cellIdx * p2Cands];
                    int originalChoice = tempGridIndices[cellIdx];
                    if (originalChoice < 0 || originalChoice >= tempPalette.size()) originalChoice = 0;

                    // the cell as it stands, at the first trial and again after a swap -- the composite changes only on a swap
                    CellStatDelta oldC;
                    bool oldKnown = false;

                    for (int k = 0; k < p2Cands; ++k) {
                        int candidateIdx = candidates[k];
                        if (candidateIdx <= 0 || candidateIdx >= tempPalette.size() || candidateIdx == originalChoice) continue;
                        ++scansThisSweep;

                        const ActiveBlock& candBlock = tempPalette[candidateIdx];

                        // the pass 1 flat cost, on the mse/ssim scale
                        float overlayPenalty = (candBlock.ovMask >= 0) ? (g_overlayBias.load() * OVERLAY_P2_SCALE) : 0.0f;

#if MMMM_VERBOSE
                        auto t_mse0 = std::chrono::high_resolution_clock::now();
#endif
                        if (!oldKnown) { oldC = cellContrib(br, bc, comp, compStride, compOffX, compOffY); oldKnown = true; }
                        const ColorLabPacked* candTile = &candPacked[(size_t)packedAt[candidateIdx] * 256];
                        CellStatDelta newC = cellContribOf(br, bc, candTile);

                        long long tryY = sumY - oldC.sumY + newC.sumY;
                        long long tryY2 = sumY2 - oldC.sumY2 + newC.sumY2;
                        long long tryXY = sumXY - oldC.sumXY + newC.sumXY;
                        double tryErr = sumErrRaw - oldC.sumErrRaw + newC.sumErrRaw;

                        float newScore = scoreFromSums(tryY, tryY2, tryXY, tryErr) + overlayPenalty;
#if MMMM_VERBOSE
                        tl_mseSsimTimeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t_mse0).count();
#endif
                        pendingTicks += p2w.refineScan;    // one real trial

                        if (newScore < currentBestScore) {
                            currentBestScore = newScore;
                            originalChoice = candidateIdx;
                            swappedThisSweep = true;

                            // accepted: the trial sums are the tile now
                            sumY = tryY; sumY2 = tryY2; sumXY = tryXY; sumErrRaw = tryErr;
                            tempGridIndices[cellIdx] = candidateIdx;
                            tempRailWishes[cellIdx] = 0;
                            for (int pr = 0; pr < 16; ++pr) {
                                ColorLabPacked* row = &comp[(size_t)(br * 16 + pr - compOffY) * compStride + (bc * 16 - compOffX)];
                                std::copy(candTile + pr * 16, candTile + pr * 16 + 16, row);
                            }
                            oldC = newC;
                        }
                    }
                }
            }
            if (pendingTicks) g_progressVal += pendingTicks;
            if (!swappedThisSweep) {

                // the budget of the skipped sweeps, handed back
                g_progressVal += (long long)(scansThisSweep * p2w.refineScan * (p2Passes - 1 - sweep));
                break;
            }
            }
            };

        {
            int numThreads = std::max(1, std::min(getOptimalThreadCount(), std::max(1, targetRefineCount)));
            if (numThreads <= 1) {
                for (int i = 0; i < targetRefineCount; ++i) {
                    if (currentCalcId.load() != myCalcId) break;
                    refineChunk(i);
                }
            }
            else {
                std::mutex syncMutex;
                std::condition_variable syncCv;
                int tasksCompleted = 0;

#if MMMM_VERBOSE
                g_mseSsimTimeNs = 0;
#endif

                // strided: tiles come worst first, a contiguous split gave one thread the hard ones (tested: bar stuck at 43%)
                for (int t = 0; t < numThreads - 1; ++t) {
                    getPool()->enqueue([&, t, numThreads, myCalcId = myCalcId]() {
#if MMMM_VERBOSE
                        tl_mseSsimTimeNs = 0;
#endif
                        for (int i = t; i < targetRefineCount; i += numThreads) {
                            if (currentCalcId.load() != myCalcId) break;
                            refineChunk(i);
                        }
#if MMMM_VERBOSE
                        g_mseSsimTimeNs += tl_mseSsimTimeNs;
#endif
                        {
                            std::lock_guard<std::mutex> lock(syncMutex);
                            tasksCompleted++;
                            syncCv.notify_one();
                        }
                        });
                }
                int last_t = numThreads - 1;
#if MMMM_VERBOSE
                tl_mseSsimTimeNs = 0;
#endif
                for (int i = last_t; i < targetRefineCount; i += numThreads) {
                    if (currentCalcId.load() != myCalcId) break;
                    refineChunk(i);
                }
#if MMMM_VERBOSE
                g_mseSsimTimeNs += tl_mseSsimTimeNs;
#endif
                {
                    std::unique_lock<std::mutex> lock(syncMutex);
                    syncCv.wait(lock, [&]() { return tasksCompleted == numThreads - 1; });
                }
            }
        }
        if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }
        DBG_PRINT("[ENGINE] 5/5c: Pass 2 refinement finished at +%.2f ms.\n", std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_p2).count());
        markPhase(4);
#if MMMM_VERBOSE
        DBG_PRINT("[ENGINE] Pass2 Finding-2 audit: %.2f ms summed across all threads was calculateSubMatrixMSE+SSIM (compare against the refinement wall time x thread count for the real fraction).\n",
            g_mseSsimTimeNs.load() / 1e6);
#endif

        // pass 2 swaps freely, so power + geometry settle again
        forbidPoweredBlocks(tempGridIndices, tW, tH, tempPalette, powerCls, cellCandidates, p2Cands);
        if (localUseOverlays) resolveOverlayGeometry(tempGridIndices, tW, tH, tempPalette, ovIndex, tempRailWishes, tempPlan);

        auto t_p2_end = std::chrono::high_resolution_clock::now();
        DBG_PRINT("[ENGINE] 5/5: Pass 2 finished in %.2f ms.\n", std::chrono::duration<double, std::milli>(t_p2_end - t_p2).count());
        DBG_HEAP("5/5 after Pass 2");
    }
    else {
        DBG_PRINT("[ENGINE] 5/5: Pass 2 skipped (Disabled or no changes).\n");
    }
    return true;
}


//////////////// ENHANCEMENT
bool PreviewRun::enhance() {
    const std::vector<ActiveBlock>& tempPalette = *paletteRef;
    const std::vector<ColorLabPacked>& labData = cachedLabData;

    // on a copy: tempGridIndices stays the passes result, the next delta run starts from it
    enhanced = false;
    if (enhRuns && !tempGridIndices.empty() && currentCalcId.load() == myCalcId) {
        markPhase(4);    // whatever sat between the refinement and here
        auto t_enh = std::chrono::high_resolution_clock::now();
        bool reusable = false;
        {
            std::lock_guard<std::mutex> lock(dataMutex);

            // nothing the stage reads has changed: same photo, palette, settings + engine grid
            reusable = !fullPaletteRebuild && !needsLab
                && enhSet == enhancedWith
                && previewGridIndices.size() == tempGridIndices.size()
                && engineGridIndices == tempGridIndices;
            if (reusable) enhancedGrid = previewGridIndices;
        }
        if (reusable) {
            enhanced = true;
            enhReused = true;    // and previewRailPlan still belongs to it
            g_progressVal += enhanceBudgetUnits;
            DBG_PRINT("[ENGINE] 6/6: Enhancement reused (no changes).\n");
        }
        else {
            const int np = (int)tempPalette.size();
            EnhanceIO eio;
            eio.gw = tW; eio.gh = tH;
            eio.tiles.assign(np, nullptr);
            eio.placeable.assign(np, 0);
            eio.baseOf.assign(np, -1);
            for (int i = 0; i < np; ++i) {
                const ActiveBlock& ab = tempPalette[i];
                if (ab.rgbArray.size() != 16 * 16 * 4) continue;
                eio.tiles[i] = ab.rgbArray.data();
                if (ab.ovMask >= 0) { eio.baseOf[i] = ab.ovBaseIdx; continue; }
                if (ab.mc_id == "air") continue;
                bool opaque = true;
                for (size_t k = 3; k < ab.rgbArray.size(); k += 4) if (ab.rgbArray[k] != 255) { opaque = false; break; }
                eio.placeable[i] = opaque ? 1 : 0;
                eio.baseOf[i] = i;
            }
            std::vector<int> bgRgb = getPreviewBgColor(currentBgMode, biomeStr);
            for (int k = 0; k < 3; ++k) eio.bg[k] = (unsigned char)std::clamp(bgRgb[k], 0, 255);

            const int rowPx = tW * 16;
            eio.fetchRow = [&labData, rowPx](int y, float* out) {
                const ColorLabPacked* src = &labData[(size_t)y * rowPx];
                for (int x = 0; x < rowPx; ++x, out += 4) {
                    const ColorLab c = unpackLab(src[x]);
                    labToLinearRgb(c.l, c.a, c.b, out);
                    out[3] = c.alpha * 0.01f;
                }
                };

            // carrier block + overlay to the entry showing that pair
            std::unordered_map<std::string, int> onBase;
            auto pairKey = [](int base, const ActiveBlock& ab) {
                return std::to_string(base) + "\x1f" + ab.ovId + "\x1f" + ab.ovState + "\x1f" + std::to_string(ab.ovMask);
                };
            if (localUseOverlays) {
                for (int i = 0; i < np; ++i) {
                    const ActiveBlock& ab = tempPalette[i];
                    if (ab.ovMask >= 0 && ab.ovBaseIdx >= 0) onBase.emplace(pairKey(ab.ovBaseIdx, ab), i);
                }
            }
            eio.rebase = [&](int entry, int base) -> int {
                if (entry < 0 || entry >= np) return -1;
                auto it = onBase.find(pairKey(base, tempPalette[entry]));
                return (it == onBase.end()) ? -1 : it->second;
                };

            eio.parallelFor = [&](int n, const std::function<void(int)>& body) {
                if (n <= 0) return;
                const int numThreads = std::min(getOptimalThreadCount(), n);
                std::atomic<int> next{ 0 };
                auto run = [&]() { for (int i = next.fetch_add(1); i < n; i = next.fetch_add(1)) body(i); };
                std::mutex syncMutex;
                std::condition_variable syncCv;
                int tasksCompleted = 0;
                for (int t = 0; t < numThreads - 1; ++t) {
                    getPool()->enqueue([&]() {
                        run();
                        std::lock_guard<std::mutex> lock(syncMutex);
                        tasksCompleted++;
                        syncCv.notify_one();
                        });
                }
                run();
                std::unique_lock<std::mutex> lock(syncMutex);
                syncCv.wait(lock, [&]() { return tasksCompleted == numThreads - 1; });
                };
            eio.cancelled = [&]() { return currentCalcId.load() != myCalcId; };
            eio.tick = [&](int ticks) { g_progressVal += (long long)ticks * wEnh; };

            enhancedGrid = tempGridIndices;
            std::string enhNote;
            enhanced = enhanceMural(enhancedGrid, eio, enhSet, enhNote);
            if (currentCalcId.load() != myCalcId) { DBG_PRINT("[ENGINE] calcId=%d CANCELLED mid-flight during enhancement (currentCalcId now=%d)\n", myCalcId, currentCalcId.load()); return false; }

            // the stage speaks up only when it left something out, and once
            static std::string lastNote;
            if (!enhNote.empty() && enhNote != lastNote) Log(enhNote);
            lastNote = enhNote;

            // feathering never changes which cells carry overlays, this keeps the guarantee from resting on that
            if (enhanced) forbidPoweredBlocks(enhancedGrid, tW, tH, tempPalette, powerCls, cellCandidates, p2Cands);
            if (enhanced && localUseOverlays) {
                RailWishes enhWishes = tempRailWishes;    // the stage moves blocks under overlays, the wishes stay
                resolveOverlayGeometry(enhancedGrid, tW, tH, tempPalette, ovIndex, enhWishes, enhPlan);
            }
            DBG_PRINT("[ENGINE] 6/6: Enhancement %s in %.2f ms.\n", enhanced ? "finished" : "skipped",
                std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_enh).count());
        }
    }
    if (enhRuns) markPhase(5);
    return true;
}


//////////////// COMMIT
bool PreviewRun::commit() {
    const std::vector<ActiveBlock>& tempPalette = *paletteRef;
    const std::vector<int>& finalGrid = enhanced ? enhancedGrid : tempGridIndices;

    if (currentCalcId.load() == myCalcId) {
#if MMMM_VERBOSE

        // census: variants 0 = never in the palette, cells 0 = in it but colour never picks it
        if (localUseOverlays) {
            std::map<std::string, int> ovVariants, ovCells;
            for (const auto& ab : tempPalette) if (!ab.ovId.empty()) ovVariants[ab.ovId]++;
            for (int idx : tempGridIndices) {
                if (idx < 0 || idx >= (int)tempPalette.size()) continue;
                const std::string& oid = tempPalette[idx].ovId;
                if (!oid.empty()) ovCells[oid]++;
            }
            if (!ovVariants.empty()) {
                std::string report = "Overlays used:";
                for (const auto& kv : ovVariants) {
                    report += " " + kv.first + "=" + std::to_string(ovCells[kv.first])
                        + "/" + std::to_string(kv.second) + "v;";
                }
                Log(report);
            }
        }
#endif

        std::string railNote;
        std::set<std::string> warningBlocks;
        for (int idx : finalGrid) {
            if (idx < 0 || idx >= tempPalette.size()) continue;
            std::string mc_id = tempPalette[idx].mc_id;
            std::string base_id = mc_id.substr(0, mc_id.find('['));
            auto extIt = extBlockInfo.find(base_id);
            if (extIt != extBlockInfo.end() && extIt->second.deteriorates) {
                warningBlocks.insert(base_id);
            }
        }

        // falling blocks with nothing in the mural to stand on -- floor + ceiling: all, wall: no fixed block below
        int unsupported = 0;
        {
            std::unordered_map<std::string, bool> known;
            std::vector<uint8_t> falls(tempPalette.size(), 0);
            for (size_t i = 0; i < tempPalette.size(); ++i) {
                const std::string id = tempPalette[i].mc_id.substr(0, tempPalette[i].mc_id.find('['));
                auto it = known.find(id);
                if (it == known.end()) it = known.emplace(id, fallsDown(id)).first;
                falls[i] = it->second ? 1 : 0;
            }
            const int np = (int)tempPalette.size();
            if (oriStr == "horizontal") {

                // tested: a sculk vein under a ceiling block holds it up, lichen and vines do not
                const bool ceiling = drctStr == "bottom";
                for (int idx : finalGrid) if (idx >= 0 && idx < np && !(ceiling && tempPalette[idx].ovId == "sculk_vein")) unsupported += falls[idx];
            }
            else if (oriStr == "vertical" && finalGrid.size() == (size_t)tW * tH) {
                for (int c = 0; c < tW; ++c) {
                    bool standing = false;    // what is under the bottom row is not ours to know
                    for (int r = tH - 1; r >= 0; --r) {
                        const int idx = finalGrid[(size_t)r * tW + c];
                        if (idx < 0 || idx >= np || tempPalette[idx].mc_id == "air") standing = false;
                        else if (!falls[idx]) standing = true;
                        else if (!standing) ++unsupported;
                    }
                }
            }
        }

        // overlays that old games treat another way -- counted for every mural, said only for those games
        OldGameCells oldGame;
        for (int idx : finalGrid) {
            if (idx < 0 || idx >= (int)tempPalette.size()) continue;
            const ActiveBlock& b = tempPalette[idx];
            if (b.ovId.empty()) continue;
            const bool rail = b.family == "rail", dust = b.family == "redstone";
            oldGame.rails += rail;
            oldGame.dust += dust;
            const std::string under = b.mc_id.substr(0, b.mc_id.find('['));
            if ((rail || dust || b.ovId == "vine") && under.find("shulker_box") != std::string::npos) ++oldGame.onShulker;
            if ((rail || dust) && under == "soul_sand") ++oldGame.onSoulSand;
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex);
            g_progressVal = g_progressMax.load();
            g_oldGameCells = oldGame;
            if (!warningBlocks.empty()) {
                std::string wList = "";
                for (auto it = warningBlocks.begin(); it != warningBlocks.end(); ++it) {
                    if (it != warningBlocks.begin()) wList += ", ";
                    wList += *it;
                }
                deteriorationWarning = "CAUTION: " + wList + (warningBlocks.size() == 1 ? " is " : " are ") + "used in the mural. These blocks will deteriorate.";
            }
            else {
                deteriorationWarning = "";
            }
            if (unsupported > 0) {
                if (!deteriorationWarning.empty()) deteriorationWarning += " ";
                deteriorationWarning += "CAUTION: " + std::to_string(unsupported) + (unsupported == 1 ? " block obeys" : " blocks obey")
                    + " gravity and " + (unsupported == 1 ? "has" : "have") + " nothing under " + (unsupported == 1 ? "it. It" : "them. They") + " will fall.";
            }

            // working caches committed only on a complete run
            cachedOri = oriStr; cachedDrct = drctStr; cachedBiome = biomeStr;
            cachedBgMode = currentBgMode; cachedAllowedBlocks = localAllowedBlocks;
            cachedUseOverlays = localUseOverlays; cachedAllowedOverlays = localAllowedOverlays;
            cachedPalette = paletteRef;
            activePalette = paletteRef;
            previewGridIndices = finalGrid;
            if (enhanced) { engineGridIndices = tempGridIndices; enhancedWith = enhSet; }
            else engineGridIndices.clear();
            committedRailWishes = tempRailWishes;
            if (!(enhanced && enhReused)) previewRailPlan = enhanced ? enhPlan : tempPlan;
            railNote = railPlanNote(previewRailPlan);
            previewCellCandidates = cellCandidates;    // for the next delta run
            g_committedCands = p2Cands;
            for (int i = 0; i < 6; ++i) { g_phaseMs[i] = phaseMs[i]; g_phaseUnits[i] = phaseUnits[i]; }
            previewWidth = tW;
            previewHeight = tH;
            previewSerial++;
        }
        lastRunCommitted = true;

        // said once per outcome, not on every recalculation
        static std::string lastRailNote;
        if (!railNote.empty() && railNote != lastRailNote) Log(railNote);
        lastRailNote = railNote;

        auto t_preview_end = std::chrono::high_resolution_clock::now();
        DBG_HEAP("6/6 end of calc");
        DBG_PRINT("[ENGINE] === PREVIEW CALCULATION COMPLETELY FINISHED (calcId=%d, currentCalcId now=%d) in %.2f ms ===\n",
            myCalcId, currentCalcId.load(), std::chrono::duration<double, std::milli>(t_preview_end - t_preview_start).count());
    }
    return true;
}


//////////////// RUN
void calculatePreview() {
    PreviewRun run;
    if (!run.readSettings()) return;
    if (!run.prepareImage()) return;
    if (!run.planWork()) return;
    if (!run.convertLab()) return;
    if (!run.publishStatus()) return;
    if (!run.prepareGrid()) return;
    if (!run.firstPass()) return;
    if (!run.resolveLayout()) return;
    if (!run.refine()) return;
    if (!run.enhance()) return;
    run.commit();
}


//////////////// SUPERVISOR
void initSupervisorThread() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    std::thread([]() {
        while (true) {
            std::unique_lock<std::mutex> lock(supervisorMutex);
            supervisorCv.wait(lock, [] { return pendingPreview; });
            pendingPreview = false;
            lock.unlock();

            DBG_PRINT("[SUPERVISOR] Woke up! Handing math to WorkerPool...\n");
            calculatePreview();
            DBG_PRINT("[SUPERVISOR] Math complete. Going back to sleep.\n");

            // a request that came in while this run was finishing keeps the flag up
            lock.lock();
            if (!pendingPreview) previewBusy = false;
            lock.unlock();
        }
        }).detach();
}
