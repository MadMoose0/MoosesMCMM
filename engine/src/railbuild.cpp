#include "railbuild.h"

#include <algorithm>

namespace railbuild {

namespace {

//////////////// DIRECTIONS
enum { N = 0, S = 1, W = 2, E = 3 };
const int DX[4] = { 0, 0, -1, 1 };
const int DZ[4] = { -1, 1, 0, 0 };
const int OPP[4] = { S, N, E, W };

// two ends of each shape, in the order the game lists them
const int ENDS[7][2] = { { -1, -1 }, { N, S }, { W, E }, { E, S }, { W, S }, { W, N }, { E, N } };
const int UPDATE_ORDER[4] = { W, E, N, S };    // neighbour updates (down + up never reach this layer)
const int HORIZONTAL[4] = { N, E, S, W };    // game order: Direction.Plane.HORIZONTAL
const int VALUES[4] = { N, S, W, E };    // game order: Direction.values() without down + up

inline bool pointsAt(uint8_t shape, int d) { return shape >= 1 && shape <= 6 && (ENDS[shape][0] == d || ENDS[shape][1] == d); }
inline int bits(uint8_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

// dust from its real connections: a lone axis grows into a line, nothing at all = a cross
inline uint8_t fill(uint8_t actual) {
    uint8_t out = actual;
    if (!(actual & (DUST_N | DUST_S))) out |= DUST_W | DUST_E;
    if (!(actual & (DUST_W | DUST_E))) out |= DUST_N | DUST_S;
    return out;
}

inline bool signalSource(uint8_t k) { return k == DETECTOR || k == WIRE; }

}    // namespace


//////////////// WORLD
World::World(int w_, int h_, Mode mode_) : w(w_), h(h_), mode(mode_),
    kind((size_t)w_ * h_, NONE), shape((size_t)w_ * h_, 0), lever((size_t)w_ * h_, 0) {}

void World::write(int x, int z, uint8_t k, uint8_t s) {
    const int i = z * w + x;
    journal.push_back({ i, kind[i], shape[i], lever[i] });
    kind[i] = k; shape[i] = s;
}

void World::rollback(size_t to) {
    while (journal.size() > to) {
        const Undo& u = journal.back();
        kind[u.idx] = u.kind; shape[u.idx] = u.shape; lever[u.idx] = u.lever;
        journal.pop_back();
    }
}

//////// rails
uint8_t World::realEnds(int x, int z) const {
    uint8_t m = 0;
    const uint8_t sh = shapeAt(x, z);
    for (int k = 0; k < 2; ++k) {
        const int d = ENDS[sh][k];
        const int qx = x + DX[d], qz = z + DZ[d];
        if (isRail(qx, qz) && pointsAt(shapeAt(qx, qz), OPP[d])) m |= (uint8_t)(1 << d);
    }
    return m;
}

// takes a connection from side d unless its two ends are already met elsewhere
bool World::canConnect(int x, int z, int d) const {
    const uint8_t r = realEnds(x, z);
    return ((r >> d) & 1) || bits(r) != 2;
}

// straight types tell their neighbours twice (removal hook of the old state, then the usual round)
void World::setRail(int x, int z, uint8_t sh) {
    const int i = z * w + x;
    if (shape[i] == sh) return;
    const uint8_t k = kind[i];
    write(x, z, k, sh);
    if (k != RAIL) notify(x, z, k);
    notify(x, z, k);
    dustAround(x, z);
}

void World::connect(int x, int z, int d) {
    const uint8_t m = (uint8_t)(realEnds(x, z) | (1 << d));
    const bool n = m & (1 << N), s = m & (1 << S), w_ = m & (1 << W), e = m & (1 << E);
    uint8_t sh = 0;
    if (n || s) sh = 1;
    if (w_ || e) sh = 2;
    if (kindAt(x, z) == RAIL) {
        if (s && e && !n && !w_) sh = 3;
        if (s && w_ && !n && !e) sh = 4;
        if (n && w_ && !s && !e) sh = 5;
        if (n && e && !s && !w_) sh = 6;
    }
    setRail(x, z, sh ? sh : (uint8_t)1);
}

// game RailState.place: shape for the rail here, standing as handed, and pulls in what its ends point at
void World::decide(int x, int z, uint8_t handed, bool always) {
    const bool curved = kindAt(x, z) == RAIL;
    const bool powered = leverAt(x, z);
    auto has = [&](int d) {
        const int qx = x + DX[d], qz = z + DZ[d];
        return isRail(qx, qz) && canConnect(qx, qz, OPP[d]);
        };
    const bool n = has(N), s = has(S), w_ = has(W), e = has(E);
    const bool ns = n || s, we = w_ || e;
    uint8_t sh = 0;
    if (ns && !we) sh = 1;
    if (we && !ns) sh = 2;
    const bool se = s && e, sw = s && w_, nw = n && w_, ne = n && e;
    if (curved) {
        if (se && !n && !w_) sh = 3;
        if (sw && !n && !e) sh = 4;
        if (nw && !s && !e) sh = 5;
        if (ne && !s && !w_) sh = 6;
    }
    if (!sh) {
        if (ns && we) sh = handed;
        else if (ns) sh = 1;
        else if (we) sh = 2;
        if (curved) {

            // the last match wins: south-east unpowered, north-west with a signal
            if (powered) { if (se) sh = 3; if (sw) sh = 4; if (ne) sh = 6; if (nw) sh = 5; }
            else { if (nw) sh = 5; if (ne) sh = 6; if (sw) sh = 4; if (se) sh = 3; }
        }
    }
    if (!sh) sh = handed;
    const bool changed = sh != shapeAt(x, z);
    setRail(x, z, sh);
    if (always || changed) {
        for (int k = 0; k < 2; ++k) {
            const int d = ENDS[sh][k];
            const int qx = x + DX[d], qz = z + DZ[d];
            if (isRail(qx, qz) && canConnect(qx, qz, OPP[d])) connect(qx, qz, OPP[d]);
        }
    }
}

//////// neighbour updates

// only a signal source makes a rail look again, and only a plain rail with exactly three rails around
void World::notify(int cx, int cz, uint8_t source) {
    if (!signalSource(source)) return;
    if (depth) layer.push_back({ false, cx, cz });    // raised inside another update: runs when that handler returns
    else run(false, cx, cz);
}

void World::notifyCell(int x, int z) {
    if (depth) layer.push_back({ true, x, z });
    else run(true, x, z);
}

void World::run(bool single, int x, int z) {
    ++depth;
    for (int k = 0; k < (single ? 1 : 4); ++k) {
        const int qx = single ? x : x + DX[UPDATE_ORDER[k]], qz = single ? z : z + DZ[UPDATE_ORDER[k]];
        if (kindAt(qx, qz) == RAIL) {
            int around = 0;
            for (int d = 0; d < 4; ++d) around += isRail(qx + DX[d], qz + DZ[d]);
            if (around == 3) decide(qx, qz, shapeAt(qx, qz), false);
        }
        if (!layer.empty()) {
            std::vector<Pending> added;
            added.swap(layer);
            for (const Pending& p : added) run(p.single, p.x, p.z);
        }
    }
    --depth;
}

//////// dust
uint8_t World::actual(int x, int z) const {
    uint8_t m = 0;
    for (int d = 0; d < 4; ++d) if (connectable(x + DX[d], z + DZ[d])) m |= (uint8_t)(1 << d);
    return m;
}

// dust beside the cell looks again at the side facing it: nothing new there and not a cross = it stays
// otherwise it re-decides from its real connections, then its own dust neighbours look in turn
void World::dustAround(int x, int z) {
    for (int k = 0; k < 4; ++k) {
        const int d = UPDATE_ORDER[k];
        const int wx = x + DX[d], wz = z + DZ[d];
        if (kindAt(wx, wz) != WIRE) continue;
        const uint8_t cur = shapeAt(wx, wz);
        const bool had = (cur >> OPP[d]) & 1;
        if (connectable(x, z) == had && cur != DUST_ALL) continue;
        const uint8_t now = fill(actual(wx, wz));
        if (now != cur) {
            write(wx, wz, WIRE, now);
            dustAround(wx, wz);
        }
    }
}

// game updateNeighborsOfNeighboringWires: every dust beside the cell tells its own surroundings
void World::wiresNextTo(int x, int z) {
    for (int k = 0; k < 4; ++k) {
        const int wx = x + DX[HORIZONTAL[k]], wz = z + DZ[HORIZONTAL[k]];
        if (kindAt(wx, wz) != WIRE) continue;
        notify(wx, wz, WIRE);
        for (int j = 0; j < 4; ++j) notify(wx + DX[VALUES[j]], wz + DZ[VALUES[j]], WIRE);
    }
}

//////// moves
void World::placeRail(int x, int z, uint8_t k, uint8_t handed) {
    write(x, z, k, handed);
    decide(x, z, handed, true);
    dustAround(x, z);
    if (mode == COMMAND) notify(x, z, k);
}

void World::placeWire(int x, int z, int given) {
    const uint8_t a = actual(x, z);
    uint8_t m;
    if (mode == HAND) m = fill(a);
    else if (given < 0) m = a ? fill(a) : (uint8_t)0;
    else m = (uint8_t)given;
    write(x, z, WIRE, m);
    wiresNextTo(x, z);
    dustAround(x, z);
    if (mode == COMMAND) notify(x, z, WIRE);
}

void World::setWire(int x, int z, uint8_t mask) {
    write(x, z, WIRE, mask);
    notify(x, z, WIRE);
}

void World::remove(int x, int z) {
    const uint8_t k = kindAt(x, z);
    write(x, z, NONE, 0);
    auto removalHook = [&]() {
        if (isRailKind(k) && k != RAIL) notify(x, z, k);
        if (k == WIRE) {
            for (int j = 0; j < 4; ++j) notify(x + DX[VALUES[j]], z + DZ[VALUES[j]], WIRE);
            wiresNextTo(x, z);
        }
        };
    if (mode == HAND) { removalHook(); notify(x, z, k); dustAround(x, z); }
    else { dustAround(x, z); removalHook(); }
}

void World::setLever(int x, int z, bool on) {
    const int i = z * w + x;
    if ((lever[i] != 0) == on) return;
    journal.push_back({ i, kind[i], shape[i], lever[i] });
    lever[i] = on ? 1 : 0;
    if (mode == HAND) notifyCell(x, z);    // a flip makes the rail above look again, a command sets it silently
}

void World::takeLever(int x, int z) {
    const int i = z * w + x;
    if (!lever[i]) return;
    journal.push_back({ i, kind[i], shape[i], lever[i] });
    lever[i] = 0;
    notifyCell(x, z);
}

namespace {

//////////////// BUILD
const uint8_t TEMPORARY = 0x80;    // on Op::kind while building: a rail that comes out again

struct Builder {
    const Input& in;
    Result& out;
    const int w, h;
    World H, C;    // the same moves by hand + by command
    std::vector<Op> trial;    // moves since the last commit, in step with the journals
    std::vector<int> temps, pins;    // cells with a temporary rail right now: helpers of the rail being stood + guards
    std::vector<int> seen;    // scratch for scoring
    long tried = 0, limit = 0;    // options played + where the second look stops

