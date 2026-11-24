#include "global.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "fldeff.h"
#include "task.h"
#include "constants/metatile_labels.h"

static EWRAM_DATA u8 sEscalatorAnim_TaskId = 0;

static void SetEscalatorMetatile(u8 taskId, const s16 *metatileIds, u16 metatileMasks);
static void Task_DrawEscalator(u8 taskId);

#define ESCALATOR_STAGES     3
#define LAST_ESCALATOR_STAGE (ESCALATOR_STAGES - 1)
#define ESCALATOR_SECTIONS   7

// The seven escalator sections, in the order Task_DrawEscalator steps through them: the 1F sections run
// from their last frame back to the first, the 2F sections the other way round.
enum
{
    SECTION_1F_0, // 1F: the step next to the rail
    SECTION_1F_1, // 1F: the rail
    SECTION_1F_2, // 1F: the step next to the escalator (impassable)
    SECTION_1F_3, // 1F: the escalator itself
    SECTION_2F_0, // 2F: the step next to the escalator (impassable)
    SECTION_2F_1, // 2F: the escalator itself
    SECTION_2F_2, // 2F: the step next to the rail
};

static const s16 sEscalatorMetatiles_PokemonCenter[ESCALATOR_SECTIONS][ESCALATOR_STAGES] = {
    [SECTION_1F_0] = {
        METATILE_PokemonCenter_Escalator1F_Tile0_Frame2,
        METATILE_PokemonCenter_Escalator1F_Tile0_Frame1,
        METATILE_PokemonCenter_Escalator1F_Tile0_Frame0
    },
    [SECTION_1F_1] = {
        METATILE_PokemonCenter_Escalator1F_Tile1_Frame2,
        METATILE_PokemonCenter_Escalator1F_Tile1_Frame1,
        METATILE_PokemonCenter_Escalator1F_Tile1_Frame0
    },
    [SECTION_1F_2] = {
        METATILE_PokemonCenter_Escalator1F_Tile2_Frame2,
        METATILE_PokemonCenter_Escalator1F_Tile2_Frame1,
        METATILE_PokemonCenter_Escalator1F_Tile2_Frame0
    },
    [SECTION_1F_3] = {
        METATILE_PokemonCenter_Escalator1F_Tile3_Frame2,
        METATILE_PokemonCenter_Escalator1F_Tile3_Frame1,
        METATILE_PokemonCenter_Escalator1F_Tile3_Frame0
    },
    [SECTION_2F_0] = {
        METATILE_PokemonCenter_Escalator2F_Tile0_Frame0,
        METATILE_PokemonCenter_Escalator2F_Tile0_Frame1,
        METATILE_PokemonCenter_Escalator2F_Tile0_Frame2
    },
    [SECTION_2F_1] = {
        METATILE_PokemonCenter_Escalator2F_Tile1_Frame0,
        METATILE_PokemonCenter_Escalator2F_Tile1_Frame1,
        METATILE_PokemonCenter_Escalator2F_Tile1_Frame2
    },
    [SECTION_2F_2] = {
        METATILE_PokemonCenter_Escalator2F_Tile2_Frame0,
        METATILE_PokemonCenter_Escalator2F_Tile2_Frame1,
        METATILE_PokemonCenter_Escalator2F_Tile2_Frame2
    },
};

// PS-096: pokefirered's Pokémon Center escalator (its src/special_field_anim.c: BottomNextRail, BottomRail,
// BottomNext, Bottom, TopNext, Top, TopNextRail, in the same section order and stage direction as Emerald's)
static const s16 sEscalatorMetatiles_KantoPokemonCenter[ESCALATOR_SECTIONS][ESCALATOR_STAGES] = {
    [SECTION_1F_0] = {
        METATILE_KantoPokemonCenter_Escalator_BottomNextRail_Transition2,
        METATILE_KantoPokemonCenter_Escalator_BottomNextRail_Transition1,
        METATILE_KantoPokemonCenter_Escalator_BottomNextRail_Normal
    },
    [SECTION_1F_1] = {
        METATILE_KantoPokemonCenter_Escalator_BottomRail_Transition2,
        METATILE_KantoPokemonCenter_Escalator_BottomRail_Transition1,
        METATILE_KantoPokemonCenter_Escalator_BottomRail_Normal
    },
    [SECTION_1F_2] = {
        METATILE_KantoPokemonCenter_Escalator_BottomNext_Transition2,
        METATILE_KantoPokemonCenter_Escalator_BottomNext_Transition1,
        METATILE_KantoPokemonCenter_Escalator_BottomNext_Normal
    },
    [SECTION_1F_3] = {
        METATILE_KantoPokemonCenter_Escalator_Bottom_Transition2,
        METATILE_KantoPokemonCenter_Escalator_Bottom_Transition1,
        METATILE_KantoPokemonCenter_Escalator_Bottom_Normal
    },
    [SECTION_2F_0] = {
        METATILE_KantoPokemonCenter_Escalator_TopNext_Normal,
        METATILE_KantoPokemonCenter_Escalator_TopNext_Transition1,
        METATILE_KantoPokemonCenter_Escalator_TopNext_Transition2
    },
    [SECTION_2F_1] = {
        METATILE_KantoPokemonCenter_Escalator_Top_Normal,
        METATILE_KantoPokemonCenter_Escalator_Top_Transition1,
        METATILE_KantoPokemonCenter_Escalator_Top_Transition2
    },
    [SECTION_2F_2] = {
        METATILE_KantoPokemonCenter_Escalator_TopNextRail_Normal,
        METATILE_KantoPokemonCenter_Escalator_TopNextRail_Transition1,
        METATILE_KantoPokemonCenter_Escalator_TopNextRail_Transition2
    },
};

