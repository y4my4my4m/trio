# Battle mode

BATTLE MODE is on the game select screen (docs/LAUNCHER.md). It starts a
2-4 player match between fighters from the three games, in arenas
cut from the stages of all three games.

Every fighter is its own running copy of its game, so movement, dashing,
wall-kicks, ladders, charge timing, special weapons, hurt, knockback,
invincibility and death all come from the game's own player code. The
battle layer puts all the copies in the same arena, turns one fighter's
shots into hits on the others, and runs the rules (lives, respawns, items,
winner) and the CPU players. A G1 hero can fight G3's Blade on a G2 stage.

Everything on screen is the games' own graphics:

- the stage is drawn from whole-level BG1 / BG2 tilemaps with a display
  copy's characters, palettes and background scroll (`ppu_draw_bg_map`,
  `stage_draw`), so no part of the background is missing at any picture
  size or camera position;
- each fighter is drawn from its copy's world objects (`spr_collect`), at
  their world position, so shots stay visible across the whole arena;
- the camera follows all fighters in two dimensions;
- the text uses the G2 / G3 title font.

Needs the recompiled build (the shot range and weapon switch are
overrides) and at least one of `rom/g1.sfc`-`rom/g3.sfc`. Characters
and arenas of a game whose ROM is missing are left out.

## Setup page

Up/Down pick a row, Left/Right change it, B/A/Start confirm, Y goes back.

