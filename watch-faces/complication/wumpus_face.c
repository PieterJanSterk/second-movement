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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "wumpus_face.h"
#include "watch_slcd.h"   // For new display functions
#include "watch_buzzer.h" // For sound functions

#if __EMSCRIPTEN__
#include <time.h>
#endif

// Game constants
#define WUMPUS_FACE_ROWS 20 // 20 rooms
#define WUMPUS_FACE_COLS 3  // 3 connections per room
#define WUMPUS_CLEAR_ROOM 255 // A magic number to clear the room display
#define WUMPUS_NUM_PITS 2
#define WUMPUS_NUM_BATS 2
#define WUMPUS_NUM_ARROWS 5
#define WUMPUS_MOVE_PROB 75 // Wumpus moves if rand(100) > 75 (24% chance)

// This static variable holds the entire game state.
static wumpus_game_state_t _state;

// The fixed 20-room labyrinth layout (a Dodecahedron)
static const uint8_t cave_map[WUMPUS_FACE_ROWS][WUMPUS_FACE_COLS] = {
    {1, 4, 7}, {0, 2, 9}, {1, 3, 11}, {2, 4, 13}, {0, 3, 5},
    {4, 6, 14}, {5, 7, 16}, {0, 6, 8}, {7, 9, 17}, {1, 8, 10},
    {9, 11, 18}, {2, 10, 12}, {11, 13, 19}, {3, 12, 14}, {5, 13, 15},
    {14, 16, 19}, {6, 15, 17}, {8, 16, 18}, {10, 17, 19}, {12, 15, 18}
};

// --- Sound Melodies ---
// "Hall of the Mountain King" intro (longer)
static const BuzzerNote melody_startup[] = {
    BUZZER_NOTE_A3, BUZZER_NOTE_B3, BUZZER_NOTE_C4, BUZZER_NOTE_D4,
    BUZZER_NOTE_E4, BUZZER_NOTE_D4, BUZZER_NOTE_C4, BUZZER_NOTE_SILENT
};
// Winning Jingle (longer ascending arpeggio)
static const BuzzerNote melody_win[] = {
    BUZZER_NOTE_C4, BUZZER_NOTE_E4, BUZZER_NOTE_G4, BUZZER_NOTE_C5,
    BUZZER_NOTE_E5, BUZZER_NOTE_G5, BUZZER_NOTE_C6, BUZZER_NOTE_SILENT
};
// Losing Jingle (longer descending)
static const BuzzerNote melody_lose[] = {
    BUZZER_NOTE_B4, BUZZER_NOTE_A4SHARP_B4FLAT, BUZZER_NOTE_A4, BUZZER_NOTE_G4SHARP_A4FLAT,
    BUZZER_NOTE_G4, BUZZER_NOTE_F4SHARP_G4FLAT, BUZZER_NOTE_F4, BUZZER_NOTE_SILENT
};
// Bat "flutter" sound
static const BuzzerNote melody_bats[] = {
    BUZZER_NOTE_C7, BUZZER_NOTE_B6, BUZZER_NOTE_C7, BUZZER_NOTE_B6, BUZZER_NOTE_SILENT
};

// --- Helper Functions ---

/**
 * @brief Gets a random number up to num_values
 */
static inline uint8_t _get_rand_num(uint8_t num_values) {
#if __EMSCRIPTEN__
    return rand() % num_values; // Web emulator
#else
    return arc4random_uniform(num_values); // On-device hardware
#endif
}

/**
 * @brief Finds a new random room that does not contain a Wumpus or a Pit.
 * Used for bat transport to find a safe-ish landing spot.
 */
static uint8_t _find_safe_random_room() {
    uint8_t new_room;
    wumpus_hazard_type_t hazard;
    do {
        new_room = _get_rand_num(WUMPUS_FACE_ROWS);
        hazard = _state.hazards[new_room];
    } while (hazard == wumpus_hazard_wumpus || hazard == wumpus_hazard_pitfall);
    return new_room;
}

/**
 * @brief Sets or clears the LAP indicator based on Wumpus mode.
 */
static void _update_lap_indicator() {
    if (_state.active_wumpus_mode) watch_set_indicator(WATCH_INDICATOR_LAP);
    else watch_clear_indicator(WATCH_INDICATOR_LAP);
}

