Trio for Windows (x64)

1. Copy your own ROM images of the games into the rom folder. Any file
   name works (.sfc or .smc); rom\README.md lists the supported versions.
2. Run trio.exe.

Settings (trio.ini) and save states (saves\) are kept in this folder.
F1 opens the port menu (video, audio, controls, save slots). Default keys
and gamepad buttons: README.md, section Controls.

This build contains no game code: the games run in the built-in
interpreter. Widescreen, the in-game PORT OPTIONS page and battle mode need
the recompiled build, made from source with your ROMs (README.md, sections
1 to 3). A build made with your ROMs contains code generated from them:
keep it to yourself.

Windows may warn that the program is unrecognized (it is not signed):
More info > Run anyway.

License: MIT (LICENSE), except src/snes/dsp.c: GNU LGPL 2.1 or later.
Linked libraries: SDL2 (zlib license), Dear ImGui (MIT). License texts are
in LICENSES\. Source code: https://github.com/y4my4my4m/trio
