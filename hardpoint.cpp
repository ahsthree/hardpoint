// hardpoint.cpp - COD-style Hardpoint game mode for BZFS
//
// Build (adjust the include path to your bzflag source tree):
//   g++ -shared -fPIC -I/path/to/bzflag/include -o hardpoint.so hardpoint.cpp
//
// Run:
//   bzfs -loadplugin ./hardpoint.so -world mymap.bzw \
//        -set _hardPointTime 60 -set _hardPointIdleTime 0 \
//        -set _hardPointScoreLimit 250 -set _hardPointMinPerTeam 2
//
// Map object (one per hardpoint, in the order they should rotate):
//   hardpoint
//     name red
//     pos 0 0 0
//     size 30 30 30      # half-extents in x/y, height in z (same as a box)
//     rot 45             # optional, degrees counterclockwise (also: rotation)
//     spawn 80 0         # optional, repeatable: a mapmaker-chosen safe spawn spot
//     spawn 0 80 0       #   (x y [z]; z defaults to the hardpoint's z)
//   end
//
// BZDB variables:
//   _hardPointTime         seconds of actual holding before the hardpoint moves
//                          (paused while empty or contested)       default 60
//   _hardPointIdleTime     move on if nobody has held it for this many seconds
//                          (0 = never move on idle)                default 0
//   _hardPointScoreLimit   team score that wins the game           default 250
//   _hardPointMinPerTeam   min players per team; at least two teams must
//                          reach it before the game starts         default 2
//
//   _hardPointResetDelay   seconds after the last player leaves before team
//                          scores reset to 0 (0 = never reset)            default 30
//   _hardPointSpawnRadius  players spawn at a random spot within this many units
//                          of the active hardpoint (0 = normal spawns)    default 100
//   _hardPointSpawnTries   how many server-checked spawn spots to try when
//                          looking for one near the hardpoint            default 60
//   _hardPointStarSpokes   shots in the marker star (360/N degrees apart)  default 3
//   _hardPointStarInterval seconds between marker volleys                   default 1.0
//
// Team scores come ONLY from holding the hardpoint. Kills, team kills and flag
// captures do not change them (player scores still count kills as usual).

#include "bzfsAPI.h"
#include <vector>
#include <string>
#include <sstream>
#include <cmath>
#include <cstdlib>

struct Hardpoint {
    std::string name;
    float pos[3]  = {0, 0, 0};
    float size[3] = {10, 10, 10};   // half-extents x/y, height z (box convention)
    float cosR = 1.0f, sinR = 0.0f; // rotation about z, set from `rot`
    std::vector<std::vector<float> > spawns;   // optional {x, y, z or NAN}
};

static std::vector<Hardpoint> g_points;

// ---------------------------------------------------------------- map object
class HardpointMapHandler : public bz_CustomMapObjectHandler {
public:
    bool MapObject(bz_ApiString object, bz_CustomMapObjectInfo *data) override {
        if (!data) return false;
        Hardpoint hp;
        for (unsigned int i = 0; i < data->data.size(); i++) {
            std::istringstream ss(data->data.get(i).c_str());
            std::string key;
            ss >> key;
            if (key == "name") {
                std::getline(ss >> std::ws, hp.name);          // allow multi-word names
                while (!hp.name.empty() &&
                       (hp.name.back() == '\r' || hp.name.back() == ' '))
                    hp.name.pop_back();
            }
            else if (key == "pos")  ss >> hp.pos[0]  >> hp.pos[1]  >> hp.pos[2];
            else if (key == "size") ss >> hp.size[0] >> hp.size[1] >> hp.size[2];
            else if (key == "spawn") {
                float sx = 0, sy = 0, sz = NAN;
                ss >> sx >> sy;
                if (!(ss >> sz)) sz = NAN;          // z is optional
                std::vector<float> v;
                v.push_back(sx); v.push_back(sy); v.push_back(sz);
                hp.spawns.push_back(v);
            }
            else if (key == "rot" || key == "rotation") {
                float deg = 0;
                ss >> deg;
                float r = deg * 3.14159265f / 180.0f;
                hp.cosR = cosf(r);
                hp.sinR = sinf(r);
            }
        }
        if (hp.name.empty())
            hp.name = std::to_string(g_points.size() + 1);
        g_points.push_back(hp);
        return true;
    }
};

static const char *teamName(int t) {
    switch (t) {
        case eRedTeam:    return "Red";
        case eGreenTeam:  return "Green";
        case eBlueTeam:   return "Blue";
        case ePurpleTeam: return "Purple";
        case eRogueTeam:  return "Rogue";
        default:          return "Unknown";
    }
}