| Row | Values |
|---|---|
| PLAYER 1-4 | OFF, PLAYER, KEYS 1, KEYS 2, PAD 1-4, CPU LV1-3 |
| CHARACTER, STYLE | see the roster below (under each player that is not OFF) |
| LIVES | 1-9 |
| TIME | OFF, 2, 3 or 5 minutes: at TIME the most lives left wins, then the most life, then the most damage dealt (a tie on all is a draw) |
| STAGE | the arenas below |
| MUSIC | STAGE (the arena's own song), RANDOM, or any stage song of the three games |
| WEAPONS | ALL (every special weapon, energy refills slowly) or BLASTER ONLY |
| WEAPON SETS | whose weapons every hero has: OWN GAME, or any of G1, G2, G3 (all three: 26 weapons) |
| ITEMS | ON (life, weapon energy and 1UP drops) or OFF |
| VIEW | NORMAL (the widescreen width by 224 lines), LARGE 512x288, HUGE 640x360 |

LARGE and HUGE show more of the arena at the same pixel size: the window
grows to fit the picture at its current scale when the desktop has room
(otherwise the largest scale that fits) and goes back after the match.

A single PLAYER slot listens to KEYS 1 and every pad that no other slot
claims. Several PLAYER slots share them out: the first gets KEYS 1, the
second KEYS 2, and the free connected pads go one each in slot order.
KEYS 2 is fixed: move W A S D, jump F, shoot G, dash H, weapon Q / E,
sword J, pause Tab.

In a match, Start pauses (CONTINUE, RESTART, EXIT); the pause menu shows
the first person's character and its controls. With a time limit the time
left is shown under the meters. The results screen
offers REMATCH or EXIT; EXIT goes back to the game select screen.

## Roster

Each player picks a CHARACTER and its STYLE: the game it comes from (its
look, shots, weapons and sounds; each style is a copy of that game).

| Character | Styles |
|---|---|
| HERO | G1 (with the leg part: the G1 hero dashes only with it), G2, G3 |
| HERO ARMOR | G1, G2, G3: the game's four armor parts and life upgrades (32 HP) |
| HERO FULL | G1, G2, G3: the armor and G1's Fireball, G2's Uppercut, G3's gold armor (the four enhancement chips) |
| BLADE | G3: blaster and sword (X button) |
| RAIDER | G3, G1 (in his walker), G1 FOOT: the boss, run by the battle layer (below). Experimental |
| WARLORD | G3, G1, G2: the boss, run by the battle layer (below). Experimental |

RAIDER and WARLORD are rougher than the playable characters (see "Not done
yet"): the setup page shows BOSSES ARE EXPERIMENTAL while one is in
the match (`battle_character_experimental`).

HERO FULL has G1's Fireball (down, down-forward, forward, Y) and G2's
Uppercut (forward, down, down-forward, Y) in every style, at any HP:

- in his own game the game does it, with its full-HP check skipped in
  battle mode (`ovr_G1FireballHp` 81:A0EC, `ovr_G2UppercutHp` 08:C5C5);
- otherwise the arena reads the motion and HERO FULL of the game that has the
  move, in a helper copy at the fighter's place, does it: the motion is
  typed into the helper (a few frames), so his game starts the move and
  loads its pose and fireball tiles (writing his action $42 directly leaves
  them unloaded: nothing was drawn). While it lasts the helper's hero is the
  one shown, in his game's armor. The Fireball's projectile (G1 shot kind 4)
  and the Uppercut's flame (G2 shot kind 4, invincible while it lasts) are
  the fighter's attacks; in the Uppercut the fighter goes where he goes.

Both do 6 of a life's 16 (the games' kill in one hit; a full charge does
5), and a fighter's next special waits 3 s (`SP_COOLDOWN`): the motion's
last press is dropped until then, in his own game too.

Each hero plays exactly as in his own game, so between the styles there is
every hero moveset of the three games, and the 24 special weapons.

With WEAPON SETS, the hero of any style has the weapons of other games too.
L / R go through his list (his blaster, then each chosen game's weapons in
that game's order). A weapon of another game moves him into his copy of
that game, in the same tier (HERO ARMOR G1 for HERO ARMOR G3): that hero
takes his place, facing, standing or falling, and his share of life (`hop`
in arena.c), and shows that game's look; the copy he leaves runs on with
its hero hidden while its shots fly, and keeps its weapon energy. In the
new copy the weapon before the chosen one is put up and one R of the game's
own takes it out, with its colors and meter. A respawn starts back in his
own game.

## Arenas

`arena_defs` in `src/arena/stage.c`: one box per stage, chosen by a scan of
each stage's collision for large connected areas with several floor levels
(`tools/arenas.py` scores candidate boxes; floors that can't be reached
from the others count against a box). A box is 1024 x 256 to 1024 x 512
px, several screens wide, with heights, platforms, ladders and walls to
climb.

Each box is then rebuilt as a platform-fighter stage (`rebuild` in stage.c,
`layout` per arena): the level's collision in it is taken out and every BG1
tile of the level with it, inside the box and around it, and a layout is
built with the stage's own blocks: its floor block on top, the block found
under it as the fill. The collision (`geo`) and the BG1 tilemap the picture
is drawn from are edited together, so they match. Only the layout stands
over the background.

The background of a rebuilt box (`find_background`, `draw_background`) is
the stage's BG2, but only the part of it the game itself shows there. The
whole-level BG2 map has holes and pieces of level where the game's camera
never looks; drawn past the game's own window they slid into the picture
as black rectangles and stray structures. So the BG2 camera is read with
the game's camera at the box's four sides: that window of the map is drawn
fresh every frame (tiles and colors animate), scrolled at the game's own
rate, mirrored side by side to fill any picture width and ended in a plain
color above and below. It is dimmed to 72%, and the top of everything stood
on gets a light edge (`light_edges`), so the platforms stand out. Where the
level's walls covered BG2 the map has nothing: with a dark backdrop, such
pixels in a row that is mostly drawn take the row's average color.

The hero's jump rises 55 px in all three games, so every platform is three
blocks (48 px) over the floor or platform under it (`STEP`): each is
reached by a jump from right below, in every game.

| Layout | |
|---|---|
| BATTLEFIELD | a main platform over open sides, two one-way platforms 48 px over it and one between them 48 px higher |
| FINAL | one long main platform, with moving platforms |
| TIERS | a low middle, sides two blocks higher, a one-way platform 48 px over each of the three |
| TOWERS | a main platform with two pillars five blocks high (walls to kick up), a one-way bridge joining their tops, one-way platforms 48 px over the floor outside them |

Under its first row a slab darkens with depth (`shade_slabs`, to 3/8 at
70 px), so its top edge reads and the repeated fill block does not.

One-way platforms are ladder tops with no ladder under them: stood on,
jumped through from below. Down + B drops through (the hero is put just
under it, falling); while the hero stands on one it is solid in his copy
for the frame, so Down (which the games read as climbing down) and the
specials' motions work there.

Hazards (`hazard`, a small section of the rebuilt layout, run in arena.c
and drawn in battle.c):

| Hazard | Stages | |
|---|---|---|
| ICE | G1 stage 8, G2 stage 6, G3 stage 2 | the outer thirds of the main floor: the speed the hero had is kept and changes slowly (slides when he stops or turns) |
| WIND | G1 stage 5, G2 stage 2, G3 stage 1 | over the middle third: a gust pushes everyone, turning about every 6 s; streaks show it |
| FIRE | G1 stage 4, G1 stage 6, G2 stage 4 | two jets in the main floor: embers for a second, then a 72 px flame (3/16 on touch; the KO goes to the last hitter) |

The bosses' rooms are stages too, as they are in their levels (19 of
them, `G1 ROOM 2` and so on): each level's collision was scanned for closed
one-screen rooms with a flat floor and open space above, and the one at the
level's end taken. Around a room the picture is darkened.

Stages whose look depends on stage logic the display copy doesn't run
(G2 stage 7's background, G2 stage 12's areas) are left out.

Booting an arena runs its game from power-on into the stage (title, new
game, stage select, dialogues paged) and glides the player to the box. The
collision of the box is then read (`trio_geo_read`): a block map of the
three games' shared attribute values (0 air, 1-0C slopes, 0D/0E water,
12/13 ladder, 33 and up solid, 3E damage, 3F spikes).