    Builder(const Input& i, Result& o) : in(i), out(o), w(i.w), h(i.h), H(i.w, i.h, HAND), C(i.w, i.h, COMMAND) {}

    void doOp(const Op& op) {
        if (in.alive) in.alive();
        const uint8_t k = op.kind & (uint8_t)~TEMPORARY;
        switch (op.type) {
        case Op::LEVER_ON: H.setLever(op.x, op.z, true); C.setLever(op.x, op.z, true); break;
        case Op::LEVER_OFF: H.takeLever(op.x, op.z); C.takeLever(op.x, op.z); break;
        case Op::PLACE_RAIL: H.placeRail(op.x, op.z, k, op.arg); C.placeRail(op.x, op.z, k, op.arg); break;
        case Op::PLACE_DUST: H.placeWire(op.x, op.z, -1); C.placeWire(op.x, op.z, H.shapeAt(op.x, op.z)); break;
        case Op::SET_DUST: H.setWire(op.x, op.z, op.arg); C.setWire(op.x, op.z, op.arg); break;
        case Op::BREAK: H.remove(op.x, op.z); C.remove(op.x, op.z); break;
        }
        trial.push_back(op);
    }

    // a point to come back to: both worlds + the move list
    struct Mark { size_t h, c, t; };
    Mark mark() const { return { H.mark(), C.mark(), trial.size() }; }
    void rollback(const Mark& m) { H.rollback(m.h); C.rollback(m.c); trial.resize(m.t); }
    void commit() {
        out.ops.insert(out.ops.end(), trial.begin(), trial.end());
        trial.clear(); H.forget(); C.forget();
    }

