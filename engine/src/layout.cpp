#include "engine_internal.h"


//////////////// BLOCK RULES

// tested: lever under the block powers the rail on top? asked the game for every palette block, these said no
// a block missing here costs a wrong rail shape, so ask again when blocks are added
bool leverPowersThrough(const std::string& id) {
    static const char* const parts[] = { "glass", "_leaves", "copper_bulb", "copper_grate", "piston", "_trapdoor" };
    for (const char* p : parts) if (id.find(p) != std::string::npos) return false;
    static const std::set<std::string> no = {
        "glowstone", "sea_lantern", "ice", "observer", "composter", "honey_block", "powder_snow",
        "daylight_detector", "dirt_path", "farmland", "cauldron", "hopper", "beacon", "air",
        "stonecutter", "end_portal_frame", "mangrove_roots"
    };
    return no.count(id) == 0;
}

// lights up, fires, extends or goes off when powered: no lever under or next to one
bool reactsToPower(const std::string& id) {
    static const char* const parts[] = { "copper_bulb", "piston", "command_block", "_trapdoor" };
    for (const char* p : parts) if (id.find(p) != std::string::npos) return true;
    static const std::set<std::string> yes = { "tnt", "redstone_lamp", "dispenser", "dropper", "crafter", "note_block" };
    return yes.count(id) != 0;
}

bool fallsDown(const std::string& id) {
    for (const BlockData& bd : masterDatabase) if (bd.id == id) return bd.obeysGravity;
    return false;
}

// may this overlay sit on top (overlay_ids.csv), leaves + trapdoors by family
bool overlayFitsOnTop(const std::string& rawId, const char* overlayId) {
    auto it = overlayMatrixTop.find(rawId);
    if (it == overlayMatrixTop.end() && rawId.find("_leaves") != std::string::npos) it = overlayMatrixTop.find("LEAVES");
    if (it == overlayMatrixTop.end() && rawId.find("_trapdoor") != std::string::npos) it = overlayMatrixTop.find("TRAPDOORS");
    if (it == overlayMatrixTop.end()) it = overlayMatrixTop.find("OTHER_BLOCKS");
    if (it == overlayMatrixTop.end()) return false;
    auto col = it->second.find(overlayId);
    return col != it->second.end() && col->second;
}

uint8_t railKindOf(const std::string& ovId) {
    for (uint8_t k = 1; k < 6; ++k) if (ovId == RAIL_KIND_IDS[k]) return k;
    return railbuild::NONE;
}

//////// side masks and rail shapes
uint8_t railShapeOfMask(int m) {
    switch (m & 15) {
    case OV_N | OV_S: return 1;
    case OV_E | OV_W: return 2;
    case OV_S | OV_E: return 3;
    case OV_S | OV_W: return 4;
    case OV_N | OV_W: return 5;
    case OV_N | OV_E: return 6;
    }
    return 0;
}
int maskOfRailShape(uint8_t s) {
    static const int M[7] = { 0, OV_N | OV_S, OV_E | OV_W, OV_S | OV_E, OV_S | OV_W, OV_N | OV_W, OV_N | OV_E };
    return s <= 6 ? M[s] : 0;
}
uint8_t dustOfMask(int m) {
    return (uint8_t)(((m & OV_N) ? railbuild::DUST_N : 0) | ((m & OV_S) ? railbuild::DUST_S : 0)
        | ((m & OV_W) ? railbuild::DUST_W : 0) | ((m & OV_E) ? railbuild::DUST_E : 0));
}
int maskOfDust(uint8_t d) {
    return ((d & railbuild::DUST_N) ? OV_N : 0) | ((d & railbuild::DUST_S) ? OV_S : 0)
        | ((d & railbuild::DUST_W) ? OV_W : 0) | ((d & railbuild::DUST_E) ? OV_E : 0);
}


