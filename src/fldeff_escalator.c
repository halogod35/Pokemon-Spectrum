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

static const s16 sEscalatorMetatiles_1F_0[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator1F_Tile0_Frame2,
    METATILE_PokemonCenter_Escalator1F_Tile0_Frame1,
    METATILE_PokemonCenter_Escalator1F_Tile0_Frame0
};

static const s16 sEscalatorMetatiles_1F_1[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator1F_Tile1_Frame2,
    METATILE_PokemonCenter_Escalator1F_Tile1_Frame1,
    METATILE_PokemonCenter_Escalator1F_Tile1_Frame0
};

static const s16 sEscalatorMetatiles_1F_2[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator1F_Tile2_Frame2,
    METATILE_PokemonCenter_Escalator1F_Tile2_Frame1,
    METATILE_PokemonCenter_Escalator1F_Tile2_Frame0
};

static const s16 sEscalatorMetatiles_1F_3[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator1F_Tile3_Frame2,
    METATILE_PokemonCenter_Escalator1F_Tile3_Frame1,
    METATILE_PokemonCenter_Escalator1F_Tile3_Frame0
};

static const s16 sEscalatorMetatiles_2F_0[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator2F_Tile0_Frame0,
    METATILE_PokemonCenter_Escalator2F_Tile0_Frame1,
    METATILE_PokemonCenter_Escalator2F_Tile0_Frame2
};

static const s16 sEscalatorMetatiles_2F_1[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator2F_Tile1_Frame0,
    METATILE_PokemonCenter_Escalator2F_Tile1_Frame1,
    METATILE_PokemonCenter_Escalator2F_Tile1_Frame2
};

static const s16 sEscalatorMetatiles_2F_2[ESCALATOR_STAGES] = {
    METATILE_PokemonCenter_Escalator2F_Tile2_Frame0,
    METATILE_PokemonCenter_Escalator2F_Tile2_Frame1,
    METATILE_PokemonCenter_Escalator2F_Tile2_Frame2
};

#define tState            data[0]
#define tTransitionStage  data[1]
#define tGoingUp          data[2]
#define tDrawingEscalator data[3]
#define tPlayerX          data[4]
#define tPlayerY          data[5]
#define tHasFrames        data[6] // PS-098

// PS-098: the frames above are metatile ids of Emerald's PokemonCenter tileset. They used to be written into
// whatever map the escalator is on; in pokefirered's Pokémon Centers (gTileset_KantoPokemonCenter) the floor
// metatile 0x281 is Emerald's Escalator1F_Tile1_Frame0, so the three floor cells beside the escalator
// cycled through its frames. A map whose secondary tileset is not listed here keeps its metatiles and the
// escalator warp only moves the player (pokefirered animates no metatiles either).
extern const struct Tileset gTileset_PokemonCenter;

static const struct Tileset *const sEscalatorFrameTilesets[] =
{
    &gTileset_PokemonCenter,
};

static bool8 TilesetHasEscalatorFrames(void)
{
    const struct Tileset *secondaryTileset = gMapHeader.mapLayout->secondaryTileset;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sEscalatorFrameTilesets); i++)
    {
        if (sEscalatorFrameTilesets[i] == secondaryTileset)
            return TRUE;
    }
    return FALSE;
}

static void SetEscalatorMetatile(u8 taskId, const s16 *metatileIds, u16 metatileMasks)
{
    s16 x = gTasks[taskId].tPlayerX - 1;
    s16 y = gTasks[taskId].tPlayerY - 1;
    s16 transitionStage = gTasks[taskId].tTransitionStage;
    s16 i;
    s16 j;

    // PS-098: no frames for this map's tileset: leave the metatiles alone (the stages still advance so
    // IsEscalatorMoving ends as before)
    if (!gTasks[taskId].tHasFrames)
        return;

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

    // Set tile for each section of the escalator in sequence for current transition stage
    switch (tState)
    {
        case 0:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_1F_0, 0);
            break;
        case 1:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_1F_1, 0);
            break;
        case 2:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_1F_2, MAPGRID_IMPASSABLE);
            break;
        case 3:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_1F_3, 0);
            break;
        case 4:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_2F_0, MAPGRID_IMPASSABLE);
            break;
        case 5:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_2F_1, 0);
            break;
        case 6:
            SetEscalatorMetatile(taskId, sEscalatorMetatiles_2F_2, 0);
            break;
    }

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
    tHasFrames = TilesetHasEscalatorFrames(); // PS-098
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
#undef tHasFrames