    bool differs(int i) const { return H.kind[i] != C.kind[i] || H.shape[i] != C.shape[i] || H.lever[i] != C.lever[i]; }

    // hand and command agree on every cell either touched since the mark
    bool same(const Mark& m) const {
        const std::vector<World::Undo>& hc = H.changes();
        const std::vector<World::Undo>& cc = C.changes();
        for (size_t j = m.h; j < hc.size(); ++j) if (differs(hc[j].idx)) return false;
        for (size_t j = m.c; j < cc.size(); ++j) if (differs(cc[j].idx)) return false;
        return true;
    }

    bool stands(int i) const { return isRailKind(in.kind[i]) && H.kind[i] == in.kind[i]; }
    bool wished(int i) const { return H.kind[i] == in.kind[i] && H.shape[i] == in.shape[i]; }

    // +100 per rail that now stands, +1 per rail newly in its wished shape; clean = no touched rail off its shape
    int gain(const Mark& m, bool& clean) {
        seen.clear();
        int g = 0;
        clean = true;
        const std::vector<World::Undo>& hc = H.changes();
        for (size_t j = m.h; j < hc.size(); ++j) {
            const World::Undo& u = hc[j];
            if (!isRailKind(in.kind[u.idx])) continue;
            if (std::find(seen.begin(), seen.end(), u.idx) != seen.end()) continue;    // the first entry holds the state before
            seen.push_back(u.idx);
            const bool stood = u.kind == in.kind[u.idx];
            g += 100 * ((int)stands(u.idx) - (int)stood);
            g += (int)wished(u.idx) - (int)(stood && u.shape == in.shape[u.idx]);
            if (stands(u.idx) && !wished(u.idx)) clean = false;
        }
        return g;
    }

    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < w && z < h; }
    bool freeCell(int x, int z) const {
        return inside(x, z) && H.kindAt(x, z) == NONE && (in.flags[z * w + x] & Input::HOLDS_RAIL);
    }
    bool temporary(int i) const {
        return std::find(temps.begin(), temps.end(), i) != temps.end() || std::find(pins.begin(), pins.end(), i) != pins.end();
    }
    void temp(std::vector<int>& list, int x, int z, uint8_t kind, uint8_t handed) {
        doOp({ Op::PLACE_RAIL, (uint8_t)(kind | TEMPORARY), handed, x, z });
        list.push_back(z * w + x);
    }
    void clear(std::vector<int>& list) {
        for (int i : list) doOp({ Op::BREAK, 0, 0, i % w, i / w });
        list.clear();
    }