//////////////// RAILS AND DUST
OverlayVariantIndex buildOverlayVariantIndex(const std::vector<ActiveBlock>& palette) {
    OverlayVariantIndex index;
    index.kind.assign(palette.size(), railbuild::NONE);
    index.base.assign(palette.size(), 0);
    std::unordered_map<std::string, uint8_t> baseOf;    // by bare block id, rotations of a block share it

    for (size_t i = 0; i < palette.size(); ++i) {
        const ActiveBlock& ab = palette[i];

        const std::string rawId = ab.mc_id.substr(0, ab.mc_id.find('['));
        auto known = baseOf.find(rawId);
        if (known == baseOf.end()) {
            uint8_t f = 0;
            if (rawId != "air") {
                if (overlayFitsOnTop(rawId, "rail")) f |= BASE_HOLDS_RAIL;

                // a falling block cant get a lever under it by hand: it drops into the gap first
                if (leverPowersThrough(rawId) && !fallsDown(rawId)) f |= BASE_CONDUCTS;
                if (rawId == "redstone_block") f |= BASE_POWERED;
                if (reactsToPower(rawId)) f |= BASE_REACTS;
            }
            known = baseOf.emplace(rawId, f).first;
        }
        index.base[i] = known->second;

        if (ab.ovMask < 0 || ab.family == "none") continue;
        if (ab.family == "rail" || ab.family == "redstone") {
            index.kind[i] = railKindOf(ab.ovId);
            if (index.kind[i] != railbuild::NONE) index.any = true;
        }

        std::string key = ab.mc_id + "\x1f" + ab.ovId;
        auto it = index.slots.find(key);
        if (it == index.slots.end()) {
            std::array<int, 16> slots;
            slots.fill(-1);
            slots[ab.ovMask & 15] = (int)i;
            index.slots.emplace(key, slots);
        }
        else if (it->second[ab.ovMask & 15] < 0) {
            it->second[ab.ovMask & 15] = (int)i;
        }
    }
    return index;
}

