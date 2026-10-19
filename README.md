# Moose's Minecraft Mural Maker

Turns an image into a Minecraft mural you can build, in a bit cooler way.

Hosted here: **[mcmural.madmoose.im](https://mcmural.madmoose.im/?utm_source=github&utm_medium=readme)**

<img src="https://mcmural.madmoose.im/resources/images/ui-screenshot.png" alt="The Mural Maker interface: settings on the left, the block preview zoomed in and the snapshots on the right" width="748" />

## How it differs from other converters

Most image-to-block converters reduce every block to one average colour and pick the nearest match, one block per pixel. That works for map art, where each block really is one pixel. A mural is seen up close, where the texture of each block is part of the picture, so this tool matches on both colours and the textures.

**Full textures, not averages.** Each block-sized area of the image is compared with the whole 16x16 px texture of every candidate block, in CIELAB rather than RGB. Three weights decide what counts: the average colour, where light and dark sit inside the texture (keeps edges and outlines readable), and how busy the texture is (matches grain, keeps flat areas calm). Stone and stone bricks average to nearly the same grey; here they are different blocks.

**Every face the game can handle.** Blocks are offered on every side and rotation that is visible in the chosen placement, so a log end, a log side and the same side turned 90 degrees are three different options, and each placement (floor, ceiling, wall facing north, south, east or west) gets the faces the game actually renders there.

**Area-aware refinement.** After the first round of block matching is complete, the mural is revisited in bigger areas of multiple blocks: each block's top candidates are tried, and a swap is kept only when the area moves closer to the source image (MSE and SSIM).

**Overlays that behave in game.** Rails, redstone dust, vines, ladders, glow lichen, flowers and other thin blocks can sit on top of the base blocks to reach colours the base palette cannot. Rails and dust change their shape, depending on what is next to them and/or the order they are placed in, so not all blockstates can be reproduced in survival. The engine models the game's rules, finds a sequence of placements (with temporary helper rails and by powering blocks with levers) that ends in the shapes shown in the preview. Every move of the build can be made in survival.

**Builds that survive.** The solver keeps pistons, TNT, lamps, copper bulbs, crafters and trapdoors away from anything that would power them, and pumpkins away from block patterns that would turn into golems. It warns about blocks that deteriorate and falling blocks without support.

**Multiple MC versions.** Picking a game version restricts the palette only to the blocks that exist there. Renamed blocks are exported under the name that version uses, and the datapack uses the folder layout that version expects.

**Transparency and biomes.** Grass, leaves and vines take the colour of the chosen biome. Transparent blocks are matched against the background that will show through them, and a transparent image can become a mural with gaps.

**Fast enough to tune by eye.** The matching engine is C++ compiled to WebAssembly, runs on all CPU cores and handles murals up to 1000x1000 blocks. The preview updates as settings change, and snapshots keep earlier results to compare and bring back.

## Repository

```
engine/   C++ engine, block data (CSV) and the build script
web/      the app page and the built engine (mural.js, mural.wasm, mural.data)
```

The page is plain HTML, CSS and JavaScript with no build step or framework.

## Building

Requirements: [Emscripten](https://emscripten.org/docs/getting_started/downloads.html).

**Block textures are not included** in this repository. Copy them from the game client jar (`assets/minecraft/textures`) into `engine/data/block_textures`; the CSV files in `engine/data` list the files the engine expects.

```
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build.ps1
```

This writes `mural.js`, `mural.wasm` and `mural.data` into `web/`.

## Running locally

The engine uses threads, which browsers only allow on a cross-origin isolated page. Serve `web/` with these headers and open `app.html`:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Opening the file directly, or a plain static server without these headers, will not start the engine.

## Credits

- [stb_image and stb_image_resize2](https://github.com/nothings/stb): image decoding and resizing
- [miniz](https://github.com/richgel999/miniz): zip and gzip output
- [Litematica](https://github.com/maruohon/litematica) by maruohon and [sakura-ryoko](https://github.com/sakura-ryoko/litematica): export format
- Mojang Studios: all the cool stuff

## License

MIT, see [LICENSE](LICENSE). Covers the code in this repository, not Minecraft's textures and not libraries in `engine/third_party`.

Not an official Minecraft product. Not approved by or associated with Mojang or Microsoft.