    // whatever goes down here pulls at every rail beside it with a loose end, a rail pulled at one end goes straight
    // so those rails get a temporary rail on each loose end first; joins = sides the newcomer should connect on
    // a rail pointing in from another side stays alone: pinned and not joined it would end up straight
    void pin(int cx, int cz, int sx, int sz, uint8_t joins) {
        for (int d = 0; d < 4; ++d) {
            const int x = cx + DX[d], z = cz + DZ[d];
            if (!inside(x, z) || (x == sx && z == sz)) continue;
            const int i = z * w + x;
            if (!isRailKind(H.kind[i]) || temporary(i)) continue;
            const uint8_t sh = H.shape[i], met = H.realEnds(x, z);
            if (pointsAt(sh, OPP[d]) && !(joins & (1 << d))) continue;
            int loose[2], n = 0;
            bool can = true;
            for (int k = 0; k < 2 && can; ++k) {
                const int e = ENDS[sh][k];
                if (met & (1 << e)) continue;
                const int gx = x + DX[e], gz = z + DZ[e];
                if (gx == cx && gz == cz) continue;
                if ((gx == sx && gz == sz) || !freeCell(gx, gz)) can = false;
                else loose[n++] = e;
            }
            if (!can) continue;
            for (int j = 0; j < n; ++j) temp(pins, x + DX[loose[j]], z + DZ[loose[j]], RAIL, (uint8_t)(loose[j] < 2 ? 1 : 2));
        }
    }

    // one way of standing a rail: lever or not, temporary rails around first, the rail either way, up to two after
    // everything temporary comes out again at the end
    struct Recipe {
        bool lever = false;
        bool pinAround = false, pinSelf = false;    // pin the rails beside the temporary ones / beside the rail itself
        uint8_t handed = 1;
        uint8_t before = 0;    // sides, one bit each
        int after[2] = { -1, -1 };    // sides, in order
        uint8_t afterKind[2] = { RAIL, RAIL };
    };

