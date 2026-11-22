#include "global.h"
#include "event_data.h"
#include "field_door.h"
#include "field_camera.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "task.h"
#include "constants/songs.h"
#include "constants/metatile_labels.h"

#define DOOR_SOUND_NORMAL  0
#define DOOR_SOUND_SLIDING 1
#define DOOR_SOUND_ARENA   2

// PS-098: door sizes (Emerald's own table uses the bare numbers 1 and 2). DOOR_SIZE_1x1 is pokefirered's small
// door: three 16x16 frames of 4 tiles, only the door metatile is redrawn.
#define DOOR_SIZE_1x1 0
#define DOOR_SIZE_1x2 1
#define DOOR_SIZE_2x2 2

struct DoorGraphics
{
    u16 metatileNum;
    u8 sound;
    u8 size;
    const void *tiles;
    const void *palettes;
};

struct DoorAnimFrame
{
    u8 time;
    u16 offset;
};

static bool8 ShouldUseMultiCorridorDoor(void);

static const u8 sDoorAnimTiles_Littleroot[] = INCGFX_U8("graphics/door_anims/littleroot.png", ".4bpp");
static const u16 sDoorNullPalette1[16] = {};
static const u8 sDoorAnimTiles_BirchsLab[] = INCGFX_U8("graphics/door_anims/birchs_lab.png", ".4bpp");
static const u16 sDoorNullPalette2[16] = {};
static const u8 sDoorAnimTiles_FallarborLightRoof[] = INCGFX_U8("graphics/door_anims/fallarbor_light_roof.png", ".4bpp");
static const u16 sDoorNullPalette3[16] = {};
static const u8 sDoorAnimTiles_Lilycove[] = INCGFX_U8("graphics/door_anims/lilycove.png", ".4bpp");
static const u16 sDoorNullPalette4[16] = {};
static const u8 sDoorAnimTiles_LilycoveWooden[] = INCGFX_U8("graphics/door_anims/lilycove_wooden.png", ".4bpp");
static const u16 sDoorNullPalette5[16] = {};
static const u8 sDoorAnimTiles_General[] = INCGFX_U8("graphics/door_anims/general.png", ".4bpp");
static const u16 sDoorNullPalette6[16] = {};
static const u8 sDoorAnimTiles_PokeCenter[] = INCGFX_U8("graphics/door_anims/poke_center.png", ".4bpp");
static const u8 sDoorAnimTiles_Gym[] = INCGFX_U8("graphics/door_anims/gym.png", ".4bpp");
static const u8 sDoorAnimTiles_PokeMart[] = INCGFX_U8("graphics/door_anims/poke_mart.png", ".4bpp");
static const u8 sDoorAnimTiles_RustboroTan[] = INCGFX_U8("graphics/door_anims/rustboro_tan.png", ".4bpp");
static const u16 sDoorNullPalette7[16] = {};
static const u8 sDoorAnimTiles_RustboroGray[] = INCGFX_U8("graphics/door_anims/rustboro_gray.png", ".4bpp");
static const u16 sDoorNullPalette8[16] = {};
static const u8 sDoorAnimTiles_Oldale[] = INCGFX_U8("graphics/door_anims/oldale.png", ".4bpp");
static const u8 sFiller1[0x5900] = {};
static const u8 sDoorAnimTiles_UnusedTops[] = INCGFX_U8("graphics/door_anims/unused_top.png", ".4bpp");
static const u8 sFiller2[0x140] = {};
static const u8 sDoorAnimTiles_UnusedBottoms[] = INCGFX_U8("graphics/door_anims/unused_bottom.png", ".4bpp");
static const u16 sDoorNullPalette11[16] = {};
static const u8 sDoorAnimTiles_Mauville[] = INCGFX_U8("graphics/door_anims/mauville.png", ".4bpp");
static const u16 sDoorNullPalette12[16] = {};
static const u8 sDoorAnimTiles_Verdanturf[] = INCGFX_U8("graphics/door_anims/verdanturf.png", ".4bpp");
static const u16 sDoorNullPalette13[16] = {};
static const u8 sDoorAnimTiles_Slateport[] = INCGFX_U8("graphics/door_anims/slateport.png", ".4bpp");
static const u16 sDoorNullPalette14[16] = {};
static const u8 sDoorAnimTiles_Dewford[] = INCGFX_U8("graphics/door_anims/dewford.png", ".4bpp");
static const u16 sDoorNullPalette15[16] = {};
static const u8 sDoorAnimTiles_Contest[] = INCGFX_U8("graphics/door_anims/contest.png", ".4bpp");
static const u16 sDoorNullPalette16[16] = {};
static const u8 sDoorAnimTiles_Mossdeep[] = INCGFX_U8("graphics/door_anims/mossdeep.png", ".4bpp");
static const u16 sDoorNullPalette17[16] = {};
static const u8 sDoorAnimTiles_SootopolisPeakedRoof[] = INCGFX_U8("graphics/door_anims/sootopolis_peaked_roof.png", ".4bpp");
static const u16 sDoorNullPalette18[16] = {};
static const u8 sDoorAnimTiles_Sootopolis[] = INCGFX_U8("graphics/door_anims/sootopolis.png", ".4bpp");
static const u16 sDoorNullPalette19[16] = {};
static const u8 sDoorAnimTiles_PokemonLeague[] = INCGFX_U8("graphics/door_anims/pokemon_league.png", ".4bpp");
static const u16 sDoorNullPalette20[16] = {};
static const u8 sDoorAnimTiles_Pacifidlog[] = INCGFX_U8("graphics/door_anims/pacifidlog.png", ".4bpp");
static const u16 sDoorNullPalette21[16] = {};
static const u8 sDoorAnimTiles_PetalburgGym[] = INCGFX_U8("graphics/door_anims/petalburg_gym.png", ".4bpp");
static const u16 sDoorNullPalette22[16] = {};
static const u8 sDoorAnimTiles_CyclingRoad[] = INCGFX_U8("graphics/door_anims/cycling_road.png", ".4bpp");
static const u16 sDoorNullPalette23[16] = {};
static const u8 sDoorAnimTiles_LilycoveDeptStore[] = INCGFX_U8("graphics/door_anims/lilycove_dept_store.png", ".4bpp");
static const u16 sDoorNullPalette24[16] = {};
static const u8 sDoorAnimTiles_SafariZone[] = INCGFX_U8("graphics/door_anims/safari_zone.png", ".4bpp");
static const u16 sDoorNullPalette25[16] = {};
static const u8 sDoorAnimTiles_MossdeepSpaceCenter[] = INCGFX_U8("graphics/door_anims/mossdeep_space_center.png", ".4bpp");
static const u16 sDoorNullPalette26[16] = {};
static const u8 sDoorAnimTiles_CableClub[] = INCGFX_U8("graphics/door_anims/cable_club.png", ".4bpp");
static const u16 sDoorNullPalette27[16] = {};
static const u8 sDoorAnimTiles_AbandonedShip[] = INCGFX_U8("graphics/door_anims/abandoned_ship.png", ".4bpp");
static const u16 sDoorNullPalette28[16] = {};
static const u8 sDoorAnimTiles_FallarborDarkRoof[] = INCGFX_U8("graphics/door_anims/fallarbor_dark_roof.png", ".4bpp");
static const u16 sDoorNullPalette29[16] = {};
static const u8 sDoorAnimTiles_AbandonedShipRoom[] = INCGFX_U8("graphics/door_anims/abandoned_ship_room.png", ".4bpp");
static const u16 sDoorNullPalette30[16] = {};
static const u8 sDoorAnimTiles_LilycoveDeptStoreElevator[] = INCGFX_U8("graphics/door_anims/lilycove_dept_store_elevator.png", ".4bpp");
static const u16 sDoorNullPalette31[16] = {};
static const u8 sDoorAnimTiles_BattleTowerOld[] = INCGFX_U8("graphics/door_anims/battle_tower_old.png", ".4bpp");
static const u16 sDoorNullPalette32[16] = {};
static const u8 sDoorAnimTiles_BattleTowerElevator[] = INCGFX_U8("graphics/door_anims/battle_tower_elevator.png", ".4bpp");
static const u16 sDoorNullPalette33[16] = {};
static const u8 sDoorAnimTiles_UnusedBattleFrontier[] = INCGFX_U8("graphics/door_anims/unused_battle_frontier.png", ".4bpp");
static const u16 sDoorNullPalette34[16] = {};
static const u8 sDoorAnimTiles_BattleDome[] = INCGFX_U8("graphics/door_anims/battle_dome.png", ".4bpp");
static const u16 sDoorNullPalette35[16] = {};
static const u8 sDoorAnimTiles_BattleFactory[] = INCGFX_U8("graphics/door_anims/battle_factory.png", ".4bpp");
static const u16 sDoorNullPalette36[16] = {};
static const u8 sDoorAnimTiles_BattleTower[] = INCGFX_U8("graphics/door_anims/battle_tower.png", ".4bpp");
static const u16 sDoorNullPalette37[16] = {};
static const u8 sDoorAnimTiles_BattleArena[] = INCGFX_U8("graphics/door_anims/battle_arena.png", ".4bpp");
static const u16 sDoorNullPalette38[16] = {};
static const u8 sDoorAnimTiles_BattleArenaLobby[] = INCGFX_U8("graphics/door_anims/battle_arena_lobby.png", ".4bpp");
static const u16 sDoorNullPalette39[16] = {};
static const u8 sDoorAnimTiles_BattleDomeLobby[] = INCGFX_U8("graphics/door_anims/battle_dome_lobby.png", ".4bpp");
static const u16 sDoorNullPalette40[16] = {};
static const u8 sDoorAnimTiles_BattlePalaceLobby[] = INCGFX_U8("graphics/door_anims/battle_palace_lobby.png", ".4bpp");
static const u8 sDoorAnimTiles_BattleTent[] = INCGFX_U8("graphics/door_anims/battle_tent.png", ".4bpp");
static const u16 sDoorNullPalette41[16] = {};
static const u8 sDoorAnimTiles_BattleDomeCorridor[] = INCGFX_U8("graphics/door_anims/battle_dome_corridor.png", ".4bpp");
static const u16 sDoorNullPalette42[16] = {};
static const u8 sDoorAnimTiles_BattleTowerMultiCorridor[] = INCGFX_U8("graphics/door_anims/battle_tower_multi_corridor.png", ".4bpp", "-mwidth 2 -mheight 4");
static const u16 sDoorNullPalette43[16] = {};
static const u8 sDoorAnimTiles_BattleFrontier[] = INCGFX_U8("graphics/door_anims/battle_frontier.png", ".4bpp");
static const u16 sDoorNullPalette44[16] = {};
static const u8 sDoorAnimTiles_BattleFrontierSliding[] = INCGFX_U8("graphics/door_anims/battle_frontier_sliding.png", ".4bpp");
static const u16 sDoorNullPalette45[16] = {};
static const u8 sDoorAnimTiles_BattleDomePreBattleRoom[] = INCGFX_U8("graphics/door_anims/battle_dome_pre_battle_room.png", ".4bpp");
static const u16 sDoorNullPalette46[16] = {};
static const u8 sDoorAnimTiles_BattleTentInterior[] = INCGFX_U8("graphics/door_anims/battle_tent_interior.png", ".4bpp");
static const u16 sDoorNullPalette47[16] = {};
static const u8 sDoorAnimTiles_TrainerHillLobbyElevator[] = INCGFX_U8("graphics/door_anims/trainer_hill_lobby_elevator.png", ".4bpp");
static const u16 sDoorNullPalette48[16] = {};
static const u8 sDoorAnimTiles_TrainerHillRoofElevator[] = INCGFX_U8("graphics/door_anims/trainer_hill_roof_elevator.png", ".4bpp");
static const u16 sDoorNullPalette49[16] = {};