Spawn points are flat floors three blocks wide (solid, or a one-way
platform) with three blocks of room above, in the box's main open space (a
floor in a sealed pocket is skipped), spread as far apart as possible. A
respawn takes the spawn point farthest from the fighters still in.

Moving platforms (the FINAL layout and the boxes kept as they are in the
level): up to two lanes per arena are picked from its collision
(`pick_lanes`): the longest run of open air a jump (48 px) above a floor,
and one a second jump (96 px) above a floor, apart from the first. A
3-block platform goes back and forth along each. Its collision is a row of
ladder-top blocks (stood on, passed from below and the sides) written into
every fighter's copy at the block column nearest it (`trio_put_block`: in a
copy with the arena's collision a block's number is its attribute);
fighters standing on it are carried along. It is drawn with the box's most
common floor block.

## Fighters of different games in one arena

Every fighter copy gets the arena's collision, whatever its game
(`trio_geo_install`): its block attribute pointer is set to an identity
table in WRAM and its block map is rebuilt so each block's id is its
attribute. The game's own collision code then sees the arena's floors,
walls, ladders and spikes. The copy's own stage graphics are never shown.

G3's Blade is 8 px taller than the hero: his object's Y is 25 px over his
feet, the hero's 17. `trio_read_player` and `trio_place` move it by that much
(`TRIO_BLADE_RISE`), so a fighter's feet are at `y + 17` whoever he is: the
item pickup, the one-way platforms (drop-through, standing on them), the
moving platforms' carry, the ice and the respawn's landing all go by the
feet. His hurt box is the 8 px taller.

| | G1 | G2 | G3 |
|---|---|---|---|
| Player object | `$0BA8` | `$09D8` | `$09D8` |
| Damage routine | `84:9D07` | `08:D963` | `04:CD6A` |
| Music call | `80:87A2` | `00:8457` | `00:84BC` |
| Weapon switch | `81:99C9` | `08:BECC` | `04:AFBF` |
| Shot on-screen test | `82:80B4` | `00:D859` | `02:D636` |

(`src/arena/games.c` has the whole table: HP, lives, energy, sound queue,
sound ids, modes.)

## Battle overrides (src/game/battle_hooks.c)

Active only while `battle_box.on`, so normal play and `trio-verify` run the
original code.

