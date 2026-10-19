#include "engine_internal.h"


//////////////// FILE NAMES

// a letter outside a-z in latin spelling: accents dropped, cyrillic spelled out, empty for the rest
std::string latinFor(unsigned int cp) {
    static const char* const latin1[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "", "O", "U", "U", "U", "U", "Y", "TH", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y" };
    static const char extendedA[] =
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    static const char* const cyrillic[32] = {
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya" };
    if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) return std::string(1, extendedA[cp - 0x100]);
    std::string t;
    bool upper = false;
    if (cp >= 0x410 && cp <= 0x42F) { t = cyrillic[cp - 0x410]; upper = true; }
    else if (cp >= 0x430 && cp <= 0x44F) t = cyrillic[cp - 0x430];
    else switch (cp) {
    case 0x401: t = "yo"; upper = true; break;   case 0x451: t = "yo"; break;
    case 0x404: t = "ye"; upper = true; break;   case 0x454: t = "ye"; break;
    case 0x406: t = "i"; upper = true; break;    case 0x456: t = "i"; break;
    case 0x407: t = "yi"; upper = true; break;   case 0x457: t = "yi"; break;
    case 0x40E: t = "u"; upper = true; break;    case 0x45E: t = "u"; break;
    case 0x490: t = "g"; upper = true; break;    case 0x491: t = "g"; break;
    }
    if (upper && !t.empty()) t[0] = (char)(t[0] - 'a' + 'A');
    return t;
}

// mural name as a file name, with lower set as a function name -- the game loads only a-z, 0-9 and _ - . in it
// no dot at either end, no windows device name, short enough for the run command to fit the chat box
std::string exportName(const std::string& name, bool lower) {
    std::string out;
    auto put = [&](char c) {
        char k = '_';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.') k = c;
        else if (c >= 'A' && c <= 'Z') k = lower ? (char)(c - 'A' + 'a') : c;
        if (k == '_' && !out.empty() && out.back() == '_') return;
        out += k;
        };
    for (size_t i = 0; i < name.size();) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x80) { put((char)c); ++i; continue; }
        const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        unsigned int cp = len == 4 ? (c & 0x07) : len == 3 ? (c & 0x0F) : len == 2 ? (c & 0x1F) : 0xFFFD;
        for (size_t k = 1; k < len && i + k < name.size(); ++k) cp = (cp << 6) | ((unsigned char)name[i + k] & 0x3F);
        i += len;
        const std::string t = latinFor(cp);
        if (t.empty()) {

            // soft and hard signs drop out, anything unknown leaves a gap
            if (cp != 0x42A && cp != 0x42C && cp != 0x44A && cp != 0x44C) put('_');
        }
        else for (char ch : t) put(ch);
    }
    auto trim = [&]() {
        while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
        size_t lead = 0;
        while (lead < out.size() && (out[lead] == '_' || out[lead] == '.')) ++lead;
        out.erase(0, lead);
        };
    trim();
    if (out.size() > 64) { out.resize(64); trim(); }
    if (out.empty()) out = "mural";

    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const char* const devices[] = { "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
    for (const char* d : devices) if (stem == d) { out = "mural_" + out; break; }
    return out;
}


//////////////// DATAPACK FILE
void saveDatapackZip(const std::string& muralName, const std::vector<std::string>& commands, int versionIdx) {
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    const std::string safeName = exportName(muralName, true);
    std::string zipPath = safeName + ".zip";

#ifdef __EMSCRIPTEN__
    if (!mz_zip_writer_init_heap(&zip_archive, 0, 0)) { Log("[Error] Failed to create zip."); return; }
#else
    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) { Log("[Error] Failed to create zip."); return; }