/**
 * @brief Sets or clears the BELL indicator based on Quiet Mode.
 */
static void _update_sound_indicator() {
    if (_state.sound_mode_on) watch_set_indicator(WATCH_INDICATOR_BELL);
    else watch_clear_indicator(WATCH_INDICATOR_BELL);
}

/**
 * @brief Wumpus movement logic for stationary mode: 100% chance to move to
 * an adjacent room. Called on a missed shot.
 */
static bool _wumpus_flee(void) {
    for (size_t i = 0; i < WUMPUS_FACE_ROWS; i++) {
        if (_state.hazards[i] == wumpus_hazard_wumpus) {
            _state.hazards[i] = wumpus_hazard_none;
            uint8_t wumpus_room = cave_map[i][_get_rand_num(3)];
            _state.hazards[wumpus_room] = wumpus_hazard_wumpus;
            return true;
        }
    }
    return false;
}

/**
 * @brief Wumpus movement logic for active mode: 24% chance to move to
 * an adjacent room. Called after every player action.
 */
static bool _wumpus_move() {
    if (_get_rand_num(100) > WUMPUS_MOVE_PROB) {
        return _wumpus_flee(); // Re-use the flee logic
    }
    return false;
}

/**
 * @brief Plays a melody if sound is on and no other melody is playing.
 * @param melody The melody to play.
 */
static void _play_melody(wumpus_melody_t melody) {
    if (!_state.sound_mode_on) return; // Quiet mode is on
    if (_state.current_melody != MELODY_NONE) return; // Don't interrupt

    _state.current_melody = melody;
    _state.melody_step = 0;
    movement_request_tick_frequency(8); // Request 8Hz for smooth playback
}

/**
 * @brief Moves player to the selected room and returns the hazard there.
 */
static wumpus_hazard_type_t _go_to_selected_room() {
    if (_state.selected_room_n >= 0) {
        _state.player_room = cave_map[_state.player_room][_state.selected_room_n];
        return _state.hazards[_state.player_room];
    }
    return wumpus_hazard_none;
}

/**
 * @brief Displays a room number (1-20) in the top-right (WATCH_POSITION_TOP_RIGHT).
 * @param room The room number (0-19).
 */
static void _display_room(uint8_t room) {
    if (room < 100) {
        char room_str[4];
        snprintf(room_str, sizeof(room_str), "%2d", room + 1); // +1 for 1-based index
        watch_display_text(WATCH_POSITION_TOP_RIGHT, room_str);
    } else {
        watch_display_text(WATCH_POSITION_TOP_RIGHT, "  "); // Clear room display
    }
}

/**
 * @brief Displays the player's current room.
 */
static void _display_current_room() {
    _display_room(_state.player_room);
}

/**
 * @brief Displays the room the player is considering moving to (blinks).
 */
static void _display_selected_room() {
    if (_state.digits_tick_show) {
        uint8_t room = _state.selected_room_n >= 0 ?
            cave_map[_state.player_room][_state.selected_room_n] :
            _state.player_room;
        _display_room(room);
    } else {
        _display_room(WUMPUS_CLEAR_ROOM);
    }
}

/**
 * @brief Main UI display function for the middle of the screen (hours/minutes).
 * Shows "GO", "SHOT", "BAT", or shot selection UI.
 */