    uint8_t wishSides(int i) const {
        const uint8_t sh = in.shape[i];
        return (sh >= 1 && sh <= 6) ? (uint8_t)((1 << ENDS[sh][0]) | (1 << ENDS[sh][1])) : (uint8_t)0;
    }

    void play(int x, int z, uint8_t k, const Recipe& r) {
        if (r.lever) doOp({ Op::LEVER_ON, 0, 0, x, z });
        for (int d = 0; d < 4; ++d) if (r.before & (1 << d)) {
            const int cx = x + DX[d], cz = z + DZ[d];
            if (r.pinAround) pin(cx, cz, x, z, 15);
            if (H.kindAt(cx, cz) == NONE) temp(temps, cx, cz, RAIL, (uint8_t)(d < 2 ? 1 : 2));
        }
        if (r.pinSelf) pin(x, z, x, z, wishSides(z * w + x));
        doOp({ Op::PLACE_RAIL, k, r.handed, x, z });
        for (int j = 0; j < 2; ++j) if (r.after[j] >= 0) {
            const int d = r.after[j];
            const int cx = x + DX[d], cz = z + DZ[d];
            if (r.pinAround) pin(cx, cz, x, z, 15);
            if (H.kindAt(cx, cz) == NONE) temp(temps, cx, cz, r.afterKind[j], (uint8_t)(d < 2 ? 1 : 2));
        }
        clear(temps);
        clear(pins);
    }

    // ways aimed at the wished shape first, then every combination -- rails around may need outvoting
    // first way in shape that harms nothing wins, else the most rails in shape; false = no way kept hand = command
    bool stand(int x, int z) {
        static const int BY_SIZE[16] = { 0, 1, 2, 4, 8, 3, 5, 6, 9, 10, 12, 7, 11, 13, 14, 15 };    // side subsets, fewest first
        const int i = z * w + x;
        const uint8_t k = in.kind[i];
        uint8_t freeSides = 0;
        for (int d = 0; d < 4; ++d) if (freeCell(x + DX[d], z + DZ[d])) freeSides |= (uint8_t)(1 << d);
        const bool leverChoice = k == RAIL && (in.flags[i] & Input::LEVER_WORKS) && !H.leverAt(x, z);
        const uint8_t wish = in.shape[i];
        int ends[2] = { -1, -1 }, nends = 0;    // wished ends with a free cell
        if (wish >= 1 && wish <= 6) for (int j = 0; j < 2; ++j) if (freeSides & (1 << ENDS[wish][j])) ends[nends++] = ENDS[wish][j];

        const Mark m = mark();
        std::vector<Op> best;
        int bestGain = 0;
        size_t bestCost = 0;
        bool found = false, done = false;
        auto attempt = [&](const Recipe& r) {
            if (done) return;
            ++tried;
            play(x, z, k, r);
            const size_t cost = trial.size() - m.t + (r.lever ? 4 : 0);
            bool clean = false;
            const bool ok = same(m);
            const int g = ok ? gain(m, clean) : 0;
            if (ok && (!found || g > bestGain || (g == bestGain && cost < bestCost))) {
                found = true; bestGain = g; bestCost = cost;
                best.assign(trial.begin() + (std::ptrdiff_t)m.t, trial.end());
            }
            rollback(m);
            if (ok && clean && g > 100) done = true;    // up, in its shape, nothing around it worse off
            };

        // aimed at the wished shape
        for (int pinning = 0; pinning < 4 && !done; ++pinning) {
            for (int lev = 0; lev <= (leverChoice ? 1 : 0) && !done; ++lev) {
                for (int pre = 0; pre < (1 << nends); ++pre) {
                    Recipe r;
                    r.lever = lev != 0; r.pinAround = (pinning & 1) != 0; r.pinSelf = (pinning & 2) != 0;
                    int rest[2], nrest = 0;
                    for (int j = 0; j < nends; ++j) {
                        if (pre & (1 << j)) r.before |= (uint8_t)(1 << ends[j]);
                        else rest[nrest++] = ends[j];
                    }
                    for (r.handed = 1; r.handed <= 2; ++r.handed) {
                        attempt(r);

                        // the rest of the ends afterwards, plain or straight-only, in either order
                        for (int first = 0; first < nrest; ++first) {
                            for (int k1 = 0; k1 < 2; ++k1) {
                                Recipe q = r;
                                q.after[0] = rest[first]; q.afterKind[0] = k1 ? ACTIVATOR : RAIL;
                                attempt(q);
                                if (nrest == 2) for (int k2 = 0; k2 < 2; ++k2) {
                                    Recipe q2 = q;
                                    q2.after[1] = rest[1 - first]; q2.afterKind[1] = k2 ? ACTIVATOR : RAIL;
                                    attempt(q2);
                                }
                            }
                        }
                    }
                }
            }
        }

        // every combination
        for (int pinning = 0; pinning < 4 && !done; ++pinning) {
            for (int lev = 0; lev <= (leverChoice ? 1 : 0) && !done; ++lev) {
                for (int si = 0; si < 16 && !done; ++si) {
                    const int subset = BY_SIZE[si];
                    if ((subset & freeSides) != subset) continue;
                    for (int post = -1; post < 4; ++post) {
                        if (post >= 0 && (!(freeSides & (1 << post)) || (subset & (1 << post)))) continue;
                        Recipe r;
                        r.lever = lev != 0; r.pinAround = (pinning & 1) != 0; r.pinSelf = (pinning & 2) != 0;
                        r.before = (uint8_t)subset;
                        r.after[0] = post;
                        for (r.handed = 1; r.handed <= 2; ++r.handed) attempt(r);
                    }
                }
            }
        }
        if (!found) return false;
        for (const Op& op : best) doOp(op);
        return true;
    }