| PC | Override | Change |
|---|---|---|
| 82:80B4, 00:D859, 02:D636 | `ovr_G?OnScreen` | the test every game runs to delete shots off its camera: inside the arena box (and a screen around it) counts as on screen, so shots fly the whole arena |
| 81:99CE, 08:BEDC, 04:AFD3 | `ovr_G?SwitchShots` | L / R switches weapon at once; the games wait for all of the hero's shots to be gone |

## Rules

Each fighter has its game's HP per life (16, or 32 with life upgrades).
Damage is in sixteenths of a life and handed to the victim's own damage
routine, so knockback, the hurt animation, invincibility frames and armor
cuts are the game's. Blaster hits do 1 (uncharged), 3 (half charge) or 5
(full charge; 6 for G1's third level); special weapons have their own
values (`arena_shot_info`).

- Shields (G2 shot kind $11, a charged special weapon; G3 shot kind $0D):
  they block the other fighters' shots that touch them instead of doing
  damage, and a blocked shot is gone.
- Blade's sword: the X button slashes at once: his action $4C on the ground,
  $4E in the air (the slashes the game plays after his charged blaster
  shot, $4A). The ground slash is cut after the blade (frame 10) and the
  pellet both slashes end with is removed. The blade is a box in front of
  him for frames 3-9 of the ground slash and 5-12 in the air, 6 damage.
- G3's arm-part charge: once the big shot leaves the hero the game moves it
  to 128 px off the camera's edge behind him (01:C47A, when it touches him)
  to fly in from there; the arena puts it back in front of him.
- A weapon switched with its shots still out: the next weapon's tiles and
  colors replace that weapon's in the copy (the games wait for the shots to
  be gone before a switch; battle mode does not), so they were drawn as
  whatever the new tiles held. The copy's picture chip is kept as it was at
  the switch (`ArenaFighter.old_ppu`) and the put-away weapon's shots, which
  go on in the copy as they are, are drawn from it (`draw_sprites`). A
  weapon held at the hero or a shield ends with the switch; shots of the
  weapon before that one leave the copy (below) or end.
- Shots that leave their copy (`ghost_take`, `ArenaGhost`): a game's shot
  table holds 8 or 9 objects. With shots crossing the whole arena the table
  filled up and the farthest were deleted in flight (44 in one 90 s match).
  Such a shot is now taken over by the arena: it flies on in a straight
  line at the speed it had, drawn from a picture of it taken then, and hits
  like any shot.
  Shields, weapons held at the hero, shots at rest and objects of kinds
  `arena_shot_info` does not list (glows, sparks) stay the copy's.
- Weapon energy goes down per shot as in each game, and refills one unit
  every 2 s.
- Falls: falling out of the bottom of the box costs a life; the last
  fighter to hit you within 5 s gets the KO.
- Death: the game's death animation and sound, then a respawn. The
  respawn is the game's warp-in: a beam down to a spawn point (from below
  any ceiling over it), with the warp sound, then 2 s of protection. The
  warp-in ends with the hero set 16 px lower than the beam landed (inside
  the floor here); the arena lifts him back onto it. At the start P1 and P2
  face each other, and P3 and P4.
- Items (ITEMS ON): every 10 s something drops onto one of the box's open
  floors: small life (45%), large life (25%), weapon energy (23%) or a
  1UP (7%). It stays 20 s, blinking for the last 3. Touching it takes it,
  with the game's pickup sound; a 1UP shows "1UP" rising over the fighter
  (the lives count under his meter goes up by one). They are the games' own pickups: item
  objects in the display copy (ids are the same in the three games: 2 life,
  sub 0 large / 2 small; 1 weapon energy, sub 0 / 1; 4 1UP; sub bit 7 the
  kind placed in a level, which neither pops up nor times out), kept where
  the arena has them every frame and drawn from that copy's sprites. The
  item table is $1628 (G1) or $1518 (G2, G3), 0x30 bytes a slot. The display
  copy's hidden player stands at the top of its camera, away from every
  item, so it picks nothing up.