static void _display_current_action() {
    char buf[8];
    char part1[3] = "  "; // Hours (pos 4-5)
    char part2[3] = "  "; // Minutes (pos 6-7)
    watch_clear_colon();

    if (_state.action_tick_show) {
        switch (_state.current_action) {
            case wumpus_face_shoot:
                _display_current_room();
                strcpy(part1, "SH"); strcpy(part2, "OT");
                break;
            case wumpus_face_shoot_n:
                _display_current_room();
                snprintf(buf, sizeof(buf), "rn%-2d", _state.shots_path_len); // "rn1 "
                watch_set_colon();
                strncpy(part1, buf, 2); strncpy(part2, buf + 2, 2);
                part1[2] = '\0'; part2[2] = '\0';
                break;
            case wumpus_face_shoot_rooms:
                _display_current_room();
                snprintf(buf, sizeof(buf), "r%d%-2d", _state.shots_picked + 1, _state.shots_room + 1); // "r1 1"
                watch_set_colon();
                strncpy(part1, buf, 2); strncpy(part2, buf + 2, 2);
                part1[2] = '\0'; part2[2] = '\0';
                break;
            case wumpus_face_go:
                _display_current_room();
                strcpy(part1, "GO"); strcpy(part2, "  ");
                break;
            case wumpus_face_choosing_room:
                _display_selected_room(); // Handles its own display
                break;
            case wumpus_face_bat_transport:
                strcpy(part1, "BA"); strcpy(part2, "T "); // Display "BAT"
                break;
            case wumpus_face_died:
            case wumpus_face_won:
                break; // Handled by _display_death/won
        }

        if (_state.current_action != wumpus_face_choosing_room &&
            _state.current_action != wumpus_face_died &&
            _state.current_action != wumpus_face_won) {
            watch_display_text(WATCH_POSITION_HOURS, part1);
            watch_display_text(WATCH_POSITION_MINUTES, part2);
        }
    } else {
        // Blink logic (clear text)
        if (_state.current_action == wumpus_face_shoot_n || _state.current_action == wumpus_face_shoot_rooms) {
            watch_set_colon();
            watch_display_text(WATCH_POSITION_MINUTES, "  ");
        } else if (_state.current_action == wumpus_face_shoot || _state.current_action == wumpus_face_go) {
            watch_display_text(WATCH_POSITION_HOURS, "  ");
            watch_display_text(WATCH_POSITION_MINUTES, "  ");
        }
    }
}

/**
 * @brief Displays a hazard code ("UU", "Bt", "Pt") in the seconds position.
 */
static void _display_hazard(wumpus_hazard_type_t h) {
    switch (h) {
        case wumpus_hazard_wumpus: watch_display_text(WATCH_POSITION_SECONDS, "UU"); break;
        case wumpus_hazard_bat: watch_display_text(WATCH_POSITION_SECONDS, "Bt"); break;
        case wumpus_hazard_pitfall: watch_display_text(WATCH_POSITION_SECONDS, "Pt"); break;
        case wumpus_hazard_arrow: watch_display_text(WATCH_POSITION_SECONDS, "Ar"); break;
        case wumpus_hazard_none: watch_display_text(WATCH_POSITION_SECONDS, "  "); break;
    }
}

/**
 * @brief Checks adjacent rooms for hazards and displays them, cycling if
 * there are multiple. Plays bat sound if bats are near.
 */
static void _display_hazards() {
    uint8_t hazards_cnt = 0;
    uint8_t hazards[WUMPUS_FACE_COLS];

    for (size_t i = 0; i < WUMPUS_FACE_COLS; i++) {
        wumpus_hazard_type_t hazard = _state.hazards[cave_map[_state.player_room][i]];
        if (hazard != wumpus_hazard_none) {
            hazards[hazards_cnt++] = hazard;
        }
    }

    if (hazards_cnt) {
        wumpus_hazard_type_t current_hazard = hazards[_state.hazard_point];
        _display_hazard(current_hazard);

        // If a bat is nearby, try to play the bat sound
        if (current_hazard == wumpus_hazard_bat) {
            _play_melody(MELODY_BATS);
        }

        if (++_state.hazard_point >= hazards_cnt) {
            _state.hazard_point = 0;
        }
    } else {
        _display_hazard(wumpus_hazard_none);
    }
}

/**
 * @brief Gets a random room index that is not the player's room and
 * does not already have a hazard.
 */
static uint8_t _generate_unique(uint8_t player_room) {
    uint8_t value;
    bool unique;
    do {
        value =_get_rand_num(WUMPUS_FACE_ROWS);
        unique = value != player_room;
        if (unique) {
            if (_state.hazards[value] != wumpus_hazard_none) {
                unique = false;
            }
        }
    } while (!unique);
    return value;
}

/**
 * @brief Populates the cave with hazards (Pits, Bats, Wumpus).
 */