// the game picks rail + dust shapes itself, by placing order + what stood next to them -- the solver only wishes
// railbuild lays the layer move by move; its end state goes back into the grid, its moves into the datapack
void resolveOverlayGeometry(std::vector<int>& grid, int tW, int tH,
    const std::vector<ActiveBlock>& palette, const OverlayVariantIndex& index,
    RailWishes& wishes, RailPlan& plan) {

    plan = RailPlan();
    const size_t cells = (size_t)std::max(tW, 0) * (size_t)std::max(tH, 0);
    if (cells == 0 || grid.size() != cells) return;
    if (wishes.size() != cells) wishes.assign(cells, 0);
    if (!index.any) return;

    const int np = (int)palette.size();

    // every shape the game can leave this overlay in has a palette entry on this block
    auto drawable = [&](const std::string& mcId, uint8_t k) -> const std::array<int, 16>* {
        auto it = index.slots.find(mcId + "\x1f" + RAIL_KIND_IDS[k]);
        if (it == index.slots.end()) return nullptr;
        const std::array<int, 16>& s = it->second;
        if (k == railbuild::WIRE) {
            for (int m = 0; m < 16; ++m) if (m != 1 && m != 2 && m != 4 && m != 8 && s[m] < 0) return nullptr;
            return &s;
        }
        for (uint8_t sh = 1; sh <= (k == railbuild::RAIL ? 6 : 2); ++sh) if (s[maskOfRailShape(sh)] < 0) return nullptr;
        return &s;
        };

    std::vector<uint8_t> kind(cells, 0), shape(cells, 0), flags(cells, 0);
    bool anyCell = false;
    for (size_t c = 0; c < cells; ++c) {
        const int idx = grid[c];
        if (idx < 0 || idx >= np) { wishes[c] = 0; continue; }
        const ActiveBlock& ab = palette[idx];

        // wishes per cell: 0x8000 recorded, 0x4000 left out, kind << 8, wished mask << 4, built mask
        const uint16_t rec = wishes[c];
        uint8_t k = index.kind[idx];
        int wish = -1;
        if (k != railbuild::NONE) {
            wish = ab.ovMask & 15;

            // still shows what was built last time: keep the shape it was meant to have
            if ((rec & 0xC000) == 0x8000 && ((rec >> 8) & 7) == k && (rec & 15) == wish) wish = (rec >> 4) & 15;
            if (!drawable(ab.mc_id, k)) {

                // overlay_states.csv doesnt list every shape: nothing to build with
                grid[c] = ab.ovBaseIdx >= 0 ? ab.ovBaseIdx : idx;
                k = railbuild::NONE;
            }
        }
        else if ((rec & 0xC000) == 0xC000 && ab.ovMask < 0) {

            // left out last time, still wished for
            k = (uint8_t)((rec >> 8) & 7);
            wish = (rec >> 4) & 15;
            if (k < 1 || k > 5 || !drawable(ab.mc_id, k)) k = railbuild::NONE;
        }
        if (k == railbuild::NONE) { wishes[c] = 0; continue; }
        kind[c] = k;
        shape[c] = (k == railbuild::WIRE) ? dustOfMask(wish) : railShapeOfMask(wish);
        anyCell = true;
    }
    if (!anyCell) return;

    for (int r = 0; r < tH; ++r) {
        for (int c = 0; c < tW; ++c) {
            const size_t cell = (size_t)r * tW + c;
            auto baseAt = [&](int rr, int cc) -> uint8_t {
                if (rr < 0 || cc < 0 || rr >= tH || cc >= tW) return 0;
                const int idx = grid[(size_t)rr * tW + cc];
                return (idx < 0 || idx >= np) ? (uint8_t)0 : index.base[idx];
                };
            const uint8_t b = baseAt(r, c);
            uint8_t f = 0;
            if (b & BASE_HOLDS_RAIL) f |= railbuild::Input::HOLDS_RAIL;
            if (b & BASE_POWERED) f |= railbuild::Input::POWERED;
            const uint8_t around = baseAt(r - 1, c) | baseAt(r + 1, c) | baseAt(r, c - 1) | baseAt(r, c + 1);
            if ((b & BASE_CONDUCTS) && !(b & (BASE_POWERED | BASE_REACTS)) && !(around & BASE_REACTS)) f |= railbuild::Input::LEVER_WORKS;
            flags[cell] = f;
        }
    }

    // same layer comes back several times per calculation + on every recalculation that leaves it alone
    static std::mutex lastMutex;
    static int lastW = 0, lastH = 0;
    static std::vector<uint8_t> lastKind, lastShape, lastFlags;
    static railbuild::Result last;
    railbuild::Result res;
    {
        std::lock_guard<std::mutex> lock(lastMutex);
        if (lastW != tW || lastH != tH || lastKind != kind || lastShape != shape || lastFlags != flags) {
            railbuild::Input in;
            in.w = tW; in.h = tH;
            in.kind = kind.data(); in.shape = shape.data(); in.flags = flags.data();
            in.alive = [] { g_alive++; };
            railbuild::build(in, last);
            lastW = tW; lastH = tH;
            lastKind = kind; lastShape = shape; lastFlags = flags;
        }
        res = last;
    }

    for (size_t c = 0; c < cells; ++c) {
        const uint8_t k = kind[c];
        if (k == railbuild::NONE) continue;
        const int idx = grid[c];
        const ActiveBlock& ab = palette[idx];
        const int bare = (ab.ovMask >= 0 && ab.ovBaseIdx >= 0 && ab.ovBaseIdx < np) ? ab.ovBaseIdx : idx;
        const int wish = (k == railbuild::WIRE) ? maskOfDust(shape[c]) : maskOfRailShape(shape[c]);
        const uint16_t record = (uint16_t)(0x8000 | (k << 8) | (wish << 4));

        int replacement = -1, built = 0;
        if (res.kind[c] == k) {
            built = (k == railbuild::WIRE) ? maskOfDust(res.shape[c]) : maskOfRailShape(res.shape[c]);
            if (const std::array<int, 16>* s = drawable(ab.mc_id, k)) replacement = (*s)[built & 15];
        }
        if (replacement >= 0) { grid[c] = replacement; wishes[c] = (uint16_t)(record | built); }
        else { grid[c] = bare; wishes[c] = (uint16_t)(record | 0x4000); }
    }

    plan.w = tW; plan.h = tH;
    plan.wishKind = std::move(kind); plan.wishShape = std::move(shape); plan.flags = std::move(flags);
    plan.ops = std::move(res.ops);
    plan.rails = res.rails; plan.dust = res.dust; plan.reshaped = res.reshaped; plan.dropped = res.dropped;
    plan.helpers = res.helpers; plan.levers = res.levers; plan.leverVisits = res.leverVisits; plan.mismatched = res.mismatched;
}