static const struct DoorAnimFrame sDoorOpenAnimFrames[] =
{
    {4, -1},
    {4, 0},
    {4, 0x100},
    {4, 0x200},
    {0, 0},
};

static const struct DoorAnimFrame sDoorCloseAnimFrames[] =
{
    {4, 0x200},
    {4, 0x100},
    {4, 0},
    {4, -1},
    {0, 0},
};

static const struct DoorAnimFrame sBigDoorOpenAnimFrames[] =
{
    {4, -1},
    {4, 0},
    {4, 0x200},
    {4, 0x400},
    {0, 0},
};

static const struct DoorAnimFrame sBigDoorCloseAnimFrames[] =
{
    {4, 0x400},
    {4, 0x200},
    {4, 0},
    {4, -1},
    {0, 0},
};

// PS-098: DOOR_SIZE_1x1 frames (4 tiles = 0x80 bytes each)
static const struct DoorAnimFrame sSmallDoorOpenAnimFrames[] =
{
    {4, -1},
    {4, 0},
    {4, 0x80},
    {4, 0x100},
    {0, 0},
};

static const struct DoorAnimFrame sSmallDoorCloseAnimFrames[] =
{
    {4, 0x100},
    {4, 0x80},
    {4, 0},
    {4, -1},
    {0, 0},
};