static void _generate_hazards(uint8_t player_room) {
    for (size_t i = 0; i < WUMPUS_FACE_ROWS; i++) _state.hazards[i] = wumpus_hazard_none;
    for (size_t i = 0; i < WUMPUS_NUM_PITS; i++) _state.hazards[_generate_unique(player_room)] = wumpus_hazard_pitfall;
    for (size_t i = 0; i < WUMPUS_NUM_BATS; i++) _state.hazards[_generate_unique(player_room)] = wumpus_hazard_bat;
    _state.hazards[_generate_unique(player_room)] = wumpus_hazard_wumpus;
}

/**
 * @brief Displays "DIED" and starts the lose melody/LED sequence.
 */
static void _display_death(wumpus_hazard_type_t hazard) {
    _display_hazard(hazard);
    watch_display_text(WATCH_POSITION_HOURS, "DI");
    watch_display_text(WATCH_POSITION_MINUTES, "ED");
    _play_melody(MELODY_LOSE); // LED will be set after melody
}

/**
 * @brief Displays "Great" and starts the win melody/LED sequence.
 */
static void _display_won() {
    _display_hazard(wumpus_hazard_none);
    watch_display_text(WATCH_POSITION_HOURS, "Gr");
    watch_display_text(WATCH_POSITION_MINUTES, "ea");
    watch_display_text(WATCH_POSITION_SECONDS, "t ");
    _play_melody(MELODY_WIN); // LED will be set after melody
}

/**
 * @brief Resets the game state to start a new game.
 */
static void _init_game() {
    _state.current_action = wumpus_face_shoot;
    _state.player_room = _get_rand_num(WUMPUS_FACE_ROWS);
    _state.selected_room_n = -1;
    _state.digits_tick_show = true;
    _state.action_tick_show = true;
    _state.hazard_point = 0;
    _state.shots_path_len = 0;
    _state.shots_picked = 0;
    _state.shots_room = 0;
    _state.arrows = WUMPUS_NUM_ARROWS;
    _state.led_cnt = 0;
    _state.transport_timer = 0;
    _state.transport_dest_room = 0;
    _state.current_melody = MELODY_NONE;
    _state.melody_step = 0;
    _state.active_wumpus_mode = false; // Default to stationary Wumpus
    _state.sound_mode_on = true;      // Default to sound ON
    _update_lap_indicator();
    _update_sound_indicator();
    _generate_hazards(_state.player_room);
}

/**
 * @brief Resolves the arrow shot, checks for "crooked arrow",
 * and returns the game state (won, died, or still playing).
 */
static wumpus_current_action_t _shot() {
    if (--_state.arrows < 0) {
        _display_death(wumpus_hazard_arrow); // Out of arrows
        return wumpus_face_died;
    }

    for (size_t i = 0; i < _state.shots_path_len; i++) {
        if (i > 0) {
            // Check if path is valid (room is connected)
            for (size_t j = 0; j < WUMPUS_FACE_COLS; j++) {
                if (cave_map[_state.shots_path[i - 1]][j] == _state.shots_path[i]) {
                    goto found; // Path is valid, continue
                }
            }
            // Path is invalid: "Crooked Arrow"
            uint8_t rnd = _get_rand_num(WUMPUS_FACE_COLS);
            _state.shots_path[i] = cave_map[_state.shots_path[i - 1]][rnd];
        }
        found:

        // Check for shooting self
        if (_state.shots_path[i] == _state.player_room) {
            _display_death(wumpus_hazard_arrow);
            return wumpus_face_died;
        }

        wumpus_hazard_type_t *hazard = & _state.hazards[_state.shots_path[i]];

        if (*hazard == wumpus_hazard_bat) *hazard = wumpus_hazard_none; // Bat killed
        else if (*hazard == wumpus_hazard_wumpus) return wumpus_face_won; // Wumpus killed!
    }

    return wumpus_face_shoot; // Missed
}

// --- Watch Face Functions ---

/**
 * @brief Called once at boot.
 */
void wumpus_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void ** context_ptr) {
    (void) settings; (void) watch_face_index; (void) context_ptr;
    // No setup needed, state is handled in _init_game
}

/**
 * @brief Called when the watch face is activated.
 */
void wumpus_face_activate(movement_settings_t *settings, void *context) {
    (void) settings; (void) context;

#if __EMSCRIPTEN__
    time_t t; srand((unsigned) time(&t)); // Seed for web emulator
#endif    

    movement_request_tick_frequency(4); // Start with 4Hz tick
    _init_game(); // Set up a new game
    
    _play_melody(MELODY_STARTUP); // Play starting tune
    _update_lap_indicator();      // Set LAP indicator
    _update_sound_indicator();    // Set BELL indicator
}