// The two sections the player cannot stand on keep their collision bit when redrawn
static const u16 sEscalatorSectionMasks[ESCALATOR_SECTIONS] = {
    [SECTION_1F_2] = MAPGRID_IMPASSABLE,
    [SECTION_2F_0] = MAPGRID_IMPASSABLE,
};

#define tState            data[0]
#define tTransitionStage  data[1]
#define tGoingUp          data[2]
#define tDrawingEscalator data[3]
#define tPlayerX          data[4]
#define tPlayerY          data[5]
#define tFrameTable       data[6] // PS-098 / PS-096: 1 + index into sEscalatorFrameTables, 0 = no frames for this tileset

// PS-098 / PS-096: the frames are metatile ids of one secondary tileset. They used to be Emerald's PokemonCenter
// ids written into whatever map the escalator is on; in pokefirered's Pokémon Centers the floor metatile 0x281
// is Emerald's Escalator1F_Tile1_Frame0, so the floor beside the escalator cycled through its frames. The table
// is chosen by the map's secondary tileset; a tileset not listed here keeps its metatiles and the escalator
// warp only moves the player.
extern const struct Tileset gTileset_PokemonCenter;
extern const struct Tileset gTileset_KantoPokemonCenter;

struct EscalatorFrameTable
{
    const struct Tileset *tileset;
    const s16 (*metatiles)[ESCALATOR_STAGES]; // ESCALATOR_SECTIONS rows
};

static const struct EscalatorFrameTable sEscalatorFrameTables[] =
{
    { &gTileset_PokemonCenter,      sEscalatorMetatiles_PokemonCenter },
    { &gTileset_KantoPokemonCenter, sEscalatorMetatiles_KantoPokemonCenter }, // PS-096
};

static u8 GetEscalatorFrameTable(void)
{
    const struct Tileset *secondaryTileset = gMapHeader.mapLayout->secondaryTileset;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sEscalatorFrameTables); i++)
    {
        if (sEscalatorFrameTables[i].tileset == secondaryTileset)
            return i + 1;
    }
    return 0;
}

static void SetEscalatorMetatile(u8 taskId, const s16 *metatileIds, u16 metatileMasks)
{
    s16 x = gTasks[taskId].tPlayerX - 1;
    s16 y = gTasks[taskId].tPlayerY - 1;
    s16 transitionStage = gTasks[taskId].tTransitionStage;
    s16 i;
    s16 j;

    // Check all the escalator sections and only progress the selected one to the next stage
    if (!gTasks[taskId].tGoingUp)
    {
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 3; j++)
            {
                s16 metatileId = MapGridGetMetatileIdAt(x + j, y + i);

                if (metatileIds[transitionStage] == metatileId)
                {
                    if (transitionStage != LAST_ESCALATOR_STAGE)
                        MapGridSetMetatileIdAt(x + j, y + i, metatileMasks | metatileIds[transitionStage + 1]);
                    else
                        MapGridSetMetatileIdAt(x + j, y + i, metatileMasks | metatileIds[0]);
                }
            }
        }
    }
    else
    {
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 3; j++)
            {
                s16 metatileId = MapGridGetMetatileIdAt(x + j, y + i);

                if (metatileIds[LAST_ESCALATOR_STAGE - transitionStage] == metatileId)
                {
                    if (transitionStage != LAST_ESCALATOR_STAGE)
                        MapGridSetMetatileIdAt(x + j, y + i, metatileMasks | metatileIds[1 - transitionStage]);
                    else
                        MapGridSetMetatileIdAt(x + j, y + i, metatileMasks | metatileIds[LAST_ESCALATOR_STAGE]);
                }
            }
        }
    }
}

static void Task_DrawEscalator(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    tDrawingEscalator = TRUE;

    // Set tile for each section of the escalator in sequence for current transition stage. PS-098 / PS-096: no
    // frames for this map's tileset: leave the metatiles alone (the stages still advance so IsEscalatorMoving
    // ends as before)
    if (tState < ESCALATOR_SECTIONS && tFrameTable != 0)
        SetEscalatorMetatile(taskId, sEscalatorFrameTables[tFrameTable - 1].metatiles[tState], sEscalatorSectionMasks[tState]);

    tState = (tState + 1) & 7;

    // If all metatiles of the escalator have been set, draw map and progress to next stage
    if (tState == 0)
    {
        DrawWholeMapView();
        tTransitionStage = (tTransitionStage + 1) % ESCALATOR_STAGES;
        tDrawingEscalator = FALSE;
    }
}

static u8 CreateEscalatorTask(bool16 goingUp)
{
    u8 taskId = CreateTask(Task_DrawEscalator, 0);
    s16 *data = gTasks[taskId].data;

    PlayerGetDestCoords(&tPlayerX, &tPlayerY);
    tState = 0;
    tTransitionStage = 0;
    tGoingUp = goingUp;
    tFrameTable = GetEscalatorFrameTable(); // PS-098 / PS-096
    Task_DrawEscalator(taskId);
    return taskId;
}

void StartEscalator(bool8 goingUp)
{
    sEscalatorAnim_TaskId = CreateEscalatorTask(goingUp);
}

void StopEscalator(void)
{
    DestroyTask(sEscalatorAnim_TaskId);
}

bool8 IsEscalatorMoving(void)
{
    if (gTasks[sEscalatorAnim_TaskId].tDrawingEscalator == FALSE
     && gTasks[sEscalatorAnim_TaskId].tTransitionStage == LAST_ESCALATOR_STAGE)
        return FALSE;
    else
        return TRUE;
}

#undef tState
#undef tTransitionStage
#undef tGoingUp
#undef tDrawingEscalator
#undef tPlayerX
#undef tPlayerY
#undef tFrameTable
