#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PLAYER_STATE_STOPPED = 0,
    PLAYER_STATE_PLAYING = 1,
    PLAYER_STATE_PAUSED  = 2
} player_state_t;

void audio_player_init(void);
bool audio_player_play_track(uint8_t track_num);
void audio_player_stop(void);
void audio_player_pause(void);
void audio_player_resume(void);
player_state_t audio_player_get_state(void);
uint8_t audio_player_get_current_track(void);
void audio_player_play_test_tone(void);
void audio_player_set_volume(uint8_t volume); // 0-100

#endif // AUDIO_PLAYER_H