static const u8 sDoorAnimPalettes_General[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_PokeCenter[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_Gym[] = {5, 5, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_PokeMart[] = {0, 0, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_Littleroot[] = {10, 10, 6, 6, 6, 6, 6, 6};
static const u8 sDoorAnimPalettes_BirchsLab[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_RustboroTan[] = {11, 11, 11, 11, 11, 11, 11, 11};
static const u8 sDoorAnimPalettes_RustboroGray[] = {10, 10, 10, 10, 10, 10, 10, 10};
static const u8 sDoorAnimPalettes_FallarborLightRoof[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_Lilycove[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_Oldale[] = {10, 10, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_Mossdeep[] = {9, 9, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_PokemonLeague[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_Pacifidlog[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_SootopolisPeakedRoof[] = {6, 6, 6, 6, 6, 6, 6, 6};
static const u8 sDoorAnimPalettes_Sootopolis[] = {6, 6, 6, 6, 6, 6, 6, 6};
static const u8 sDoorAnimPalettes_Dewford[] = {0, 0, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_Slateport[] = {6, 6, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_Mauville[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_Verdanturf[] = {6, 6, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_LilycoveWooden[] = {5, 5, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_Contest[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_PetalburgGym[] = {6, 6, 6, 6, 6, 6, 6, 6};
static const u8 sDoorAnimPalettes_CyclingRoad[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_LilycoveDeptStore[] = {5, 5, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_SafariZone[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_MossdeepSpaceCenter[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_CableClub[] = {6, 6, 6, 6, 6, 6, 6, 6};
static const u8 sDoorAnimPalettes_AbandonedShip[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_FallarborDarkRoof[] = {11, 11, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_AbandonedShipRoom[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_LilycoveDeptStoreElevator[] = {6, 6, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattleTowerOld[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_BattleTowerElevator[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_UnusedBattleFrontier[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_BattleDome[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_BattleFactory[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_BattleTower[] = {0, 0, 0, 0, 0, 0, 0, 0};
static const u8 sDoorAnimPalettes_BattleArena[] = {5, 5, 5, 5, 5, 5, 5, 5};
static const u8 sDoorAnimPalettes_BattleArenaLobby[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattleDomeLobby[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattlePalaceLobby[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattleTent[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_BattleDomeCorridor[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattleTowerMultiCorridor[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_Unused[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_BattleFrontier[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const u8 sDoorAnimPalettes_BattleDomePreBattleRoom[] = {9, 9, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_BattleTentInterior[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_TrainerHillLobbyElevator[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_TrainerHillRoofElevator[] = {9, 9, 7, 7, 7, 7, 7, 7};

static const struct DoorGraphics sDoorAnimGraphicsTable[] =
{
    {METATILE_General_Door,                                 DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_General, sDoorAnimPalettes_General},
    {METATILE_General_Door_PokeCenter,                      DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_PokeCenter, sDoorAnimPalettes_PokeCenter},
    {METATILE_General_Door_Gym,                             DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_Gym, sDoorAnimPalettes_Gym},
    {METATILE_General_Door_PokeMart,                        DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_PokeMart, sDoorAnimPalettes_PokeMart},
    {METATILE_Petalburg_Door_Littleroot,                    DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Littleroot, sDoorAnimPalettes_Littleroot},
    {METATILE_Petalburg_Door_BirchsLab,                     DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_BirchsLab, sDoorAnimPalettes_BirchsLab},
    {METATILE_Rustboro_Door_Tan,                            DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_RustboroTan, sDoorAnimPalettes_RustboroTan},
    {METATILE_Rustboro_Door_Gray,                           DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_RustboroGray, sDoorAnimPalettes_RustboroGray},
    {METATILE_Fallarbor_Door_LightRoof,                     DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_FallarborLightRoof, sDoorAnimPalettes_FallarborLightRoof},
    {METATILE_Petalburg_Door_Oldale,                        DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Oldale, sDoorAnimPalettes_Oldale},
    {METATILE_Mauville_Door,                                DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Mauville, sDoorAnimPalettes_Mauville},
    {METATILE_Mauville_Door_Verdanturf,                     DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Verdanturf, sDoorAnimPalettes_Verdanturf},
    {METATILE_Slateport_Door,                               DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Slateport, sDoorAnimPalettes_Slateport},
    {METATILE_Dewford_Door,                                 DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Dewford, sDoorAnimPalettes_Dewford},
    {METATILE_General_Door_Contest,                         DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_Contest, sDoorAnimPalettes_Contest},
    {METATILE_Lilycove_Door,                                DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Lilycove, sDoorAnimPalettes_Lilycove},
    {METATILE_Lilycove_Door_Wooden,                         DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_LilycoveWooden, sDoorAnimPalettes_LilycoveWooden},
    {METATILE_Mossdeep_Door,                                DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Mossdeep, sDoorAnimPalettes_Mossdeep},
    {METATILE_Sootopolis_Door_PeakedRoof,                   DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_SootopolisPeakedRoof, sDoorAnimPalettes_SootopolisPeakedRoof},
    {METATILE_Sootopolis_Door,                              DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Sootopolis, sDoorAnimPalettes_Sootopolis},
    {METATILE_EverGrande_Door_PokemonLeague,                DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_PokemonLeague, sDoorAnimPalettes_PokemonLeague},
    {METATILE_Pacifidlog_Door,                              DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_Pacifidlog, sDoorAnimPalettes_Pacifidlog},
    {METATILE_PetalburgGym_Door,                            DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_PetalburgGym, sDoorAnimPalettes_PetalburgGym},
    {METATILE_Mauville_Door_CyclingRoad,                    DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_CyclingRoad, sDoorAnimPalettes_CyclingRoad},
    {METATILE_Lilycove_Door_DeptStore,                      DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_LilycoveDeptStore, sDoorAnimPalettes_LilycoveDeptStore},
    {METATILE_Lilycove_Door_SafariZone,                     DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_SafariZone, sDoorAnimPalettes_SafariZone},
    {METATILE_Mossdeep_Door_SpaceCenter,                    DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_MossdeepSpaceCenter, sDoorAnimPalettes_MossdeepSpaceCenter},
    {METATILE_PokemonCenter_Door_CableClub,                 DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_CableClub, sDoorAnimPalettes_CableClub},
    {METATILE_InsideShip_IntactDoor_Bottom_Unlocked,        DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_AbandonedShip, sDoorAnimPalettes_AbandonedShip},
    {METATILE_Fallarbor_Door_DarkRoof,                      DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_FallarborDarkRoof, sDoorAnimPalettes_FallarborDarkRoof},
    {METATILE_InsideShip_IntactDoor_Bottom_Interior,        DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_AbandonedShipRoom, sDoorAnimPalettes_AbandonedShipRoom},
    {METATILE_Shop_Door_Elevator,                           DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_LilycoveDeptStoreElevator, sDoorAnimPalettes_LilycoveDeptStoreElevator},
    {METATILE_Dewford_Door_BattleTower,                     DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTowerOld, sDoorAnimPalettes_BattleTowerOld},
    {METATILE_BattleFrontier_Door_Elevator,                 DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTowerElevator, sDoorAnimPalettes_BattleTowerElevator},
    // The metatile for this door doesn't seem to correspond to a door in any Emerald tileset. Given the surrounding door animations, it was likely cut from the Battle Frontier.
    // From the palettes array we know it uses palette 9, and the door's shadow looks correct using either the Battle Tent or Battle Frontier Outside's 9th palette.
    {0x3B0,                                                 DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_UnusedBattleFrontier, sDoorAnimPalettes_UnusedBattleFrontier},
    {METATILE_BattleFrontierOutsideWest_Door_BattleDome,    DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleDome, sDoorAnimPalettes_BattleDome},
    {METATILE_BattleFrontierOutsideWest_Door_BattleFactory, DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleFactory, sDoorAnimPalettes_BattleFactory},
    {METATILE_BattleFrontierOutsideEast_Door_BattleTower,   DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTower, sDoorAnimPalettes_BattleTower},
    {METATILE_BattleFrontierOutsideEast_Door_BattleArena,   DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_BattleArena, sDoorAnimPalettes_BattleArena},
    {METATILE_BattleArena_Door,                             DOOR_SOUND_ARENA,   1, sDoorAnimTiles_BattleArenaLobby, sDoorAnimPalettes_BattleArenaLobby},
    {METATILE_BattleDome_Door_Lobby,                        DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleDomeLobby, sDoorAnimPalettes_BattleDomeLobby},
    {METATILE_BattlePalace_Door,                            DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_BattlePalaceLobby, sDoorAnimPalettes_BattlePalaceLobby},
    {METATILE_Slateport_Door_BattleTent,                    DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTent, sDoorAnimPalettes_BattleTent},
    {METATILE_Mauville_Door_BattleTent,                     DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTent, sDoorAnimPalettes_BattleTent},
    {METATILE_Fallarbor_Door_BattleTent,                    DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTent, sDoorAnimPalettes_BattleTent},
    {METATILE_BattleDome_Door_Corridor,                     DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleDomeCorridor, sDoorAnimPalettes_BattleDomeCorridor},
    {METATILE_BattleFrontier_Door_MultiCorridor,            DOOR_SOUND_SLIDING, 2, sDoorAnimTiles_BattleTowerMultiCorridor, sDoorAnimPalettes_BattleTowerMultiCorridor},
    {METATILE_BattleFrontierOutsideWest_Door,               DOOR_SOUND_NORMAL,  1, sDoorAnimTiles_BattleFrontier, sDoorAnimPalettes_BattleFrontier},
    {METATILE_BattleFrontierOutsideWest_Door_Sliding,       DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleFrontierSliding, sDoorAnimPalettes_BattleFrontier},
    {METATILE_BattleDome_Door_PreBattleRoom,                DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleDomePreBattleRoom, sDoorAnimPalettes_BattleDomePreBattleRoom},
    {METATILE_BattleTent_Door,                              DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_BattleTentInterior, sDoorAnimPalettes_BattleTentInterior},
    {METATILE_TrainerHill_Door_Elevator_Lobby,              DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_TrainerHillLobbyElevator, sDoorAnimPalettes_TrainerHillLobbyElevator},
    {METATILE_TrainerHill_Door_Elevator_Roof,               DOOR_SOUND_SLIDING, 1, sDoorAnimTiles_TrainerHillRoofElevator, sDoorAnimPalettes_TrainerHillRoofElevator},
    {},
};

// PS-098: Kanto doors: pokefirered's door table for the imported maps (its Sevii Islands, Trainer Tower
// and Sea Cottage entries are left out). The sheets are copied by tools/kanto_import into
// graphics/door_anims/kanto/; the metatile ids and palette slots are pokefirered's own (the converter's
// prefix re-pack keeps both). DOOR_SIZE_1x1 doors have three 16x16 frames (4 tiles each) and redraw only
// the door metatile; DOOR_SIZE_1x2 is Emerald's size 1 (top + bottom metatile, 8 tiles per frame).
static const u8 sDoorAnimTiles_KantoGeneral[] = INCGFX_U8("graphics/door_anims/kanto/general.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSlidingSingle[] = INCGFX_U8("graphics/door_anims/kanto/sliding_single.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSlidingDouble[] = INCGFX_U8("graphics/door_anims/kanto/sliding_double.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoPallet[] = INCGFX_U8("graphics/door_anims/kanto/pallet.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoOaksLab[] = INCGFX_U8("graphics/door_anims/kanto/oaks_lab.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoViridian[] = INCGFX_U8("graphics/door_anims/kanto/viridian.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoPewter[] = INCGFX_U8("graphics/door_anims/kanto/pewter.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSaffron[] = INCGFX_U8("graphics/door_anims/kanto/saffron.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSilphCo[] = INCGFX_U8("graphics/door_anims/kanto/silph_co.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoCerulean[] = INCGFX_U8("graphics/door_anims/kanto/cerulean.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoLavender[] = INCGFX_U8("graphics/door_anims/kanto/lavender.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoVermilion[] = INCGFX_U8("graphics/door_anims/kanto/vermilion.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoPokemonFanClub[] = INCGFX_U8("graphics/door_anims/kanto/pokemon_fan_club.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoDeptStore[] = INCGFX_U8("graphics/door_anims/kanto/dept_store.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoFuchsia[] = INCGFX_U8("graphics/door_anims/kanto/fuchsia.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSafariZone[] = INCGFX_U8("graphics/door_anims/kanto/safari_zone.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoCinnabarLab[] = INCGFX_U8("graphics/door_anims/kanto/cinnabar_lab.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoDeptStoreElevator[] = INCGFX_U8("graphics/door_anims/kanto/dept_store_elevator.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoCableClub[] = INCGFX_U8("graphics/door_anims/kanto/cable_club.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoHideoutElevator[] = INCGFX_U8("graphics/door_anims/kanto/hideout_elevator.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSSAnne[] = INCGFX_U8("graphics/door_anims/kanto/ss_anne.png", ".4bpp");
static const u8 sDoorAnimTiles_KantoSilphCoElevator[] = INCGFX_U8("graphics/door_anims/kanto/silph_co_elevator.png", ".4bpp");

static const u8 sDoorAnimPalettes_KantoGeneral[] = {2, 2, 2, 2, 2, 2, 2, 2};
static const u8 sDoorAnimPalettes_KantoSlidingSingle[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u8 sDoorAnimPalettes_KantoSlidingDouble[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u8 sDoorAnimPalettes_KantoPallet[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoOaksLab[] = {10, 10, 10, 10, 10, 10, 10, 10};
static const u8 sDoorAnimPalettes_KantoViridian[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoPewter[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoSaffron[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoSilphCo[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u8 sDoorAnimPalettes_KantoCerulean[] = {12, 12, 12, 12, 12, 12, 12, 12};
static const u8 sDoorAnimPalettes_KantoLavender[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_KantoVermilion[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_KantoPokemonFanClub[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_KantoDeptStore[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u8 sDoorAnimPalettes_KantoFuchsia[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoSafariZone[] = {9, 9, 9, 9, 9, 9, 9, 9};
static const u8 sDoorAnimPalettes_KantoCinnabarLab[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u8 sDoorAnimPalettes_KantoDeptStoreElevator[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoCableClub[] = {8, 8, 8, 8, 8, 8, 8, 8};
static const u8 sDoorAnimPalettes_KantoHideoutElevator[] = {12, 12, 2, 2, 2, 2, 2, 2};
static const u8 sDoorAnimPalettes_KantoSSAnne[] = {7, 7, 7, 7, 7, 7, 7, 7};
static const u8 sDoorAnimPalettes_KantoSilphCoElevator[] = {8, 8, 2, 2, 2, 2, 2, 2};

static const struct DoorGraphics sKantoDoorAnimGraphicsTable[] =
{
    {METATILE_KantoGeneral_Door,                  DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoGeneral, sDoorAnimPalettes_KantoGeneral},
    {METATILE_KantoGeneral_SlidingSingleDoor,     DOOR_SOUND_SLIDING, DOOR_SIZE_1x1, sDoorAnimTiles_KantoSlidingSingle, sDoorAnimPalettes_KantoSlidingSingle},
    {METATILE_KantoGeneral_SlidingDoubleDoor,     DOOR_SOUND_SLIDING, DOOR_SIZE_1x1, sDoorAnimTiles_KantoSlidingDouble, sDoorAnimPalettes_KantoSlidingDouble},
    {METATILE_KantoPalletTown_Door,               DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoPallet, sDoorAnimPalettes_KantoPallet},
    {METATILE_KantoPalletTown_OaksLabDoor,        DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoOaksLab, sDoorAnimPalettes_KantoOaksLab},
    {METATILE_KantoViridianCity_Door,             DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoViridian, sDoorAnimPalettes_KantoViridian},
    {METATILE_KantoPewterCity_Door,               DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoPewter, sDoorAnimPalettes_KantoPewter},
    {METATILE_KantoSaffronCity_Door,              DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoSaffron, sDoorAnimPalettes_KantoSaffron},
    {METATILE_KantoSaffronCity_SilphCoDoor,       DOOR_SOUND_SLIDING, DOOR_SIZE_1x1, sDoorAnimTiles_KantoSilphCo, sDoorAnimPalettes_KantoSilphCo},
    {METATILE_KantoCeruleanCity_Door,             DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoCerulean, sDoorAnimPalettes_KantoCerulean},
    {METATILE_KantoLavenderTown_Door,             DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoLavender, sDoorAnimPalettes_KantoLavender},
    {METATILE_KantoVermilionCity_Door,            DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoVermilion, sDoorAnimPalettes_KantoVermilion},
    {METATILE_KantoVermilionCity_SSAnneWarp,      DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoPokemonFanClub, sDoorAnimPalettes_KantoPokemonFanClub},
    {METATILE_KantoCeladonCity_DeptStoreDoor,     DOOR_SOUND_SLIDING, DOOR_SIZE_1x1, sDoorAnimTiles_KantoDeptStore, sDoorAnimPalettes_KantoDeptStore},
    {METATILE_KantoFuchsiaCity_Door,              DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoFuchsia, sDoorAnimPalettes_KantoFuchsia},
    {METATILE_KantoFuchsiaCity_SafariZoneDoor,    DOOR_SOUND_SLIDING, DOOR_SIZE_1x1, sDoorAnimTiles_KantoSafariZone, sDoorAnimPalettes_KantoSafariZone},
    {METATILE_KantoCinnabarIsland_LabDoor,        DOOR_SOUND_NORMAL,  DOOR_SIZE_1x1, sDoorAnimTiles_KantoCinnabarLab, sDoorAnimPalettes_KantoCinnabarLab},
    {METATILE_KantoDepartmentStore_ElevatorDoor,  DOOR_SOUND_SLIDING, DOOR_SIZE_1x2, sDoorAnimTiles_KantoDeptStoreElevator, sDoorAnimPalettes_KantoDeptStoreElevator},
    {METATILE_KantoPokemonCenter_CableClubDoor,   DOOR_SOUND_SLIDING, DOOR_SIZE_1x2, sDoorAnimTiles_KantoCableClub, sDoorAnimPalettes_KantoCableClub},
    {METATILE_KantoSilphCo_HideoutElevatorDoor,   DOOR_SOUND_SLIDING, DOOR_SIZE_1x2, sDoorAnimTiles_KantoHideoutElevator, sDoorAnimPalettes_KantoHideoutElevator},
    {METATILE_KantoSSAnne_Door,                   DOOR_SOUND_NORMAL,  DOOR_SIZE_1x2, sDoorAnimTiles_KantoSSAnne, sDoorAnimPalettes_KantoSSAnne},
    {METATILE_KantoSilphCo_ElevatorDoor,          DOOR_SOUND_SLIDING, DOOR_SIZE_1x2, sDoorAnimTiles_KantoSilphCoElevator, sDoorAnimPalettes_KantoSilphCoElevator},
    {},
};

// PS-098: door tables per primary tileset. sDoorAnimGraphicsTable is keyed by metatile id only and was used
// for every map: a Kanto door whose id equals an Emerald door's (Oak's lab door 0x2AC is Emerald's
// METATILE_Mauville_Door) drew Emerald's frames over Kanto tiles, every other Kanto door did nothing. A map
// whose primary tileset is not listed uses Emerald's table, as before.
extern const struct Tileset gTileset_KantoGeneral;
extern const struct Tileset gTileset_KantoBuilding;

struct DoorAnimTableForTileset
{
    const struct Tileset *primaryTileset;
    const struct DoorGraphics *table;
};

static const struct DoorAnimTableForTileset sDoorAnimTablesByPrimaryTileset[] =
{
    { &gTileset_KantoGeneral,  sKantoDoorAnimGraphicsTable },
    { &gTileset_KantoBuilding, sKantoDoorAnimGraphicsTable },
};

static const struct DoorGraphics *GetDoorAnimGraphicsTable(void)
{
    const struct Tileset *primaryTileset = gMapHeader.mapLayout->primaryTileset;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sDoorAnimTablesByPrimaryTileset); i++)
    {
        if (sDoorAnimTablesByPrimaryTileset[i].primaryTileset == primaryTileset)
            return sDoorAnimTablesByPrimaryTileset[i].table;
    }
    return sDoorAnimGraphicsTable;
}

// NOTE: The tiles of a door's animation must be copied to VRAM because they are not already part of any given tileset.
//       This means that if there are any pre-existing tiles in this copied region that are visible when the door
//       animation is played they will be overwritten.
#define DOOR_TILE_START_SIZE1 (NUM_TILES_TOTAL - 8)
#define DOOR_TILE_START_SIZE2 (NUM_TILES_TOTAL - 16)

static void CopyDoorTilesToVram(const struct DoorGraphics *gfx, const struct DoorAnimFrame *frame)
{
    if (gfx->size == DOOR_SIZE_2x2)
        CpuFastCopy(gfx->tiles + frame->offset, (void *)(VRAM + TILE_OFFSET_4BPP(DOOR_TILE_START_SIZE2)), 16 * TILE_SIZE_4BPP);
    else if (gfx->size == DOOR_SIZE_1x1) // PS-098: one metatile per frame
        CpuFastCopy(gfx->tiles + frame->offset, (void *)(VRAM + TILE_OFFSET_4BPP(DOOR_TILE_START_SIZE1)), 4 * TILE_SIZE_4BPP);
    else
        CpuFastCopy(gfx->tiles + frame->offset, (void *)(VRAM + TILE_OFFSET_4BPP(DOOR_TILE_START_SIZE1)), 8 * TILE_SIZE_4BPP);
}

static void BuildDoorTiles(u16 *tiles, u16 tileNum, const u8 *paletteNums)
{
    int i;
    u16 tile;

    // Only the first 4 tiles of each metatile (bottom layer) actually use the door tiles
    for (i = 0; i < 4; i++)
    {
        tile = *(paletteNums++) << 12;
        tiles[i] = tile | (tileNum + i);
    }

    // The remaining layers are left as tile 0 (with the same palette)
    for (; i < 8; i++)
    {
        tile = *(paletteNums++) << 12;
        tiles[i] = tile;
    }
}

static void DrawCurrentDoorAnimFrame(const struct DoorGraphics *gfx, u32 x, u32 y, const u8 *paletteNums)
{
    u16 tiles[24];

    if (gfx->size == DOOR_SIZE_2x2)
    {
        // Top left metatile
        BuildDoorTiles(&tiles[8], DOOR_TILE_START_SIZE2 + 0, &paletteNums[0]);
        DrawDoorMetatileAt(x, y - 1, &tiles[8]);

        // Bottom left metatile
        BuildDoorTiles(&tiles[8], DOOR_TILE_START_SIZE2 + 4, &paletteNums[4]);
        DrawDoorMetatileAt(x, y, &tiles[8]);

        // Top right metatile
        BuildDoorTiles(&tiles[8], DOOR_TILE_START_SIZE2 + 8, &paletteNums[0]);
        DrawDoorMetatileAt(x + 1, y - 1, &tiles[8]);

        // Bottom right metatile
        BuildDoorTiles(&tiles[8], DOOR_TILE_START_SIZE2 + 12, &paletteNums[4]);
        DrawDoorMetatileAt(x + 1, y, &tiles[8]);
    }
    else if (gfx->size == DOOR_SIZE_1x1)
    {
        // PS-098: the door metatile only
        BuildDoorTiles(&tiles[0], DOOR_TILE_START_SIZE1 + 0, &paletteNums[0]);
        DrawDoorMetatileAt(x, y, &tiles[0]);
    }
    else
    {
        // Top metatile
        BuildDoorTiles(&tiles[0], DOOR_TILE_START_SIZE1 + 0, &paletteNums[0]);
        DrawDoorMetatileAt(x, y - 1, &tiles[0]);

        // Bottom metatile
        BuildDoorTiles(&tiles[0], DOOR_TILE_START_SIZE1 + 4, &paletteNums[4]);
        DrawDoorMetatileAt(x, y, &tiles[0]);
    }
}

static void DrawClosedDoorTiles(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    if (gfx->size != DOOR_SIZE_1x1) // PS-098: a 1x1 door has no top metatile
        CurrentMapDrawMetatileAt(x, y - 1);
    CurrentMapDrawMetatileAt(x, y);

    if (gfx->size == DOOR_SIZE_2x2)
    {
        CurrentMapDrawMetatileAt(x + 1, y - 1);
        CurrentMapDrawMetatileAt(x + 1, y);
    }
}

static void DrawDoor(const struct DoorGraphics *gfx, const struct DoorAnimFrame *frame, u32 x, u32 y)
{
    if (frame->offset == 0xFFFF)
    {
        DrawClosedDoorTiles(gfx, x, y);
        if (ShouldUseMultiCorridorDoor())
            DrawClosedDoorTiles(gfx, gSpecialVar_0x8004 + MAP_OFFSET, gSpecialVar_0x8005 + MAP_OFFSET);
    }
    else
    {
        CopyDoorTilesToVram(gfx, frame);
        DrawCurrentDoorAnimFrame(gfx, x, y, gfx->palettes);
        if (ShouldUseMultiCorridorDoor())
            DrawCurrentDoorAnimFrame(gfx, gSpecialVar_0x8004 + MAP_OFFSET, gSpecialVar_0x8005 + MAP_OFFSET, gfx->palettes);
    }
}

#define tFrameId data[4]
#define tCounter data[5]
#define tX data[6]
#define tY data[7]

// Draws a single frame of the door animation, or skips drawing to wait between frames.
// Returns FALSE when the final frame has been reached
static bool32 AnimateDoorFrame(struct DoorGraphics *gfx, struct DoorAnimFrame *frames, s16 *data)
{
    if (tCounter == 0)
        DrawDoor(gfx, &frames[tFrameId], tX, tY);

    if (tCounter == frames[tFrameId].time)
    {
        tCounter = 0;
        tFrameId++;
        if (frames[tFrameId].time == 0)
            return FALSE;
        else
            return TRUE;
    }
    tCounter++;
    return TRUE;
}

static void Task_AnimateDoor(u8 taskId)
{
    u16 *data = (u16*) gTasks[taskId].data;
    struct DoorAnimFrame *frames = gTasks[taskId].ptr.genericPtr[0];
    struct DoorGraphics *gfx = gTasks[taskId].ptr.genericPtr[1];

    if (AnimateDoorFrame(gfx, frames, data) == FALSE)
        DestroyTask(taskId);
}

static const struct DoorAnimFrame *GetLastDoorFrame(const struct DoorAnimFrame *frame, const void *unused)
{
    while (frame->time != 0)
        frame++;
    return frame - 1;
}

static const struct DoorGraphics *GetDoorGraphics(const struct DoorGraphics *gfx, u16 metatileNum)
{
    while (gfx->tiles != NULL)
    {
        if (gfx->metatileNum == metatileNum)
            return gfx;
        gfx++;
    }
    return NULL;
}

static s8 StartDoorAnimationTask(const struct DoorGraphics *gfx, const struct DoorAnimFrame *frames, u32 x, u32 y)
{
    if (FuncIsActiveTask(Task_AnimateDoor) == TRUE)
    {
        return -1;
    }
    else
    {
        u8 taskId = CreateTask(Task_AnimateDoor, 0x50);
        s16 *data = gTasks[taskId].data;

        tX = x;
        tY = y;

        gTasks[taskId].ptr.genericPtr[0] = (void*)frames;
        gTasks[taskId].ptr.genericPtr[1] = (void*)gfx;

        return taskId;
    }
}

static void DrawClosedDoor(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    DrawClosedDoorTiles(gfx, x, y);
}

static void DrawOpenedDoor(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    gfx = GetDoorGraphics(gfx, MapGridGetMetatileIdAt(x, y));
    if (gfx != NULL)
        DrawDoor(gfx, GetLastDoorFrame(gfx->size == DOOR_SIZE_1x1 ? sSmallDoorOpenAnimFrames : sDoorOpenAnimFrames, sDoorOpenAnimFrames), x, y); // PS-098: 1x1 frames
}

static s8 StartDoorOpenAnimation(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    gfx = GetDoorGraphics(gfx, MapGridGetMetatileIdAt(x, y));
    if (gfx == NULL)
    {
        return -1;
    }
    else
    {
        if (gfx->size == DOOR_SIZE_2x2)
            return StartDoorAnimationTask(gfx, sBigDoorOpenAnimFrames, x, y);
        else if (gfx->size == DOOR_SIZE_1x1) // PS-098
            return StartDoorAnimationTask(gfx, sSmallDoorOpenAnimFrames, x, y);
        else
            return StartDoorAnimationTask(gfx, sDoorOpenAnimFrames, x, y);
    }
}

static s8 StartDoorCloseAnimation(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    gfx = GetDoorGraphics(gfx, MapGridGetMetatileIdAt(x, y));
    if (gfx == NULL)
        return -1;
    else if (gfx->size == DOOR_SIZE_1x1) // PS-098
        return StartDoorAnimationTask(gfx, sSmallDoorCloseAnimFrames, x, y);
    else
        return StartDoorAnimationTask(gfx, sDoorCloseAnimFrames, x, y);
}

static s8 GetDoorSoundType(const struct DoorGraphics *gfx, u32 x, u32 y)
{
    gfx = GetDoorGraphics(gfx, MapGridGetMetatileIdAt(x, y));
    if (gfx == NULL)
        return -1;
    else
        return gfx->sound;
}

// Debug? Same as FieldAnimateDoorOpen but doesnt return or check if metatile is actually a door
static void UNUSED Debug_FieldAnimateDoorOpen(u32 x, u32 y)
{
    StartDoorOpenAnimation(GetDoorAnimGraphicsTable(), x, y);
}

void FieldSetDoorOpened(u32 x, u32 y)
{
    if (MetatileBehavior_IsDoor(MapGridGetMetatileBehaviorAt(x, y)))
        DrawOpenedDoor(GetDoorAnimGraphicsTable(), x, y);
}

void FieldSetDoorClosed(u32 x, u32 y)
{
    if (MetatileBehavior_IsDoor(MapGridGetMetatileBehaviorAt(x, y)))
        DrawClosedDoor(GetDoorAnimGraphicsTable(), x, y);
}

s8 FieldAnimateDoorClose(u32 x, u32 y)
{
    if (!MetatileBehavior_IsDoor(MapGridGetMetatileBehaviorAt(x, y)))
        return -1;
    else
        return StartDoorCloseAnimation(GetDoorAnimGraphicsTable(), x, y);
}

s8 FieldAnimateDoorOpen(u32 x, u32 y)
{
    if (!MetatileBehavior_IsDoor(MapGridGetMetatileBehaviorAt(x, y)))
        return -1;
    else
        return StartDoorOpenAnimation(GetDoorAnimGraphicsTable(), x, y);
}

bool8 FieldIsDoorAnimationRunning(void)
{
    return FuncIsActiveTask(Task_AnimateDoor);
}

u32 GetDoorSoundEffect(u32 x, u32 y)
{
    int sound = GetDoorSoundType(GetDoorAnimGraphicsTable(), x, y);

    if (sound == DOOR_SOUND_NORMAL)
        return SE_DOOR;
    else if (sound == DOOR_SOUND_SLIDING)
        return SE_SLIDING_DOOR;
    else if (sound == DOOR_SOUND_ARENA)
        return SE_REPEL;
    else
        return SE_DOOR;
}

// Opens the Battle Tower multi partner's door in sync with the player's door
static bool8 ShouldUseMultiCorridorDoor(void)
{
    if (FlagGet(FLAG_ENABLE_MULTI_CORRIDOR_DOOR))
    {
        if (gSaveBlock1Ptr->location.mapGroup == MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_CORRIDOR)
            && gSaveBlock1Ptr->location.mapNum == MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_CORRIDOR))
        {
            return TRUE;
        }
    }
    return FALSE;
}