/**
 * @brief Called every tick. This is the main game loop.
 */
bool wumpus_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    (void) context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            // Display "WMPUS" on custom LCD, "WH" on classic
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "WMPUS", "WH");
            _display_current_action();
            _display_hazards();
            break;
        case EVENT_TICK:
            // State 1: A melody is playing
            if (_state.current_melody != MELODY_NONE) {
                const BuzzerNote* melody;
                if (_state.current_melody == MELODY_STARTUP) melody = melody_startup;
                else if (_state.current_melody == MELODY_WIN) melody = melody_win;
                else if (_state.current_melody == MELODY_BATS) melody = melody_bats;
                else melody = melody_lose;

                BuzzerNote note = melody[_state.melody_step];

                if (note == BUZZER_NOTE_SILENT) {
                    // Melody finished
                    if (_state.current_melody == MELODY_WIN) {
                        _state.led_cnt = 3; watch_set_led_green(); // Start win flash
                    } else if (_state.current_melody == MELODY_LOSE) {
                        _state.led_cnt = 3; watch_set_led_red(); // Start lose flash
                    }
                    _state.current_melody = MELODY_NONE;
                    _state.melody_step = 0;
                    movement_request_tick_frequency(4); // Restore 4Hz tick
                } else {
                    watch_buzzer_play_note(note, 120); // Play note
                    _state.melody_step++;
                }
            }
            // State 2: Win/Lose LED is flashing (game is paused)
            else if (_state.led_cnt > 0) {
                if (_state.led_cnt == 1) {
                    watch_set_led_off();
                    _init_game(); // Reset the game
                    _display_current_action();
                    _display_hazards();
                }
                _state.led_cnt--;
            }
            // State 3: Bat transport is active (game is paused)
            else if (_state.current_action == wumpus_face_bat_transport) {
                if (_state.transport_timer > 0) {
                    _state.transport_timer--;
                } else {
                    _state.player_room = _state.transport_dest_room; // Move player
                    wumpus_hazard_type_t new_hazard = _state.hazards[_state.player_room];

                    if (new_hazard == wumpus_hazard_bat) {
                        // Landed on another bat!
                        _state.transport_dest_room = _find_safe_random_room();
                        _state.transport_timer = 4; // 1 second
                        _play_melody(MELODY_BATS);
                    } else {
                        // Landed safe (or on Wumpus/Pit, checked on next move)
                        _state.current_action = wumpus_face_go;
                        _display_current_action();
                        _display_hazards();
                    }
                }
            }
            // State 4: Normal game tick (blinking UI)
            else {
                if (_state.current_action == wumpus_face_choosing_room) {
                    _display_selected_room();
                    _state.digits_tick_show = !_state.digits_tick_show;
                } else if (_state.current_action != wumpus_face_died) {
                    _display_current_action();
                    _state.action_tick_show = !_state.action_tick_show;
                }
                if (_state.current_action != wumpus_face_died) {
                    _display_hazards();
                }
            }
            break;

        // "Cycle" (short press top-left)
        case EVENT_LIGHT_BUTTON_UP:
            switch (_state.current_action) {
                case wumpus_face_shoot:
                case wumpus_face_go: // Toggle GO/SHOT
                    _state.current_action = _state.current_action == wumpus_face_go
                        ? wumpus_face_shoot : wumpus_face_go;
                    _state.action_tick_show = true;
                    break;
                case wumpus_face_shoot_n: // Change shot distance
                    if (++_state.shots_path_len > 5) _state.shots_path_len = 1;
                    _state.action_tick_show = true;
                    break;
                case wumpus_face_shoot_rooms: // Change room in path
                    if (++_state.shots_room >= WUMPUS_FACE_ROWS) _state.shots_room = 0;
                    _state.action_tick_show = true;
                    break;
                case wumpus_face_choosing_room: // Change room to move to
                    if (++_state.selected_room_n >= WUMPUS_FACE_COLS) _state.selected_room_n = -1;
                    break;
                case wumpus_face_won: case wumpus_face_died: break;
            }
            _display_current_action();
            break;
        
        // TOGGLE WUMPUS MODE (long press top-left)
        case EVENT_LIGHT_LONG_PRESS:
            _state.active_wumpus_mode = !_state.active_wumpus_mode;
            _update_lap_indicator();
            if (_state.sound_mode_on) watch_buzzer_play_note(BUZZER_NOTE_C6, 50);
            break;

        // "Confirm" (short press top-right)
        case EVENT_ALARM_BUTTON_UP:
            switch (_state.current_action) {
                case wumpus_face_go: // Confirm GO
                    _state.selected_room_n = -1;
                    _state.digits_tick_show = false;
                    _state.action_tick_show = true;
                    _display_current_action();
                    _state.current_action = wumpus_face_choosing_room;
                    break;
                case wumpus_face_shoot: // Confirm SHOT
                    _state.shots_path_len = 1;
                    _state.action_tick_show = true;
                    _state.current_action = wumpus_face_shoot_n;
                    break;
                case wumpus_face_shoot_n: // Confirm distance
                    _state.shots_room = cave_map[_state.player_room][0];
                    _state.shots_picked = 0;
                    _state.action_tick_show = true;
                    _state.current_action = wumpus_face_shoot_rooms;
                    break;
                case wumpus_face_choosing_room: // Confirm move
                    {
                        wumpus_hazard_type_t result = _go_to_selected_room();
                        if (result == wumpus_hazard_bat) {
                            // Bat transport!
                            _state.current_action = wumpus_face_bat_transport;
                            _state.transport_timer = 4; // 1 second @ 4Hz
                            _state.transport_dest_room = _find_safe_random_room();
                            _display_current_action(); // Shows "BAT"
                            _play_melody(MELODY_BATS);
                        } else if (result == wumpus_hazard_none) {
                            // Safe move
                            _state.current_action = wumpus_face_go;
                            _state.digits_tick_show = true;
                            _display_hazards();
                        } else {
                            // Death (Wumpus or Pit)
                            _display_death(result);
                            _state.current_action = wumpus_face_died;
                        }
                    }
                    break;
                case wumpus_face_shoot_rooms: // Confirm room in path
                    _state.shots_path[_state.shots_picked] = _state.shots_room;
                    _state.shots_room = 0;
                    _state.action_tick_show = true;
                    if (++_state.shots_picked >= _state.shots_path_len) {
                        // Path complete, fire arrow
                        _state.current_action = _shot();
                        if (_state.current_action == wumpus_face_won) {
                            _display_won();
                        } else {
                            // Missed shot
                            if (!_state.active_wumpus_mode) {
                                // Stationary mode: Wumpus flees now
                                if (_wumpus_flee() && _state.hazards[_state.player_room] == wumpus_hazard_wumpus) {
                                    _display_death(wumpus_hazard_wumpus);
                                    _state.current_action = wumpus_face_died;
                                }
                            }
                        }
                    }
                    break;
                case wumpus_face_won: case wumpus_face_died: break;
            }

            _display_current_action();

            // Check for Wumpus movement (only in active mode)
            if (_state.active_wVumpus_mode) {
                if (_wumpus_move() && _state.hazards[_state.player_room] == wumpus_hazard_wumpus) {
                    _display_death(wumpus_hazard_wumpus);
                    _state.current_action = wumpus_face_died;
                }
            }
            break;

        // TOGGLE QUIET MODE (long press top-right)
        case EVENT_ALARM_LONG_PRESS:
            _state.sound_mode_on = !_state.sound_mode_on;
            _update_sound_indicator();
            if (_state.sound_mode_on) watch_buzzer_play_note(BUZZER_NOTE_C5, 50);
            break;

        default:
            return movement_default_loop_handler(event, settings);
    }

    // Keep the watch face active unless the LED is flashing (win/lose)
    // or a melody is playing.
    return _state.led_cnt == 0 && _state.current_melody == MELODY_NONE;
}

/**
 * @brief Called when the watch face is resigned.
 */
void wumpus_face_resign(movement_settings_t *settings, void *context) {
    (void) settings; (void) context;
    watch_set_led_off();
    watch_buzzer_stop(); // Stop any sounds
}