//////////////// POWER AND GOLEMS
std::vector<uint8_t> powerClasses(const std::vector<ActiveBlock>& palette) {
    std::vector<uint8_t> cls(palette.size(), 0);
    for (size_t i = 0; i < palette.size(); ++i) {
        const std::string id = palette[i].mc_id.substr(0, palette[i].mc_id.find('['));

        // tested: a bulb beside a redstone block or in reach of an observer pulse comes out lit, a crafter triggered
        if (id == "piston" || id == "sticky_piston" || id == "tnt" || id == "crafter" || id == "redstone_lamp"
            || id.find("copper_bulb") != std::string::npos || id.find("_trapdoor") != std::string::npos) cls[i] = PWR_VICTIM;
        else if (id == "observer") cls[i] = PWR_OBSERVER;
        else if (id == "redstone_block" || id == "sculk_sensor" || id == "calibrated_sculk_sensor" || id == "daylight_detector") cls[i] = PWR_SOURCE;

        // tested: pumpkin or lantern put down last makes a golem + takes the blocks with it, any direction
        else if (id == "carved_pumpkin" || id == "jack_o_lantern") cls[i] = GOLEM_HEAD;
        else if (id == "snow_block") cls[i] = GOLEM_SNOW;
        else if (id == "iron_block") cls[i] = GOLEM_IRON;
        else {

            // tested: copper golems take plain copper blocks in any stage, waxed or not -- cut, chiseled, grate, bulb dont
            const std::string plain = id.rfind("waxed_", 0) == 0 ? id.substr(6) : id;
            if (plain == "copper_block" || plain == "exposed_copper" || plain == "weathered_copper" || plain == "oxidized_copper") cls[i] = GOLEM_COPPER;
        }
    }
    return cls;
}

