# Super Mario Maker 2 (title 01009B90006DC000), version 3.0.3: what the build (build.ps1) needs to know.
@{
    GameId         = 'smm2'
    Title          = 'smm2-dx'
    BundleId       = 'org.smcraft.smm2recomp'
    TitleId        = '0x01009B90006DC000'
    DisplayVersion = '3.0.3'

    # SMM2's code has no movie library: no runtime/hle/movie.c, no FFmpeg
    Movies         = $false

    # The game's glue, built into the executable with the shared runtime (the host-drawn SMM1 100 Mario screens are
    # games/smm2/mods/smm1)
    SourceGlobs    = @('games/smm2/*.c', 'games/smm2/mods/*.c', 'games/smm2/mods/smm1/*.c')

    # Guest symbols replaced by host code before recompiling (one mangled name per line; empty: none)
    Hle            = 'games/smm2/hle_symbols.txt'
    # Per-instruction patches for the generator, and the files they import (part of what the game code library is built from)
    Patches        = 'games/smm2/patches.py'
    PatchDeps      = 'games/smm2/mods/*.py'

    # Your own dump, extracted: the base game (v1.0.0) and the v3.0.3 update (`build.ps1 -Setup`). Only the update's
    # exefs is used (an update ships the whole exefs); its romfs goes over the base game's.
    DumpRoot       = '~/smm-work/extracted'
    DumpBaseRomfs  = 'smm2-romfs'
    DumpUpdateExefs = 'smm2-v303-exefs'
    DumpUpdateRomfs = 'smm2-v303-romfs'
}