#endif

    std::string mcmeta = "{\n  \"pack\": {\n    \"pack_format\": 61,\n    \"description\": \"MMMM Generated Mural\"\n  }\n}";
    mz_zip_writer_add_mem(&zip_archive, "pack.mcmeta", mcmeta.c_str(), mcmeta.size(), MZ_DEFAULT_COMPRESSION);

    // the folder was functions until 1.21 renamed it -- with the other spelling the pack loads without its function
    const bool oldFolder = versionIdx >= 0 && versionIdx < (int)g_gameVersions.size()
        && parseVersion(g_gameVersions[versionIdx]) < parseVersion("1.21");

    // last entry below 1.21 also serves 1.21.0+ when the next one starts at a sub-version = both folders
    bool bothFolders = false;
    if (oldFolder) {
        bothFolders = true;
        if (versionIdx + 1 < (int)g_gameVersions.size()) {
            const std::string& next = g_gameVersions[versionIdx + 1];
            const auto nv = parseVersion(next);
            bothFolders = nv > parseVersion("1.21") || (nv == parseVersion("1.21") && versionPatch(next) > 0);
        }
    }
    std::vector<std::string> basePaths;
    if (oldFolder) basePaths.push_back("data/mmmm/functions/");
    if (!oldFolder || bothFolders) basePaths.push_back("data/mmmm/function/");
    int numFiles = 1;

    if (commands.size() > 65000) {    // a function runs 65536 commands at most (maxCommandChainLength)
        numFiles = (int)((commands.size() + 64999) / 65000);
        for (int i = 0; i < numFiles; ++i) {
            std::ostringstream out;
            size_t start = (size_t)i * 65000;
            size_t end = std::min<size_t>(start + 65000, commands.size());
            for (size_t j = start; j < end; ++j) {
                out << commands[j];
                if (j < end - 1) out << "\n";
            }
            std::string fileData = out.str();
            for (const std::string& basePath : basePaths) {
                std::string fileName = basePath + safeName + std::to_string(i) + ".mcfunction";
                mz_zip_writer_add_mem(&zip_archive, fileName.c_str(), fileData.c_str(), fileData.size(), MZ_DEFAULT_COMPRESSION);
            }
        }
    }
    else {
        std::ostringstream out;
        for (size_t j = 0; j < commands.size(); ++j) {
            out << commands[j];
            if (j < commands.size() - 1) out << "\n";
        }
        std::string fileData = out.str();
        for (const std::string& basePath : basePaths) {
            std::string fileName = basePath + safeName + ".mcfunction";
            mz_zip_writer_add_mem(&zip_archive, fileName.c_str(), fileData.c_str(), fileData.size(), MZ_DEFAULT_COMPRESSION);
        }
    }

#ifdef __EMSCRIPTEN__
    void* pBuf = nullptr;
    size_t sz = 0;
    mz_zip_writer_finalize_heap_archive(&zip_archive, &pBuf, &sz);

    MAIN_THREAD_EM_ASM({
        const ptr = $0 >>> 0;    // signed in wasm, negative above 2 GiB
        const size = $1;
        const name = UTF8ToString($2);

        const sharedData = new Uint8Array(HEAPU8.buffer, ptr, size);

        // a Blob wont take a view of shared memory, so a copy
        const nonSharedData = new Uint8Array(size);
        nonSharedData.set(sharedData);

        const blob = new Blob([nonSharedData], { type: "application/zip" });
        const link = document.createElement("a");
        link.href = URL.createObjectURL(blob);
        link.download = name;
        link.click();
        }, pBuf, sz, zipPath.c_str());

    mz_zip_writer_end(&zip_archive);
    Log("[SUCCESS] Datapack downloaded as " + zipPath);
#else
    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    std::string fullPath = std::filesystem::current_path().string() + "\\" + zipPath;
    Log("[SUCCESS] Datapack successfully saved as " + fullPath);
#endif

    Log("Move " + zipPath + " to .\\saves\\your_world\\datapacks and run the following commands in game:");
    Log("/reload");

    if (numFiles > 1) {
        for (int i = 0; i < numFiles; ++i) {
            Log("/execute positioned ~ ~ ~ run function mmmm:" + safeName + std::to_string(i));
        }
    }
    else {
        Log("/execute positioned ~ ~ ~ run function mmmm:" + safeName);
    }
}