// a block power would change, or a pumpkin that would make a golem, gives way to the next best block
int forbidPoweredBlocks(std::vector<int>& grid, int tW, int tH, const std::vector<ActiveBlock>& palette,
    const std::vector<uint8_t>& cls, const std::vector<int>& cands, int stride) {

    const int np = (int)palette.size();
    if (grid.size() != (size_t)tW * tH || cls.size() != palette.size()) return 0;
    auto at = [&](int r, int c) -> uint8_t {
        if (r < 0 || c < 0 || r >= tH || c >= tW) return 0;
        const int i = grid[(size_t)r * tW + c];
        return (i < 0 || i >= np) ? (uint8_t)0 : cls[i];
        };
    auto airAt = [&](int r, int c) -> bool {
        if (r < 0 || c < 0 || r >= tH || c >= tW) return false;
        const int i = grid[(size_t)r * tW + c];
        return i >= 0 && i < np && palette[i].mc_id == "air";
        };

    int swapped = 0;
    for (int r = 0; r < tH; ++r) {
        for (int c = 0; c < tW; ++c) {
            const size_t cell = (size_t)r * tW + c;
            const int idx = grid[cell];
            if (idx < 0 || idx >= np || !(cls[idx] & (PWR_VICTIM | GOLEM_HEAD))) continue;

            bool powered = false;
            if (cls[idx] & GOLEM_HEAD) {

                // tested: copper = one block beside it, snow = two in a line, iron = the T of four with air in its corners
                static const int DR[4] = { -1, 1, 0, 0 }, DC[4] = { 0, 0, -1, 1 };
                for (int d = 0; d < 4 && !powered; ++d) {
                    const int r1 = r + DR[d], c1 = c + DC[d], r2 = r1 + DR[d], c2 = c1 + DC[d];
                    const int sr = DC[d], sc = DR[d];    // across the body
                    const uint8_t body = at(r1, c1);
                    if (body & GOLEM_COPPER) powered = true;
                    else if (body & GOLEM_SNOW) powered = (at(r2, c2) & GOLEM_SNOW) != 0;
                    else if (body & GOLEM_IRON) {
                        powered = (at(r2, c2) & GOLEM_IRON) && (at(r1 + sr, c1 + sc) & GOLEM_IRON) && (at(r1 - sr, c1 - sc) & GOLEM_IRON)
                            && airAt(r + sr, c + sc) && airAt(r - sr, c - sc) && airAt(r2 + sr, c2 + sc) && airAt(r2 - sr, c2 - sc);
                    }
                }
            }
            for (int dr = -2; dr <= 2 && !powered && (cls[idx] & PWR_VICTIM); ++dr) {
                for (int dc = -2; dc <= 2 && !powered; ++dc) {
                    const int d = std::abs(dr) + std::abs(dc);
                    if (d == 0 || d > 2) continue;
                    const uint8_t u = at(r + dr, c + dc);

                    // tested: observer set by command pulses once + powers the block behind it = reach of 2 cells
                    powered = (u & PWR_OBSERVER) || (d == 1 && (u & PWR_SOURCE));
                }
            }
            if (!powered) continue;

            int pick = -1;
            if (stride > 0 && cands.size() == grid.size() * (size_t)stride) {
                for (int k = 0; k < stride && pick < 0; ++k) {
                    const int cand = cands[cell * stride + k];
                    if (cand >= 0 && cand < np && !cls[cand]) pick = cand;
                }
            }
            if (pick < 0) {

                // nothing usable among its candidates: the plain block closest in colour
                const ActiveBlock& was = palette[(palette[idx].ovMask >= 0 && palette[idx].ovBaseIdx >= 0) ? palette[idx].ovBaseIdx : idx];
                float best = 1e30f;
                for (int i = 0; i < np; ++i) {
                    const ActiveBlock& ab = palette[i];
                    if (cls[i] || ab.ovMask >= 0 || ab.mc_id == "air" || ab.flatLab.empty()) continue;
                    const float dl = ab.meanLab.l - was.meanLab.l, da = ab.meanLab.a - was.meanLab.a, db = ab.meanLab.b - was.meanLab.b;
                    const float dist = dl * dl + da * da + db * db;
                    if (dist < best) { best = dist; pick = i; }
                }
            }
            if (pick >= 0) { grid[cell] = pick; ++swapped; }
        }
    }
    return swapped;
}

// only the self-check speaks: hand build and datapack must give the same cells
std::string railPlanNote(const RailPlan& p) {
    if (!p.mismatched) return "";
    return "[Error] Rails and dust: " + std::to_string(p.mismatched) + (p.mismatched == 1 ? " cell differs" : " cells differ")
        + " between a hand build and the datapack.";
}


//////////////// BLOCKSTATE TEXT
std::string overlayStateForMask(const ActiveBlock& b) {
    if (b.family == "redstone") {
        auto side = [&](int bit) { return (b.ovMask & bit) ? "side" : "none"; };
        return std::string("north=") + side(OV_N) + ",south=" + side(OV_S)
            + ",east=" + side(OV_E) + ",west=" + side(OV_W);
    }
    if (b.family == "rail") {
        int m = b.ovMask & 15;
        if (m == (OV_E | OV_W)) return "shape=east_west";
        if (m == (OV_S | OV_E)) return "shape=south_east";
        if (m == (OV_S | OV_W)) return "shape=south_west";
        if (m == (OV_N | OV_W)) return "shape=north_west";
        if (m == (OV_N | OV_E)) return "shape=north_east";
        return "shape=north_south";
    }
    return b.ovState;
}

std::string overlayBlockString(const ActiveBlock& b) {
    std::string state = overlayStateForMask(b);
    if (state.empty()) return b.ovId;
    return b.ovId + "[" + state + "]";
}