// -------------------------------------------------------------------- plugin
class HardpointPlugin : public bz_Plugin {
public:
    const char *Name() override { return "Hardpoint"; }
    void Init(const char *config) override;
    void Cleanup() override;
    void Event(bz_EventData *eventData) override;

private:
    HardpointMapHandler mapHandler;

    bool   started    = false;
    int    active     = -1;
    int    lastHolder = -1;      // -1 empty, -2 contested, otherwise team id
    double remaining  = 0;       // holding seconds left before rotation
    double idle       = 0;       // seconds since a team last held the zone
    double pointAccum = 0;       // progress toward the next point
    double lastStep   = 0;
    double nextPulse  = 0;
    float  spin       = 0;

    int    hpScore[5] = {0, 0, 0, 0, 0};   // the only source of team score
    bool   fixScores  = false;             // a death happened: re-assert next tick
    double nextFix    = 0;
    double emptySince = -1;      // when the server became empty, -1 = not empty
    double nextEmptyCheck = 0;
    bool   scoresReset = false;  // already reset for this empty period

    void enforceScores();
    void spawnNearHardpoint(bz_GetPlayerSpawnPosEventData_V1 *d);
    void rotate();
    bool enoughPlayers();
    bool inside(const Hardpoint &hp, const float *p);
    int  evaluateZone(bool &contested);
    void pulseStar();
    void resetGame();
};

BZ_PLUGIN(HardpointPlugin)

void HardpointPlugin::Init(const char *) {
    bz_registerCustomMapObject("HARDPOINT", &mapHandler);

    if (!bz_BZDBItemExists("_hardPointTime"))
        bz_setBZDBDouble("_hardPointTime", 60.0);
    if (!bz_BZDBItemExists("_hardPointIdleTime"))
        bz_setBZDBDouble("_hardPointIdleTime", 0.0);
    if (!bz_BZDBItemExists("_hardPointScoreLimit"))
        bz_setBZDBDouble("_hardPointScoreLimit", 250.0);
    if (!bz_BZDBItemExists("_hardPointMinPerTeam"))
        bz_setBZDBDouble("_hardPointMinPerTeam", 2.0);
    if (!bz_BZDBItemExists("_hardPointResetDelay"))
        bz_setBZDBDouble("_hardPointResetDelay", 30.0);
    if (!bz_BZDBItemExists("_hardPointSpawnRadius"))
        bz_setBZDBDouble("_hardPointSpawnRadius", 100.0);
    if (!bz_BZDBItemExists("_hardPointSpawnTries"))
        bz_setBZDBDouble("_hardPointSpawnTries", 60.0);
    if (!bz_BZDBItemExists("_hardPointStarSpokes"))
        bz_setBZDBDouble("_hardPointStarSpokes", 3.0);
    if (!bz_BZDBItemExists("_hardPointStarInterval"))
        bz_setBZDBDouble("_hardPointStarInterval", 1.0);

    Register(bz_eTickEvent);
    Register(bz_ePlayerDieEvent);
    Register(bz_eGetPlayerSpawnPosEvent);
}

void HardpointPlugin::Cleanup() {
    Flush();
    bz_removeCustomMapObject("HARDPOINT");
    g_points.clear();
}

// Team score = hardpoint points only. Put every team's wins back to our own
// count and clear losses (team kills subtract from team score otherwise).
void HardpointPlugin::enforceScores() {
    const bz_eTeamType teams[] = { eRedTeam, eGreenTeam, eBlueTeam, ePurpleTeam };
    for (bz_eTeamType t : teams) {
        if (bz_getTeamWins(t) != hpScore[t])  bz_setTeamWins(t, hpScore[t]);
        if (bz_getTeamLosses(t) != 0)         bz_setTeamLosses(t, 0);
    }
}

void HardpointPlugin::resetGame() {
    started    = false;
    active     = -1;
    lastHolder = -1;
    remaining  = idle = pointAccum = 0;
}

// At least two teams must each have the minimum number of players.
bool HardpointPlugin::enoughPlayers() {
    const int minPer = (int)bz_getBZDBDouble("_hardPointMinPerTeam");
    const bz_eTeamType teams[] = { eRedTeam, eGreenTeam, eBlueTeam, ePurpleTeam };
    int ready = 0;
    for (bz_eTeamType t : teams)
        if (bz_getTeamCount(t) >= minPer) ready++;
    return ready >= 2;
}

