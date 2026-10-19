
#include "engine.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

int main(int, char**) {

    // no mural_init() here -- the page calls it once it can show texture progress
    emscripten_exit_with_live_runtime();
    return 0;
}

#else

// native build: no ui, just enough to drive the engine from a harness
#include <cstdio>

int main(int argc, char** argv) {
    mural_init();
    while (!mural_ready()) {  }

    if (argc > 1) {
        setDroppedImage(argv[1]);
        mural_request_preview();
    }
    printf("Engine ready: %d blocks loaded.\n", mural_block_count());
    return 0;
}

#endif