    // second go at a rail off its shape: out and stood again, alone or after lifting one or two neighbours
    // the order rails go down in decides who pulls at whom; kept only if the rails around come out ahead
    bool again(int x, int z) {
        int around[4], n = 0;
        for (int d = 0; d < 4; ++d) {
            const int qx = x + DX[d], qz = z + DZ[d];
            if (inside(qx, qz) && stands(qz * w + qx)) around[n++] = d;
        }
        const Mark m = mark();
        std::vector<Op> best;
        int bestGain = 0;

        // nothing lifted, each one alone, each pair
        int sets[11][2], ns = 0;
        sets[ns][0] = -1; sets[ns++][1] = -1;
        for (int a = 0; a < n; ++a) { sets[ns][0] = around[a]; sets[ns++][1] = -1; }
        for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b) { sets[ns][0] = around[a]; sets[ns++][1] = around[b]; }
        for (int s = 0; s < ns; ++s) {
            if (tried >= limit) break;
            for (int j = 0; j < 2; ++j) if (sets[s][j] >= 0) doOp({ Op::BREAK, 0, 0, x + DX[sets[s][j]], z + DZ[sets[s][j]] });
            if (H.kindAt(x, z) != NONE) doOp({ Op::BREAK, 0, 0, x, z });
            bool ok = stand(x, z);
            for (int j = 0; j < 2 && ok; ++j) if (sets[s][j] >= 0) ok = stand(x + DX[sets[s][j]], z + DZ[sets[s][j]]);
            bool clean = false;
            ok = ok && same(m);
            const int g = ok ? gain(m, clean) : 0;
            if (ok && g > bestGain) { bestGain = g; best.assign(trial.begin() + (std::ptrdiff_t)m.t, trial.end()); }
            rollback(m);
            if (ok && clean && g > 0) break;
        }
        if (bestGain <= 0) return false;
        for (const Op& op : best) doOp(op);
        commit();
        return true;
    }

    void run() {

        // a redstone block under a cell powers what stands on it from the start
        for (int i = 0; i < w * h; ++i) if (in.flags[i] & Input::POWERED) { H.lever[i] = 1; C.lever[i] = 1; }

        // rails first, while the dust cells are still empty: most curves need an empty cell for a temporary rail
        int wanted = 0;
        for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) {
            if (!isRailKind(in.kind[z * w + x])) continue;
            ++wanted;
            if (stand(x, z)) commit();
        }