void HardpointPlugin::rotate() {
    if (g_points.empty()) return;
    active     = (active + 1) % (int)g_points.size();
    double t   = bz_getBZDBDouble("_hardPointTime");
    remaining  = t;
    idle       = 0;
    pointAccum = 0;
    lastHolder = -1;

    bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
        "Go to the %s hardpoint! Holding it for %.0f seconds moves it on.",
        g_points[active].name.c_str(), t);
}

bool HardpointPlugin::inside(const Hardpoint &hp, const float *p) {
    // move the point into the hardpoint's own (unrotated) frame
    float dx = p[0] - hp.pos[0];
    float dy = p[1] - hp.pos[1];
    float lx =  dx * hp.cosR + dy * hp.sinR;
    float ly = -dx * hp.sinR + dy * hp.cosR;
    return fabsf(lx) <= hp.size[0] &&
           fabsf(ly) <= hp.size[1] &&
           p[2] >= hp.pos[2] && p[2] <= hp.pos[2] + hp.size[2];
}

// One team alone in the zone = holding. Two or more teams = contested.
int HardpointPlugin::evaluateZone(bool &contested) {
    contested = false;
    int holder = -1;
    const Hardpoint &hp = g_points[active];

    bz_APIIntList *players = bz_getPlayerIndexList();
    if (!players) return -1;

    for (unsigned int i = 0; i < players->size(); i++) {
        bz_BasePlayerRecord *pr = bz_getPlayerByIndex(players->get(i));
        if (!pr) continue;
        if (pr->spawned && pr->team != eObservers &&
            inside(hp, pr->lastKnownState.pos)) {
            if (holder == -1) holder = pr->team;
            else if (holder != pr->team) contested = true;
        }
        bz_freePlayerRecord(pr);
    }
    bz_deleteIntList(players);
    return contested ? -1 : holder;
}

// Spawn players near the active hardpoint, facing it.
//  1. If the hardpoint has `spawn` lines in the map, use one of those (the
//     mapmaker chose them, so they are clear).
//  2. Otherwise ask the server for its own spawn spots with bz_getStandardSpawn
//     (it checks buildings, ground, enemies and dangerous flags) and keep the
//     first one within _hardPointSpawnRadius of the hardpoint, trying up to
//     _hardPointSpawnTries times.
//  3. If none is close enough, leave the server's normal spawn alone.
void HardpointPlugin::spawnNearHardpoint(bz_GetPlayerSpawnPosEventData_V1 *d) {
    const double R = bz_getBZDBDouble("_hardPointSpawnRadius");
    if (!d || R <= 0 || !started || active < 0) return;

    const Hardpoint &hp = g_points[active];

    if (!hp.spawns.empty()) {
        const std::vector<float> &sp = hp.spawns[rand() % hp.spawns.size()];
        d->pos[0] = sp[0];
        d->pos[1] = sp[1];
        d->pos[2] = std::isnan(sp[2]) ? hp.pos[2] : sp[2];
        d->rot    = atan2f(hp.pos[1] - sp[1], hp.pos[0] - sp[0]);
        d->handled = true;
        return;
    }

    const int maxTries = (int)bz_getBZDBDouble("_hardPointSpawnTries");
    const double R2 = R * R;
    for (int i = 0; i < maxTries; i++) {
        float p[3] = {0, 0, 0};
        float rot = 0;
        if (!bz_getStandardSpawn(d->playerID, p, &rot)) continue;

        float dx = p[0] - hp.pos[0];
        float dy = p[1] - hp.pos[1];
        if (dx * dx + dy * dy > R2) continue;

        d->pos[0] = p[0];
        d->pos[1] = p[1];
        d->pos[2] = p[2];
        d->rot    = atan2f(-dy, -dx);              // face the hardpoint
        d->handled = true;
        return;
    }
    // nothing near enough: the server's default spawn stands
}

// Marker star: N shots (default 3, i.e. 120 degrees apart) flying level toward
// the center from 100 units out, 60 units above the hardpoint. They are fired
// in the rogue team's color (yellow), which no team in play uses, so they can't
// be mistaken for a team's shot.
void HardpointPlugin::pulseStar() {
    const Hardpoint &hp = g_points[active];
    int spokes = (int)bz_getBZDBDouble("_hardPointStarSpokes");
    if (spokes < 1) spokes = 1;

    const float dist   = 100.0f;
    const float height = 60.0f;
    const float pi     = 3.14159265f;
    const float life   = dist / (float)bz_getBZDBDouble("_shotSpeed");

    for (int i = 0; i < spokes; i++) {
        float a = spin + i * (2.0f * pi / spokes);
        float p[3] = { hp.pos[0] + dist * cosf(a),
                       hp.pos[1] + dist * sinf(a),
                       hp.pos[2] + height };
        bz_fireWorldWep("PZ", life, BZ_SERVER, p, 0.0f, a + pi, 0, 0.0f, eRogueTeam);
    }
    spin += 0.1f;
}