//////////////// BLOCK IDS

// export only: ids as the target release spells them, palette order -- the live palette keeps the newest ids
std::vector<std::string> exportIds(const std::vector<ActiveBlock>& palette, int versionIdx) {
    std::vector<std::string> ids(palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        const std::string& id = palette[i].mc_id;
        const size_t propAt = id.find('[');
        ids[i] = versionRenames.empty() ? id
            : applyVersionRename(id.substr(0, propAt), versionIdx) + (propAt == std::string::npos ? std::string() : id.substr(propAt));
    }
    return ids;
}


//////////////// DATAPACK
void generateDatapack(const std::string& name) {

    // one export at a time + not while a preview is being made
    if (previewBusy.load() || exportBusy.exchange(true)) return;
    Log("Bundling into datapack archive...");

    getPool()->enqueue([name]() {
        int localW, localH, oIdx, dIdx, verIdx;
        PaletteRef paletteRef;
        std::vector<int> localGrid;
        RailPlan localPlan;

        {
            std::lock_guard<std::mutex> lock(dataMutex);
            paletteRef = activePalette;
            localGrid = previewGridIndices;
            localPlan = previewRailPlan;
            localW = previewWidth;
            localH = previewHeight;
            oIdx = orientationIdx;
            dIdx = directionIdx;
            verIdx = selectedVersionIdx;
        }
        if (localGrid.empty() || localGrid.size() != (size_t)localW * localH) { exportBusy = false; return; }
        const std::vector<ActiveBlock>& localPalette = *paletteRef;
        const std::vector<std::string> ids = exportIds(localPalette, verIdx);
        const int np = (int)localPalette.size();

        auto entryAt = [&](int row, int col) -> int {
            const int i = localGrid[(size_t)row * localW + col];
            return (i >= 0 && i < np) ? i : -1;
            };

        // cell to its offset from where the function runs
        auto place = [&](int row, int col, int& x, int& y, int& z) {
            x = col; y = 0; z = 0;
            if (oIdx == 0) {
                y = localH - 1 - row;
                if (dIdx == 0) x = -col;
                else if (dIdx == 2) { x = 0; z = -col; }
                else if (dIdx == 3) { x = 0; z = col; }
            }
            else {
                z = row;
                if (dIdx == 1) x = -col;
            }
            };
        auto at = [](int x, int y, int z) { return " ~" + std::to_string(x) + " ~" + std::to_string(y) + " ~" + std::to_string(z); };

        // entries that write the same block text share a number, so a run is found without comparing strings
        std::vector<int> sameAs(np, 0);
        {
            std::unordered_map<std::string, int> firstWith;
            for (int i = 0; i < np; ++i) sameAs[i] = firstWith.emplace(ids[i], i).first->second;
        }

        // order: blocks, rails + dust, other overlays -- rails + dust replay their moves, else the game reshapes them
        std::vector<std::string> overlayCommands;
        const bool planned = oIdx == 1 && dIdx == 0 && localPlan.w == localW && localPlan.h == localH;

        std::vector<std::string> finalCommands;
        if (planned && !localPlan.ops.empty()) {

            // the moves only end in the previewed shapes on a clean slate: rails, dust + levers of an earlier run go first
            const int rowsPerFill = std::max(1, 32768 / std::max(1, localW));    // /fill takes 32768 blocks at most
            for (int z = 0; z < localH; z += rowsPerFill) {
                const std::string box = " ~" + std::to_string(z) + " ~" + std::to_string(localW - 1);
                const std::string zEnd = " ~" + std::to_string(std::min(localH, z + rowsPerFill) - 1);
                finalCommands.push_back("fill ~0 ~1" + box + " ~1" + zEnd + " minecraft:air replace #minecraft:rails");
                finalCommands.push_back("fill ~0 ~1" + box + " ~1" + zEnd + " minecraft:air replace minecraft:redstone_wire");
                finalCommands.push_back("fill ~0 ~-1" + box + " ~-1" + zEnd + " minecraft:air replace minecraft:lever[face=ceiling]");
            }
        }

        // walls go bottom row first -- tested: top first, falling blocks at the end of a file fell before the next ran
        for (int k = 0; k < localH; ++k) {
            const int row = (oIdx == 0) ? localH - 1 - k : k;
            g_alive++;

            // cells of one row with the same block = one fill
            for (int col = 0; col < localW; ) {
                const int idx = entryAt(row, col);
                if (idx < 0) { ++col; continue; }
                int end = col + 1;
                while (end < localW) {
                    const int next = entryAt(row, end);
                    if (next < 0 || sameAs[next] != sameAs[idx]) break;
                    ++end;
                }
                int x, y, z, x2, y2, z2;
                place(row, col, x, y, z);
                place(row, end - 1, x2, y2, z2);
                if (end - col == 1) finalCommands.push_back("setblock" + at(x, y, z) + " minecraft:" + ids[idx]);
                else finalCommands.push_back("fill" + at(x, y, z) + at(x2, y2, z2) + " minecraft:" + ids[idx]);
                col = end;
            }
        }

        for (int row = 0; row < localH; ++row) {
            for (int col = 0; col < localW; ++col) {
                const int idx = entryAt(row, col);
                if (idx < 0) continue;
                const ActiveBlock& b = localPalette[idx];
                if (b.ovId.empty() || (planned && (b.family == "rail" || b.family == "redstone"))) continue;

                int ox, oy, oz;
                place(row, col, ox, oy, oz);
                if (oIdx == 1 && dIdx == 0) oy++;
                else if (oIdx == 1 && dIdx == 1) oy--;
                else if (oIdx == 0) {
                    if (dIdx == 0) oz--;
                    else if (dIdx == 1) oz++;
                    else if (dIdx == 2) ox++;
                    else if (dIdx == 3) ox--;
                }
                overlayCommands.push_back("setblock" + at(ox, oy, oz) + " minecraft:" + overlayBlockString(b));
            }
        }

        // vines spread sideways -- string on every side without an overlay stops it, never past the mural edge
        std::vector<char> stringAt((size_t)localW * localH, 0);
        for (int row = 0; row < localH; ++row) {
            for (int col = 0; col < localW; ++col) {
                const int idx = entryAt(row, col);
                if (idx < 0 || localPalette[idx].ovId.find("vine") == std::string::npos) continue;
                static const int DR[4] = { -1, 1, 0, 0 }, DC[4] = { 0, 0, -1, 1 };
                for (int k = 0; k < 4; ++k) {
                    const int nr = row + DR[k], nc = col + DC[k];
                    if (nr < 0 || nc < 0 || nr >= localH || nc >= localW) continue;
                    const size_t cell = (size_t)nr * localW + nc;
                    const int beside = entryAt(nr, nc);
                    if (stringAt[cell] || (beside >= 0 && !localPalette[beside].ovId.empty())) continue;
                    stringAt[cell] = 1;

                    int x = nc, y = 0, z = 0;
                    if (oIdx == 0) {
                        y = localH - 1 - nr;
                        if (dIdx == 0) { x = -nc; z = -1; }
                        else if (dIdx == 1) { x = nc; z = 1; }
                        else if (dIdx == 2) { x = 1; z = -nc; }
                        else { x = -1; z = nc; }
                    }
                    else {
                        z = nr;
                        if (dIdx == 0) { x = nc; y = 1; }
                        else { x = -nc; y = -1; }
                    }
                    overlayCommands.push_back("setblock" + at(x, y, z) + " minecraft:tripwire");
                }
            }
        }

        if (planned) {

            // floor mural: column = +x, row = +z, overlays one block up
            for (const railbuild::Op& op : localPlan.ops) {
                const bool under = op.type == railbuild::Op::LEVER_ON || op.type == railbuild::Op::LEVER_OFF;
                std::string cmd = "setblock ~" + std::to_string(op.x) + " ~" + (under ? "-1" : "1") + " ~" + std::to_string(op.z) + " minecraft:";
                switch (op.type) {
                case railbuild::Op::LEVER_ON: cmd += "lever[face=ceiling,facing=north,powered=true]"; break;
                case railbuild::Op::PLACE_RAIL:
                    cmd += std::string(RAIL_KIND_IDS[op.kind >= 1 && op.kind <= 4 ? op.kind : 1]) + (op.arg == 2 ? "[shape=east_west]" : "[shape=north_south]");
                    break;
                case railbuild::Op::PLACE_DUST:
                case railbuild::Op::SET_DUST: {
                    auto side = [&](uint8_t bit) { return (op.arg & bit) ? "side" : "none"; };
                    cmd += std::string("redstone_wire[north=") + side(railbuild::DUST_N) + ",south=" + side(railbuild::DUST_S)
                        + ",east=" + side(railbuild::DUST_E) + ",west=" + side(railbuild::DUST_W) + "]";
                    break;
                }
                default: cmd += "air"; break;
                }
                finalCommands.push_back(cmd);
            }
        }
        for (const auto& cmd : overlayCommands) finalCommands.push_back(cmd);

        saveDatapackZip(name, finalCommands, verIdx);
        exportBusy = false;
        });
}


