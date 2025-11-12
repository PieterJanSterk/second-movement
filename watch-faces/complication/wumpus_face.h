/*
 * MIT License
 *
 * Copyright (c) 2024 Evgeny Stepanischev
 * (and contributions from the Sensor Watch community)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KINDD, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WUMPUS_FACE_H_
#define WUMPUS_FACE_H_

#include "movement.h"
#include "watch_buzzer.h" // Include for playing sounds

/*
 * Hunt the Wumpus
 * =====================
 *
 * This is a port of the classic 1973 text-based game "Hunt the Wumpus".
 *
 * Objective:
 * Find and shoot the Wumpus, a creature living in a dark cave of 20 rooms.
 *
 * The Labyrinth:
 * The cave is a dodecahedron: 20 rooms, each connected to 3 other rooms.
 * Your current room number (1-20) is shown in the top-right.
 *
 * Hazards:
 * - 1 Wumpus: Moving into its room is fatal.
 * - 2 Bottomless Pits: Moving into a pit is fatal.
 * - 2 Super Bats: Moving into a bat's room will cause it to grab you
 * and transport you to a random, safe (non-Wumpus, non-Pit) room.
 *
 * Warnings (bottom-right display):
 * You will get warnings for hazards in *adjacent* rooms.
 * - "UU": A Wumpus is one room away.
 * - "Pt": A Pit is one room away.
 * - "Bt": Bats are one room away.
 *
 * Controls (Short Press):
 * --------------------------
 * Light (Top-Left): "Cycle"
 * - Toggles between "GO" and "SHOT" on the main screen.
 * - Cycles through connected rooms when in "GO" mode.
 * - Cycles through distance (1-5) when in "SHOT" mode.
 * - Cycles through rooms (1-20) when choosing a shot path.
 *
 * Alarm (Top-Right): "Confirm"
 * - Confirms your choice ("GO", "SHOT", move to room, shot distance, etc).
 *
 * Controls (Long Press):
 * --------------------------
 * Long-Press Light (Top-Left): Toggle Wumpus Mode
 * - Toggles between "Stationary" and "Active" Wumpus.
 * - LAP indicator ON: Active Mode. The Wumpus has a 24% chance
 * to move to an adjacent room after *every* action you take.
 * - LAP indicator OFF: Stationary Mode. The Wumpus only moves to an
 * adjacent room after you fire an arrow and *miss*.
 *
 * Long-Press Alarm (Top-Right): Toggle Quiet Mode
 * - Toggles all game sounds on or off.
 * - BELL indicator ON: Sound is ON.
 * - BELL indicator OFF: Quiet Mode.
 *
 * How to Shoot:
 * You have 5 arrows. If you run out, you die.
 * 1. Select "SHOT" and press Confirm.
 * 2. Select distance (1-5 rooms) and press Confirm.
 * 3. Select each room in the path and press Confirm.
 * WARNING: The "Crooked Arrow"
 * If you choose a room in your path that is *not* connected to the
 * arrow's previous room, it will fly to a *random* connected room instead.
 * You can shoot yourself!
 */


// An enum to define the different hazards in the cave
typedef enum {
    wumpus_hazard_none = 0,
    wumpus_hazard_wumpus,
    wumpus_hazard_bat,
    wumpus_hazard_pitfall,
    wumpus_hazard_arrow,
} wumpus_hazard_type_t;

// An enum to define the different melodies the game can play
typedef enum {
    MELODY_NONE,
    MELODY_STARTUP,
    MELODY_WIN,
    MELODY_LOSE,
    MELODY_BATS
} wumpus_melody_t;

// An enum to track the player's current action or game state
typedef enum {
    wumpus_face_shoot,        // Player is choosing "SHOT"
    wumpus_face_shoot_n,      // Player is choosing shot distance
    wumpus_face_shoot_rooms,  // Player is choosing shot path
    wumpus_face_go,           // Player is choosing "GO"
    wumpus_face_choosing_room,// Player is choosing which room to enter
    wumpus_face_bat_transport,// Player is being transported by a bat
    wumpus_face_died,         // Game over, player lost
    wumpus_face_won,          // Game over, player won
} wumpus_current_action_t;

// This structure holds the entire state of the game
typedef struct {
    // Game World State
    uint8_t player_room;    // Current room number of the player (0-19)
    wumpus_hazard_type_t hazards[WUMPUS_FACE_ROWS]; // Array tracking hazard in each room
    int8_t arrows;           // Number of arrows remaining

    // UI/Input State
    wumpus_current_action_t current_action; // The player's current state (see enum)
    int8_t selected_room_n;  // Index (0-2) of the room the player is about to move to
    bool digits_tick_show; // Used for blinking text
    bool action_tick_show; // Used for blinking text
    uint8_t hazard_point;   // Index for cycling through hazard warnings

    // Shot Path State
    uint8_t shots_path_len;   // Distance (1-5) of the arrow
    uint8_t shots_picked;     // How many rooms in the path have been chosen
    uint8_t shots_room;       // The room currently being selected for the path (0-19)
    uint8_t shots_path[WUMPUS_NUM_ARROWS]; // Array storing the chosen path rooms

    // Bat Transport State
    uint8_t transport_timer;      // Timer for how long "BAT" is displayed
    uint8_t transport_dest_room;  // Room the bat will drop the player in

    // Sound/Melody State
    wumpus_melody_t current_melody; // Which melody is currently playing (if any)
    uint8_t melody_step;      // The current note index in the melody

    // LED State
    uint8_t led_cnt;          // Timer for the win/lose LED flash

    // Game Settings
    bool active_wumpus_mode; // false = stationary, true = active (24% move)
    bool sound_mode_on;      // true = sounds on, false = quiet mode

} wumpus_game_state_t;

// Standard watch face function definitions
void wumpus_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void ** context_ptr);
void wumpus_face_activate(movement_settings_t *settings, void *context);
bool wumpus_face_loop(movement_event_t event, movement_settings_t *settings, void *context);
void wumpus_face_resign(movement_settings_t *settings, void *context);

// Watch face definition macro
#define wumpus_face ((const watch_face_t){ \
    wumpus_face_setup, \
    wumpus_face_activate, \
    wumpus_face_loop, \
    wumpus_face_resign \
})

#endif // WUMPUS_FACE_H_