void HardpointPlugin::Event(bz_EventData *eventData) {
    if (!eventData || g_points.empty())
        return;

    // A kill just changed the team score: take it back on the next tick
    // (and right now, in case the server updates scores before this event).
    if (eventData->eventType == bz_ePlayerDieEvent) {
        enforceScores();
        fixScores = true;
        return;
    }
    if (eventData->eventType == bz_eGetPlayerSpawnPosEvent) {
        spawnNearHardpoint((bz_GetPlayerSpawnPosEventData_V1 *)eventData);
        return;
    }
    if (eventData->eventType != bz_eTickEvent)
        return;

    double now = bz_getCurrentTime();

    // ---- reset team scores once the server has been empty for a while
    if (now >= nextEmptyCheck) {
        nextEmptyCheck = now + 1.0;
        bz_APIIntList *pl = bz_getPlayerIndexList();
        int n = pl ? (int)pl->size() : 0;
        if (pl) bz_deleteIntList(pl);

        if (n > 0) {
            emptySince  = -1;
            scoresReset = false;
        } else {
            if (emptySince < 0) emptySince = now;
            const double delay = bz_getBZDBDouble("_hardPointResetDelay");
            if (delay > 0 && !scoresReset && now - emptySince >= delay) {
                for (int i = 0; i < 5; i++) hpScore[i] = 0;
                resetGame();
                enforceScores();
                scoresReset = true;
            }
        }
    }

    if (fixScores || now >= nextFix) {
        enforceScores();
        fixScores = false;
        nextFix   = now + 0.1;
    }

    // ---- hold everything until enough players are on enough teams
    if (!enoughPlayers()) {
        if (started) {
            resetGame();
            bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS,
                "Hardpoint paused - need at least 2 players on each of 2 teams.");
        }
        return;
    }
    if (!started) {
        started  = true;
        active   = -1;           // rotate() lands on the first hardpoint
        lastStep = now;
        rotate();
    }

    // ---- visual / radar cue (rogue-colored star)
    if (now >= nextPulse) {
        nextPulse = now + bz_getBZDBDouble("_hardPointStarInterval");
        pulseStar();
    }

    // ---- game step (10 per second)
    double dt = now - lastStep;
    if (dt < 0.1) return;
    if (dt > 1.0) dt = 1.0;
    lastStep = now;

    bool contested;
    int team = evaluateZone(contested);

    if (contested) {
        if (lastHolder != -2) {
            bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS, "Hardpoint contested!");
            lastHolder = -2;
            pointAccum = 0;
        }
        idle += dt;                       // timer paused
    } else if (team != -1) {
        if (team != lastHolder) {
            bz_sendTextMessagef(BZ_SERVER, BZ_ALLUSERS,
                "%s team is holding the hardpoint", teamName(team));
            lastHolder = team;
            pointAccum = 0;               // no partial credit across teams
        }
        idle = 0;
        remaining  -= dt;                 // timer runs only while held
        pointAccum += dt;

        while (pointAccum >= 1.0) {       // COD: 1 point per second
            pointAccum -= 1.0;
            bz_eTeamType t = (bz_eTeamType)team;
            hpScore[team]++;
            enforceScores();
            if (hpScore[team] >= (int)bz_getBZDBDouble("_hardPointScoreLimit")) {
                resetGame();
                bz_gameOver(BZ_SERVER, t);
                for (int i = 0; i < 5; i++) hpScore[i] = 0;
                return;
            }
        }
    } else {
        lastHolder = -1;                  // empty: timer paused
        pointAccum = 0;
        idle += dt;
    }

    // ---- rotation
    const double idleLimit = bz_getBZDBDouble("_hardPointIdleTime");
    if (remaining <= 0) {
        rotate();
    } else if (idleLimit > 0 && idle >= idleLimit) {
        bz_sendTextMessage(BZ_SERVER, BZ_ALLUSERS,
            "Nobody took the hardpoint - moving on.");
        rotate();
    }
}
