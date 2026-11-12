# Hardpoint for BZFlag

A Call of Duty style **Hardpoint** game mode for `bzfs`. One zone on the map is
active at a time. Teams fight to stand in it, earn points for holding it, and
the zone moves to the next location on a timer.

## How the game is played

- One hardpoint is active at a time. The server announces it by name, for
  example *"Go to the red hardpoint!"*
- Stand inside the active hardpoint to earn your team **1 point per second**.
- If tanks from **two or more teams** are inside, it is **contested** and nobody
  scores.
- After a team has **held it for the hold time** (default 60 seconds), the
  hardpoint moves to the next one in the map. The hold clock only runs while
  exactly one team is inside. It pauses when the zone is empty or contested,
  and it keeps its remaining time when a different team takes over.
- The first team to the **score limit** (default 250) wins.
- **Team score comes only from the hardpoint.** Kills, team kills and flag
  captures do not change it. Individual player scores still count kills as
  usual.
- The game waits until **at least two teams each have two players** (2v2,
  2v2v2 or 2v2v2v2). If it drops below that, it pauses and starts over from the
  first hardpoint when enough players are back.
- While a game is running, players **spawn near the active hardpoint**: at a
  random spot within `_hardPointSpawnRadius` units (default 100) of its
  center, facing it. Spawn spots always come from the server's own safe-spawn
  logic or from spots the mapmaker placed, so players don't spawn inside
  buildings. This keeps the fight at the hardpoint. Set the variable to
  `0` for normal spawns.
- Players find the active hardpoint three ways: the chat announcement, the
  visible marker in the map, and a star of **yellow (rogue-colored) bolts**
  that fly in toward it from above. The bolts also show as dots on the radar.

## Quick start

1. Build the plugin (adjust the include path to your BZFlag source tree):

   ```
   g++ -shared -fPIC -I/path/to/bzflag/include -o hardpoint.so hardpoint.cpp
   ```

2. Add `hardpoint` objects to your map (see below).

3. Start the server with the plugin:

   ```
   bzfs -loadplugin ./hardpoint.so -world mymap.bzw \
        -set _hardPointTime 60 -set _hardPointScoreLimit 250
   ```

## Server variables

Set these with `-set` on the command line or in your server config. Every one
has a default, so none are required.

| Variable | Default | What it does |
| --- | --- | --- |
| `_hardPointTime` | `60` | Seconds of actual holding before the hardpoint moves on. |
| `_hardPointIdleTime` | `0` | Move on if no team has held the zone for this many seconds. `0` means never move on idle. |
| `_hardPointScoreLimit` | `250` | Team score that wins the game. |
| `_hardPointMinPerTeam` | `2` | Minimum players per team. At least two teams must reach it before the game starts. |
| `_hardPointSpawnRadius` | `100` | Players spawn within this many units of the active hardpoint. `0` turns it off and uses normal spawns. |
| `_hardPointSpawnTries` | `60` | How many server-checked spawn spots to try when looking for one near the hardpoint. Raise it on large maps. |
| `_hardPointStarSpokes` | `3` | Number of bolts in the marker star, spaced evenly (3 is 120 degrees apart). |
| `_hardPointStarInterval` | `1.0` | Seconds between marker volleys. |

Tips:

- With `_hardPointIdleTime` at `0`, the hardpoint stays put until a team holds
  it for the full hold time. If you want the game to keep moving on a map where
  nobody can reach a spot, set it to something like `90`.
- Automatic spawning picks from the server's normal spawn spots. On a map with
  team bases and base spawns, players may keep spawning at their bases; use
  `spawn` lines in that case.
- If the yellow bolts are too busy or confusing, raise `_hardPointStarInterval`
  or lower `_hardPointStarSpokes`.

## The map object

Add one `hardpoint` block per location. They rotate **in the order they appear
in the map file**.

```
hardpoint
  name red
  pos 0 0 0
  size 30 30 30
  rot 45
end
```