        // then dust -- it can make a rail with three rails around decide again, by hand and by command differently
        // such rails come out first and go back after the dust; dust that still leaves the two apart is left out
        std::vector<int> apart;
        for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) {
            if (in.kind[z * w + x] != WIRE) continue;
            const Mark m = mark();
            doOp({ Op::PLACE_DUST, 0, 0, x, z });
            trial.back().arg = H.shapeAt(x, z);
            if (same(m)) { commit(); continue; }

            apart.clear();
            bool railsOnly = true;
            for (const World* wld : { &H, &C }) {
                const std::vector<World::Undo>& ch = wld->changes();
                for (size_t j = (wld == &H ? m.h : m.c); j < ch.size(); ++j) {
                    const int c = ch[j].idx;
                    if (!differs(c) || std::find(apart.begin(), apart.end(), c) != apart.end()) continue;
                    if (stands(c) && isRailKind(C.kind[c])) apart.push_back(c);
                    else railsOnly = false;
                }
            }
            rollback(m);
            if (!railsOnly) continue;
            for (int c : apart) doOp({ Op::BREAK, 0, 0, c % w, c / w });
            doOp({ Op::PLACE_DUST, 0, 0, x, z });
            trial.back().arg = H.shapeAt(x, z);
            bool ok = same(m);
            for (size_t j = 0; j < apart.size() && ok; ++j) ok = stand(apart[j] % w, apart[j] / w);
            if (ok && same(m)) commit();
            else rollback(m);
        }
        for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) {
            const int i = z * w + x;
            if (H.kindAt(x, z) == WIRE && in.shape[i] == 0 && H.actual(x, z) == 0 && H.shapeAt(x, z) == DUST_ALL) {
                const Mark m = mark();
                doOp({ Op::SET_DUST, 0, 0, x, z });    // the solver liked a dot here and it has nothing to connect to
                if (same(m)) commit();
                else rollback(m);
            }
        }

        // second look at rails off their shape, now their surroundings are done -- bounded, all rails could run for ever
        limit = tried + 40000 + 250L * wanted;
        for (int sweep = 0; sweep < 4; ++sweep) {
            bool better = false;
            for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) {
                const int i = z * w + x;
                if (!isRailKind(in.kind[i]) || wished(i) || tried >= limit) continue;
                if (again(x, z)) better = true;
            }
            if (!better) break;
        }

        // levers come out last: they also keep their rail steady while dust lands beside it
        // a rail with exactly three rails around decides again when its lever goes: a fourth, temporary one stops that
        // a lever stays only where neither works
        for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x) {
            const int i = z * w + x;
            if (!H.lever[i] || (in.flags[i] & Input::POWERED)) continue;
            const Mark m = mark();
            bool clean = false, gone = false;
            doOp({ Op::LEVER_OFF, 0, 0, x, z });
            if (same(m) && gain(m, clean) >= 0) gone = true;
            else rollback(m);
            for (int d = 0; d < 4 && !gone; ++d) {
                if (!freeCell(x + DX[d], z + DZ[d])) continue;
                for (int v = 0; v < 4 && !gone; ++v) {
                    temp(temps, x + DX[d], z + DZ[d], (v & 1) ? ACTIVATOR : RAIL, (uint8_t)((v & 2) ? 2 : 1));
                    doOp({ Op::LEVER_OFF, 0, 0, x, z });
                    clear(temps);
                    if (same(m) && gain(m, clean) >= 0) gone = true;
                    else rollback(m);
                }
            }
            if (gone) commit();
        }

        out.kind = H.kind;
        out.shape = H.shape;
        for (int i = 0; i < w * h; ++i) {
            if (differs(i)) ++out.mismatched;
            if (isRailKind(in.kind[i])) {
                if (!stands(i)) ++out.dropped;
                else { ++out.rails; if (!wished(i)) ++out.reshaped; }
            }
            else if (in.kind[i] == WIRE) {
                if (H.kind[i] == WIRE) ++out.dust;
                else ++out.dropped;
            }
        }
        for (Op& op : out.ops) {
            if (op.type == Op::LEVER_ON) ++out.levers;
            if (op.type == Op::LEVER_OFF) { --out.levers; ++out.leverVisits; }
            if (op.type == Op::PLACE_RAIL && (op.kind & TEMPORARY)) { ++out.helpers; op.kind &= (uint8_t)~TEMPORARY; }
        }
        out.effort = tried;
    }
};

}    // namespace

void build(const Input& in, Result& out) {
    out = Result();
    Builder b(in, out);
    b.run();
}

}    // namespace railbuild