Last one standing wins.

## Sound

`src/arena/sound.c`. The fighter copies run with no sound chip at all
(`no_apu`); the sound effects they queue (read from the game's sound queue)
are sent to chips run by the battle, one per frame through the driver's
handshake, as the game's NMI would.

| Chip | |
|---|---|
| shared, one per game (`chip`) | the music (the arena's game, or the chosen song's) and the menu sounds: one song is heard however many fighters of a game are in the match |
| silent, one per game (`silent`) | a booted copy's chip with its music stopped (command `$F0` in all three games), run until quiet: what the fighters' chips start as |
| per fighter and game, two (`own`) | `SND_BODY`: his hurt and death sounds alone. `SND_ACT`: the rest (shots, dashes, jumps, pickups). Made on the first sound |

The drivers have a few effect channels and priorities meant for one
player. On one chip per game, fighters cut each other's sounds: G3's sound
`$06` silences whatever plays and keeps a hurt sound (`$0D`) from starting
for at least 40 frames; `$04`, `$05`, `$07` and `$0E` replace it. A fighter's
own sounds do the same to his hurt sound: a charged shot (`$04`, `$05`)
20-40 frames earlier, a sword swing (`$07`) or a pickup (`$15`) leaves it
one of its two channels, cuts it short or keeps it from starting (CPU
players charge all the time: in a Blade match 4 of his 9 hurt sounds were
cut). So the hurt and death sounds have a chip to themselves, and every hit
is heard. A fighter's chip that has been silent for 1.5 s stops running
until his next sound. `ARENA_SND_LOG=1` prints every sound a fighter
queues.

## Raider

Raider is the boss from his own stage, on foot, running his own code: his
jumping attack, his single shot and his spread of shots, his fire, his hit
flash. The game has no player code for him, so the battle layer moves him
between his attacks and his own patterns play the attacks (every boss works
this way):

| | Every boss |
|---|---|
| Left / Right | face that way and walk (steered in the air too) |
| B | jump, held for height |
| Down + B | drop through a moving platform |
| X | his jump attack (Raider: jump at the target with fire bombs) |
| Y | his shot (Raider: rise and fire a shot) |
| A | his special (Raider: rise and fire a spread) |

Between attacks he is held in his wait and the controller (`control` in
boss.c) moves him: walking, jumping with gravity, landing on floors and on
the moving platforms, stopped by walls and ceilings, climbing steps of up to
a block. G3's bosses show their own jump frames in the air (Raider 04 rising,
06 falling; Warlord 81). An attack starts when he is in his wait on the
ground; one pressed up to a third of a second early is kept until then.
The attack itself is the ROM's, with its own movement; B cancels it into a
jump after half a second, and none runs longer than 2.5 s. While attacking
a boss's body hurts whoever it touches (2/16: punches, charges, stomps hit
only the stand-in hero in its own copy). Neither boss walks in
his fight and the ROM has no walking frames, so a walk steps through his
standing frames (Raider's four stances, Warlord's two). A boss that falls out of
the arena loses a life. He has the game's 32 HP and takes double damage, so
a life lasts as long as anyone's.

How it works (`src/arena/boss.c`):

- Source. His stage (G3 stage 14) is booted in the corridor before his door
  (the boot glide carries the hero over the spikes at the top of the level,
  `glide_y`), and the hero walks through the door so the game loads the room
  the normal way. His walker (object $63) is cut to 1 HP and shot; Raider on
  foot is object $61. Once he waits in his fight loop, his object, both OBJ
  name tables and the OBJ palettes (CGRAM and the game's copy at $7E:0400)
  are kept. This runs once per session.
- Fighter. A clone of the arena's hero copy gets those. Other enemies are
  switched off, his tiles are put back every frame (the stage's own loaders
  write into the OBJ tables as his camera moves), and he is kept inside the
  room (enemies are not held by the camera). The stage's WRAM tables a boss
  reads come from his room too (`stage_tab`): G3's object graphics table
  ($7F:8000, by object +$10), from which his animation frames are streamed
  (04:BCC0; with the arena stage's table they streamed the wrong tiles and
  he turned to noise), and G1's palette slots ($7F:83xx; the walker's
  +$11 is `$7F:8349 | +$33`, 83:E860).
