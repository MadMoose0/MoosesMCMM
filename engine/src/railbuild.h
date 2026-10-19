#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// rails + dust on a floor mural, laid only by moves a survival player can make
namespace railbuild {

enum Kind : uint8_t { NONE = 0, RAIL = 1, POWERED = 2, ACTIVATOR = 3, DETECTOR = 4, WIRE = 5 };

// rail shape: 1 n-s, 2 e-w, 3 s-e, 4 s-w, 5 n-w, 6 n-e; dust keeps a side mask there
enum : uint8_t { DUST_N = 1, DUST_S = 2, DUST_W = 4, DUST_E = 8, DUST_ALL = 15 };

inline bool isRailKind(uint8_t k) { return k >= RAIL && k <= DETECTOR; }

// hand tells the neighbours the block that was there, /setblock the new one (only matters by dust)
enum Mode { HAND, COMMAND };

// the game rules for rails and dust (26.3 jar), checked move for move against a running server
class World {
public:
    World(int w, int h, Mode mode);

    // moves
    void placeRail(int x, int z, uint8_t kind, uint8_t handed);
    void placeWire(int x, int z, int given);    // given: sides a command names, -1 = none (hand dust works it out)
    void setWire(int x, int z, uint8_t mask);    // lone dust: cross or dot
    void remove(int x, int z);
    void setLever(int x, int z, bool on);    // lever under the block the cell stands on
    void takeLever(int x, int z);    // unlike a flip by command, always tells the rail above

    uint8_t kindAt(int x, int z) const { return inside(x, z) ? kind[z * w + x] : (uint8_t)NONE; }
    uint8_t shapeAt(int x, int z) const { return inside(x, z) ? shape[z * w + x] : (uint8_t)0; }
    bool leverAt(int x, int z) const { return inside(x, z) && lever[z * w + x]; }
    uint8_t actual(int x, int z) const;    // dust: sides with something to connect to
    uint8_t realEnds(int x, int z) const;    // rail: ends met by a rail pointing back, DUST_* bits

    // undo, for trying a move out
    struct Undo { int idx; uint8_t kind, shape, lever; };
    size_t mark() const { return journal.size(); }
    void rollback(size_t to);
    const std::vector<Undo>& changes() const { return journal; }
    void forget() { journal.clear(); }

    const int w, h;
    const Mode mode;
    std::vector<uint8_t> kind, shape, lever;

private:
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < w && z < h; }
    void write(int x, int z, uint8_t k, uint8_t s);
    bool isRail(int x, int z) const { return isRailKind(kindAt(x, z)); }
    bool canConnect(int x, int z, int d) const;
    void setRail(int x, int z, uint8_t sh);
    void connect(int x, int z, int d);
    void decide(int x, int z, uint8_t handed, bool always);
    void notify(int cx, int cz, uint8_t source);
    void notifyCell(int x, int z);
    void run(bool single, int x, int z);
    bool connectable(int x, int z) const { uint8_t k = kindAt(x, z); return k == WIRE || k == DETECTOR; }
    void dustAround(int x, int z);
    void wiresNextTo(int x, int z);

    std::vector<Undo> journal;
    int depth = 0;
    struct Pending { bool single; int x, z; };
    std::vector<Pending> layer;
};

struct Op {
    enum Type : uint8_t { LEVER_ON, PLACE_RAIL, PLACE_DUST, SET_DUST, BREAK, LEVER_OFF };
    uint8_t type;
    uint8_t kind;    // which rail, for PLACE_RAIL
    uint8_t arg;    // for PLACE_RAIL 1 or 2 = the way its put down, for dust a side mask
    int x, z;
};

struct Input {
    int w = 0, h = 0;

    // what the solver put in each cell + the shape it wanted
    const uint8_t* kind = nullptr;
    const uint8_t* shape = nullptr;

    // bits: a rail can stand here / a lever under it powers the top / powers it by itself
    const uint8_t* flags = nullptr;
    enum : uint8_t { HOLDS_RAIL = 1, LEVER_WORKS = 2, POWERED = 4 };
    void (*alive)() = nullptr;    // called every move, so a long build isnt taken for a hang
};

struct Result {
    std::vector<uint8_t> kind, shape;    // finished layer, NONE where an overlay was left out
    std::vector<Op> ops;
    int rails = 0, dust = 0;    // standing at the end
    int reshaped = 0;    // rails that couldnt take the wished shape
    int dropped = 0;    // overlays left out: hand and command never agreed
    int helpers = 0;    // temporary rails placed and broken again
    int levers = 0;    // levers that stay
    int leverVisits = 0;    // levers hung, switched on and taken away again
    int mismatched = 0;    // hand vs command apart, 0 unless the model has a hole
    long effort = 0;    // rail placements played through
};

// temporary rails steer: a rail takes its shape from its neighbours when placed and keeps it
// a search, not a proof: a rail off its wished shape may still have had a way there
void build(const Input& in, Result& out);

}    // namespace railbuild