//////////////// LITEMATICA
void generateLitematica(const std::string& name) {
    if (previewBusy.load() || exportBusy.exchange(true)) return;

    Log("Bundling into Litematica schematic...");

    getPool()->enqueue([name]() {
        int localW, localH, oIdx, dIdx, verIdx;
        PaletteRef paletteRef;
        std::vector<int> localGrid;
        RailPlan localPlan;

        {
            std::lock_guard<std::mutex> lock(dataMutex);
            if (previewGridIndices.empty() || activePalette->empty() || previewGridIndices.size() != (size_t)previewWidth * previewHeight) {
                exportBusy = false;
                return;
            }
            paletteRef = activePalette;
            localGrid = previewGridIndices;
            localPlan = previewRailPlan;
            localW = previewWidth;
            localH = previewHeight;
            oIdx = orientationIdx;
            dIdx = directionIdx;
            verIdx = selectedVersionIdx;
        }
        const std::vector<ActiveBlock>& localPalette = *paletteRef;
        const std::vector<std::string> ids = exportIds(localPalette, verIdx);
        const int np = (int)localPalette.size();

        // only what the mural uses goes into the file, air first -- bits per block follow the length of this list
        std::vector<std::string> outPalette{ "air" };
        std::unordered_map<std::string, int> slotByText{ { "air", 0 } };
        auto slotOf = [&](const std::string& text) {
            auto it = slotByText.find(text);
            if (it != slotByText.end()) return it->second;
            outPalette.push_back(text);
            return slotByText[text] = (int)outPalette.size() - 1;
            };

        // slot of the block + of the overlay per palette entry, looked up once; -1 = not asked for yet
        std::vector<int> baseSlot(np, -1), overlaySlot(np, -1);
        std::vector<char> isVine(np, 0);

        bool hasOverlays = std::any_of(localPalette.begin(), localPalette.end(),
            [](const ActiveBlock& ab) { return !ab.ovId.empty(); });
        int overlayDepth = hasOverlays ? 2 : 1;

        // some floor rails hold their shape only with a lever under their block: those get a layer below the mural
        std::vector<std::pair<int, int> > levers;    // the ones still hanging when the build is done
        if (oIdx == 1 && dIdx == 0 && localPlan.w == localW && localPlan.h == localH) {
            for (const railbuild::Op& op : localPlan.ops) {
                const std::pair<int, int> at(op.x, op.z);
                if (op.type == railbuild::Op::LEVER_ON) levers.push_back(at);
                else if (op.type == railbuild::Op::LEVER_OFF) levers.erase(std::remove(levers.begin(), levers.end(), at), levers.end());
            }
        }
        const int leverDepth = levers.empty() ? 0 : 1;

        int szX = 1, szY = 1, szZ = 1;
        if (oIdx == 0) {
            if (dIdx == 0 || dIdx == 1) { szX = localW; szY = localH; szZ = overlayDepth; }
            else { szX = overlayDepth; szY = localH; szZ = localW; }
        }
        else { szX = localW; szY = overlayDepth + leverDepth; szZ = localH; }

        int64_t totalBlocks = (int64_t)szX * szY * szZ;
        if (totalBlocks > INT_MAX) {
            Log("[Error] Schematic too large.");
            exportBusy = false;
            return;
        }

        // block indices
        std::vector<int> mappedIndices((size_t)totalBlocks, 0);    // slot per block of the region, 0 = air

        // string beside vines; asked for only where a vine is, so a mural without one has no tripwire entry
        int tripwireSlot = -1;

        for (int row = 0; row < localH; ++row) {
            for (int col = 0; col < localW; ++col) {
                const int palIdx = localGrid[(size_t)row * localW + col];
                if (palIdx < 0 || palIdx >= np) continue;
                if (col == 0) g_alive++;

                const ActiveBlock& ovSrc = localPalette[palIdx];

                int rx = 0, ry = 0, rz = 0;    // block
                int ox = 0, oy = 0, oz = 0;    // overlay

                int farSlot = hasOverlays ? 1 : 0;    // back layer of two, or the only one

                if (oIdx == 0) {
                    if (dIdx == 0) { rx = localW - 1 - col; ry = localH - 1 - row; rz = farSlot; ox = rx; oy = ry; oz = 0; }
                    else if (dIdx == 1) { rx = col; ry = localH - 1 - row; rz = 0; ox = rx; oy = ry; oz = 1; }
                    else if (dIdx == 2) { rx = 0; ry = localH - 1 - row; rz = localW - 1 - col; ox = 1; oy = ry; oz = rz; }
                    else if (dIdx == 3) { rx = farSlot; ry = localH - 1 - row; rz = col; ox = 0; oy = ry; oz = rz; }
                }
                else {
                    if (dIdx == 0) { rx = col; ry = leverDepth; rz = row; ox = rx; oy = 1 + leverDepth; oz = rz; }
                    else if (dIdx == 1) { rx = localW - 1 - col; ry = farSlot; rz = row; ox = rx; oy = 0; oz = rz; }
                }

                int64_t baseFlatIndex = ((int64_t)ry * szZ + rz) * szX + rx;
                if (baseSlot[palIdx] < 0) baseSlot[palIdx] = slotOf(ids[palIdx]);
                mappedIndices[(size_t)baseFlatIndex] = baseSlot[palIdx];

                if (!ovSrc.ovId.empty()) {
                    if (overlaySlot[palIdx] < 0) {
                        const std::string overlayId = overlayBlockString(ovSrc);
                        overlaySlot[palIdx] = slotOf(overlayId);
                        isVine[palIdx] = overlayId.find("vine") != std::string::npos ? 1 : 0;
                    }
                    int64_t ovFlatIndex = ((int64_t)oy * szZ + oz) * szX + ox;
                    mappedIndices[(size_t)ovFlatIndex] = overlaySlot[palIdx];

                    if (isVine[palIdx]) {
                        if (tripwireSlot < 0) tripwireSlot = slotOf("tripwire");
                        int dr[] = { -1, 1, 0, 0 };
                        int dc[] = { 0, 0, -1, 1 };
                        for (int i = 0; i < 4; ++i) {
                            int nr = row + dr[i];
                            int nc = col + dc[i];

                            if (nr >= 0 && nr < localH && nc >= 0 && nc < localW) {
                                int tx = 0, ty = 0, tz = 0;

                                if (oIdx == 0) {
                                    if (dIdx == 0) { tx = localW - 1 - nc; ty = localH - 1 - nr; tz = 0; }
                                    else if (dIdx == 1) { tx = nc; ty = localH - 1 - nr; tz = 1; }
                                    else if (dIdx == 2) { tx = 1; ty = localH - 1 - nr; tz = localW - 1 - nc; }
                                    else if (dIdx == 3) { tx = 0; ty = localH - 1 - nr; tz = nc; }
                                }
                                else {
                                    if (dIdx == 0) { tx = nc; ty = 1 + leverDepth; tz = nr; }
                                    else if (dIdx == 1) { tx = localW - 1 - nc; ty = 0; tz = nr; }
                                }

                                int64_t twFlatIndex = ((int64_t)ty * szZ + tz) * szX + tx;

                                // string only into empty space, a real overlay there wins in its own pass
                                if (mappedIndices[(size_t)twFlatIndex] == 0) {
                                    mappedIndices[(size_t)twFlatIndex] = tripwireSlot;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!levers.empty()) {
            const int leverPalIdx = slotOf("lever[face=ceiling,facing=north,powered=true]");
            for (const auto& xz : levers) mappedIndices[((size_t)0 * szZ + xz.second) * szX + xz.first] = leverPalIdx;
        }

        // bits per block the way litematica reads them back: enough for the highest slot, two at least
        int P = (int)outPalette.size();
        int b = 2; while ((1ULL << b) < (uint64_t)P) b++;

        int longsCount = (int)(((int64_t)totalBlocks * b + 63) / 64);
        std::vector<uint64_t> longs(longsCount, 0);

        for (int64_t i = 0; i < totalBlocks; ++i) {
            int64_t startBit = i * b;
            int longIndex = (int)(startBit / 64);
            int bitOffset = (int)(startBit % 64);

            uint64_t val = (uint64_t)(mappedIndices[(size_t)i]) & ((1ULL << b) - 1);
            longs[longIndex] |= (val << bitOffset);

            if (bitOffset + b > 64) {    // litematica lets a value run over into the next long
                longs[longIndex + 1] |= (val >> (64 - bitOffset));
            }
        }

        // schematic
        NBTWriter nbt;
        nbt.beginCompound("");

        nbt.beginCompound("Metadata");
        nbt.tagString("Author", "Moose's Minecraft Mural Maker");
        nbt.tagString("Description", "MMMM Generated Schematic");
        nbt.beginCompound("EnclosingSize");
        nbt.tagInt("x", szX); nbt.tagInt("y", szY); nbt.tagInt("z", szZ);
        nbt.tagEnd();
        nbt.tagString("Name", name);
        nbt.tagInt("RegionCount", 1);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        nbt.tagLong("TimeCreated", ms);
        nbt.tagLong("TimeModified", ms);
        nbt.tagInt("TotalBlocks", (int32_t)totalBlocks);
        nbt.tagInt("TotalVolume", (int32_t)totalBlocks);
        nbt.tagEnd();

        nbt.tagInt("MinecraftDataVersion", 3465);    // 1.20.1

        nbt.beginCompound("Regions");
        nbt.beginCompound("Unnamed");

        nbt.beginList("BlockStatePalette", 10, (int32_t)outPalette.size());

        for (const std::string& text : outPalette) {
            std::string base_id = text;
            std::map<std::string, std::string> props;
            size_t bracket = base_id.find('[');
            if (bracket != std::string::npos) {
                std::string propStr = base_id.substr(bracket + 1);
                if (!propStr.empty() && propStr.back() == ']') propStr.pop_back();
                base_id = base_id.substr(0, bracket);

                std::stringstream ss(propStr);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    size_t eq = item.find('=');
                    if (eq != std::string::npos) props[item.substr(0, eq)] = item.substr(eq + 1);
                }
            }
            if (base_id.find("minecraft:") == std::string::npos) base_id = "minecraft:" + base_id;

            nbt.tagString("Name", base_id);
            if (!props.empty()) {
                nbt.beginCompound("Properties");
                for (auto& p : props) nbt.tagString(p.first, p.second);
                nbt.tagEnd();
            }
            nbt.tagEnd();
        }

        nbt.beginLongArray("BlockStates", (int32_t)longs.size());
        for (uint64_t val : longs) nbt.writeLong((int64_t)val);

        nbt.beginList("Entities", 10, 0);
        nbt.beginList("PendingBlockTicks", 10, 0);
        nbt.beginList("PendingFluidTicks", 10, 0);

        nbt.beginCompound("Position");
        nbt.tagInt("x", 0); nbt.tagInt("y", 0); nbt.tagInt("z", 0);
        nbt.tagEnd();

        nbt.beginCompound("Size");
        nbt.tagInt("x", szX); nbt.tagInt("y", szY); nbt.tagInt("z", szZ);
        nbt.tagEnd();

        nbt.beginList("TileEntities", 10, 0);
        nbt.tagEnd();
        nbt.tagEnd();

        nbt.tagInt("SubVersion", 1);
        nbt.tagInt("Version", 6);
        nbt.tagEnd();

        std::vector<uint8_t> nbtData = nbt.buf;

        mz_ulong zlen = (mz_ulong)(nbtData.size() * 1.1 + 1024);    // room for data that wont shrink
        std::vector<uint8_t> zlibData(zlen);
        if (mz_compress(zlibData.data(), &zlen, nbtData.data(), (mz_ulong)nbtData.size()) != MZ_OK) {
            Log("[Error] Failed to compress Litematica data.");
            exportBusy = false;
            return;
        }

        // zlib to gzip: 2-byte header + 4-byte adler off, gzip header, crc + size on
        size_t rawDeflateLen = zlen - 6;

        std::vector<uint8_t> gzData;
        uint8_t header[10] = { 0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF };
        gzData.insert(gzData.end(), header, header + 10);
        gzData.insert(gzData.end(), zlibData.begin() + 2, zlibData.begin() + 2 + rawDeflateLen);

        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, nbtData.data(), nbtData.size());
        uint32_t isize = (uint32_t)nbtData.size();

        gzData.push_back(crc & 0xFF); gzData.push_back((crc >> 8) & 0xFF);
        gzData.push_back((crc >> 16) & 0xFF); gzData.push_back((crc >> 24) & 0xFF);
        gzData.push_back(isize & 0xFF); gzData.push_back((isize >> 8) & 0xFF);
        gzData.push_back((isize >> 16) & 0xFF); gzData.push_back((isize >> 24) & 0xFF);

        const std::string safeName = exportName(name, false);

#ifdef __EMSCRIPTEN__
        MAIN_THREAD_EM_ASM({
            const ptr = $0 >>> 0;    // signed in wasm, negative above 2 GiB
            const size = $1;
            const filename = UTF8ToString($2);
            const sharedData = new Uint8Array(HEAPU8.buffer, ptr, size);
            const nonSharedData = new Uint8Array(size);
            nonSharedData.set(sharedData);

            const blob = new Blob([nonSharedData], { type: "application/octet-stream" });
            const link = document.createElement("a");
            link.href = URL.createObjectURL(blob);
            link.download = filename;
            link.click();
            }, gzData.data(), gzData.size(), (safeName + ".litematic").c_str());

        Log("[SUCCESS] Litematica schematic is downloaded as " + safeName + ".litematic");
        Log("Move it to .\\schematics directory and load with Litematica.");
#else
        std::string filepath = safeName + ".litematic";
        std::ofstream out(filepath, std::ios::binary);
        out.write((char*)gzData.data(), gzData.size());
        out.close();

        Log("[SUCCESS] Litematica schematic is successfully saved as " + filepath);
        Log("Move it to .\\schematics folder and load via the Litematica mod!");
#endif
        exportBusy = false;
        });
}