- Aim. The hero stays in the copy, invisible (+$0E bit 0 clear), invincible,
  and standing on the opponent Raider faces (or 200 px out on that side).
  The AI turns toward the hero and aims at him, so it aims at the real
  opponent.
- Meter. $7E:1F1F points the game's boss meter at the boss object; its HUD
  sprites come after the hero's (OAM slots 16 on, drawn at x 232), and the
  battle HUD shows those for a boss fighter.
- Control. Pattern 4 is his wait. While he waits he is put back to the
  moment the wait began before every frame (keeping his position, HP, hit
  state and flags), so the AI's wait never runs out; a move sets the pattern
  (0 jump, 6 shot, 2 spread) and the ROM plays it.
- Hits. His shots are the enemy shots at $1318-$1517 (3 damage). Hits on him
  take HP, set +$28 and start his invincibility (+$37 = $3C), which drives
  the game's hit flash. At 0 HP he goes into his defeat scene (state 4, a
  dialogue on the text layer, not drawn), so he blinks out instead.
- Sound. His twin is the same copy without him, so only his sounds remain.
- The hero in the boss's copy is frozen: his whole object (up to $0A7B,
  where a death gets queued) is put back each frame, so the boss's attacks
  landing on him there change nothing.
- Bosses are not held by the camera or by ceilings: each is kept inside the
  room and below its top, and one that ends up inside solid ground (a jump
  through a room's ceiling) is put back on the floor.

| | |
|---|---|
| $7E:1318-$1517 | enemy shots, 8 slots of 0x40 |
| +$01 / +$02 / +$03 | state (2 fighting, 4 defeated) / pattern / step |
| +$27, +$28, +$37 | HP, hit, invincibility |
| player +$0E bit 0 | drawn |

## Warlord

Warlord is the first form of G3's final boss, run like Raider (same controls,
same code in `src/arena/boss.c`). He has two moves: B and A are his jumping
attack (pattern 4), Y his barrage of cannon shots (pattern 2); pattern 0 is
his wait. His shield blocks shots as in the game.

He holds his shield up while he waits or walks: shots from in front are
stopped then. His attacks open his guard. (Warlord's sprite does not follow
bit 6 of +$11 the way Raider's does, so the arena uses the facing the
controls chose.)

His stage (G3 stage 13) is a shaft climbed from the start, and his room
sits over a hole in its floor. The game only loads the room (and spawns him
with his graphics) the way the player arrives, so the source copy is taken
there that way:

- boot at the stage start, then move the camera across under the hole and
  straight up into the room with the hero carried at the top of the level;
  only the scroll limits are written, since the spawner compares the camera
  with its previous position ($1E7A / $1E7C);
- start the room's area objects: category 2 (table $1D18, 0x10 per slot),
  the camera zone $00/$06 at (1920, 2688) and $1A/$10, $18/$21 at the hole;
- open the scroll limits and take the hero down through the hole and back
  up, so the camera follows him like in play;
- the spawner then brings Warlord in (object $5E, enemy slot $0D98) with his
  dialogue, which B pages through, and he is taken once he waits.

| | |
|---|---|
| $7E:1518-$1817 | category 0 objects (events), 0x30 per slot |
| $7E:1818-$1D17 | category 1 (items), 0x20 per slot |
| $7E:1D18-$1E17 | category 2 (area and camera objects), 0x10 per slot |
| $7E:F800 | spawn list per 32 px column: entries of 5 bytes (spawned flag, y, pointer into bank $3C: category, y, id, sub, x) |

## G1 and G2 bosses

The same framework (`src/arena/boss.c`) runs bosses of the other games in a
copy of their own game: the enemy table, enemy shots and player are that
game's, and each boss names its fight state, its wait (pattern, and the
step of it for G2 Warlord), and its hit and invincibility fields. Their
sources are booted by scripted routes:

| Boss | Route | Fight | Attacks (X / Y / A) |
|---|---|---|---|
| G1 RAIDER ARMOR ($67) | G1 stage 9 the first time: Raider in his walker; through the shutter, A pages the dialogue | state 2, wait $12 (the landing; $0A, which he picks moves from, backsteps), chooser 0 | jump stomp / punch / charge across |
| G1 RAIDER ($69) | G1 stage 9; `$1F7D = 2` (his retry path) skips the walker; through both shutters, A pages the dialogue | state 4, wait 4 | jump with fire bombs / shoulder cannon / dash kick |
| G1 WARLORD ($65) | G1 stage 12 ($0C): up the shaft, his object builds the room; his guard (object $26) cut to 1 HP and shot | state $0A, waits 8 and $0E | dash (slash on contact) / blaster / sword arc |
| G2 WARLORD ($67) | G2's final stage (stage 5 with `$1FAE = 4`): Blade's spawn marked done, camera glided down the shaft, A pages the intro | state 2, wait = step 6 of pattern $0A | claw dash / energy balls / beam |

Moves that need the boss's own room walls are left out (G1 Warlord's wall
jump, G2 Warlord's jump to the wall, which the game also chains to after a
slash: it is sent back to the wait). Pattern 0 is G2 Warlord's chooser, which
picks a move the frame it runs; it is sent back to the wait too. In their
copies the stand-in hero isn't drawn and the hero's life meter shows the
boss's HP.

## CPU players

`src/arena/abot.c` plays through the pad, like a person. It builds a
platform graph of the arena: floors with room above, and edges for walking
off a ledge, jumping (up to 52 px, gaps up to 96 px with a dash), climbing
a wall with wall kicks, and ladders (up and down). It goes along the graph
to its target or to an item it wants; when it hasn't landed a hit for a
while and the target is out of reach, it roams to another floor and picks
another target. It dodges shots with jumps, charges, keeps its distance
(never closer than 32 px: a shot leaves the blaster about 20 px ahead, and
from nearer comes out behind the target),
switches weapons now and then (and back to the blaster when dry), and Blade
slashes when close. Levels 1-3 change reaction time, aim and aggression.

`ABOT_LOG=1` prints each bot's position, platform and plan once a second.

## Tools and tests

- `trio-battle-shot ROMDIR PREFIX` drives the game select screen and the
  setup page headless and saves frames of a CPU match as PPM (environment:
  `BATTLE_CHARS`, `BATTLE_MUSIC`, `BATTLE_ITEMS`, `BATTLE_WEAPONS`,
  `BATTLE_WIDE`, `BATTLE_WAV`).
- `trio-arena-preview ROMDIR` renders every arena (`ARENA_COLLISION=1`
  overlays the collision).
- `ctest` runs `trio_test` (each game's boot, damage, death, warp-in, weapon
  energy, every arena installed in every game), `sound_test` (music, stop
  and effects of each game's chip) and `arena_test` (CPU matches across the
  three games must land hits and mostly finish; weapon switching with a
  shot out). `ARENA_MATCH=n` starts at match n, `ARENA_LOG=1` prints the
  fighters every 5 s. They need the ROMs in `rom/` and are skipped without
  them.

## Not done yet

- RAIDER and WARLORD: their sprites break up at times, they have no walking
  frames, and their attacks were made for one opponent in their own room.
- A boss's controller treats slopes as flat ground at the block's top, so
  on a slope it can stand up to a block above the surface.
- Platform collision moves in 16 px steps (the block grid) while the
  platform is drawn where it is, so its edges are up to 8 px off. The CPU
  players don't plan routes over moving platforms.
- Fighters of the same character look the same apart from their tags.
- The rebuilt layouts use one floor block and one fill block per stage.
  Where a stage's BG2 has nothing in the game's window (G3 stage 0, G1
  stage 11) the background is a plain color.
- A shot taken over by the arena (far from its owner with his shot table
  full) flies straight: a homing or bouncing weapon's shot stops turning
  then.