| Field | Meaning |
| --- | --- |
| `name` | What the server announces. Any text works: `red`, `1`, `north bridge`. Rename it any time without touching anything else. |
| `pos` | Position of the zone. `z` is the **bottom** of the zone. |
| `size` | Same as a box: **half-extents** in x and y, and the **height** in z. `size 30 30 30` is a zone 60 wide, 60 long and 30 tall. |
| `rot` | Optional. Rotation in degrees, counterclockwise, around the vertical axis. `rotation` also works. |
| `spawn` | Optional, repeatable. `spawn x y [z]` is a safe spawn spot you chose for this hardpoint. If a hardpoint has any, players spawn at one of them (random pick) facing the hardpoint. `z` defaults to the hardpoint's `z`. |

The zone is a box in space: tanks must be between `z` and `z + size z`. Make it
tall enough to cover jumps if your map allows them.

**The `hardpoint` block is invisible by itself.** It only tells the server where
the zone is. Players need something in the map to see it.

### Spawn spots

While a game is running, players spawn near the active hardpoint. There are two
ways it finds a spot, and both avoid spawning inside buildings:

1. **Spots you place.** Give a hardpoint `spawn` lines in open areas around it.
   Players then spawn at one of them, chosen at random, facing the hardpoint.
   This is the most reliable option.

   ```
   hardpoint
     name red
     pos 0 0 0
     size 30 30 30
     spawn 70 0
     spawn -70 0
     spawn 0 70
     spawn 0 -70
   end
   ```

2. **Automatic.** With no `spawn` lines, the plugin asks the server for its
   own spawn spots, which it already checks against buildings, the ground,
   enemies and dangerous flags, and keeps the first one within
   `_hardPointSpawnRadius` of the hardpoint. It tries up to
   `_hardPointSpawnTries` times. If none is close enough, the player gets the
   server's normal spawn. On a large map with a small radius, raise the tries
   or add `spawn` lines.

`_hardPointSpawnRadius 0` turns near-hardpoint spawning off.

## Showing players where the hardpoint is

### With BZMapMaker

BZMapMaker supports the hardpoint object. It places the visible marker and the
zone together from a single position, size and rotation, so they cannot drift
apart, and it keeps the hardpoints in rotation order.

### Making your own marker (any editor)

Mapmakers can build the marker themselves. Use an object that is **passable**
(`drivethrough` and `shootthrough`) so tanks and shots go through it, and give
it the **same position, size and rotation** as the `hardpoint` block. A box
works well:

```
material
  name hpRedMat
  diffuse 1 0 0 0.35
  emission 0.6 0 0
end

box
  name hardpoint_red_visual
  position 0 0 0
  size 30 30 30
  rotation 45
  drivethrough
  shootthrough
  matref hpRedMat
end

hardpoint
  name red
  pos 0 0 0
  size 30 30 30
  rot 45
end
```

Notes for hand-built markers:

- A semi-transparent material (alpha below 1) lets players see through the
  marker. A different color per hardpoint, matching its name, makes the
  announcement easy to follow.
- Boxes and meshes show on the radar, so every hardpoint appears as a marker
  there. Only the active one gets the yellow bolts.
- If the marker is not passable, tanks will collide with it and shots will hit
  it. Set both `drivethrough` and `shootthrough`.
- Numbered textures work too. Name the hardpoints `1`, `2`, `3` to match, in
  file order.

## Example map snippet

```
hardpoint
  name red
  pos -150 0 0
  size 25 25 20
end

hardpoint
  name blue
  pos 150 0 0
  size 25 25 20
end

hardpoint
  name center
  pos 0 0 0
  size 30 30 20
  rot 45
end
```

This cycles red, blue, center, red, and so on. Add a matching passable marker for
each one.

## Notes for server owners

- Team flags and bases are not needed. If your map has them, captures are
  ignored for team score.
- Rogue players can score and contest like a team, but they do not count toward
  the minimum players needed to start.
- The marker bolts are fired in the rogue color. On a server that allows rogue
  players, their real shots look the same.

## Troubleshooting

- **The game never starts.** At least two teams need the minimum number of
  players. Lower `_hardPointMinPerTeam` to test alone or in small groups.
- **No announcement or no zone.** Check the server log that the plugin loaded
  and that your map has at least one `hardpoint` block.
- **Players score but the marker is in the wrong place.** The marker and the
  `hardpoint` block need the same position, size and rotation.
- **No yellow bolts.** They appear only while a game is running, above the
  active hardpoint. Raise `_hardPointStarSpokes` or lower
  `_hardPointStarInterval` if they are too sparse.
