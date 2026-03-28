#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#ifdef __linux__
#include <sys/time.h>
#endif

#include "../third-party/dave-g-json/cJSON.h"
#include "../third-party/libopus-1.5.2/include/opus.h"
#include "../third-party/rxi-log/log.h"
#define DR_MP3_IMPLEMENTATION
#include "../third-party/dr_mp3/dr_mp3.h"

#include "base.h"

#include "audio_channel.h"
#include "musicbot.h"
#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "memory_manager.h"
#include "server_message.h"

//todo, add two stereo, mono sounds weird

//for testing on windows

//quiestionable thread safety
void musicbot__remove_song(client_t *music_bot, int song_id)
{
	if (music_bot->music_bot_client_extension.songs[song_id].is_existing == false)
	{
		return;
	}
	music_bot->music_bot_client_extension.songs[song_id].is_existing = FALSE;

	if (music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer != NULL_POINTER)
	{
		memorymanager__free((nuint)music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer);
		music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer = 0;
	}

	//null out single music_bot_single_song_data_t entry
	clib__null_memory((void *)&music_bot->music_bot_client_extension.songs[song_id], sizeof(music_bot_single_song_data_t));

	music_bot->music_bot_client_extension.music_bot_songs_count--;
}

/**
 * @brief adds song to music bot, it is used within write lock on clients array
 * @attention can cause future problems with tread safety
 * *
 * @return void
 * */

//add client check macro that checks if client is valid before every access to it

void musicbot__add_song(musicbot_add_song_arg_struct_t *arg)
{
	music_bot_single_song_data_t *song_in_loop;
	drmp3 mp3;

	//find free song index

	boole status = FALSE;

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	//need some type of macro to verify the client
	if (arg->music_bot->music_bot_client_extension.music_bot_songs_count == MUSIC_BOT_MAX_FILE_COUNT)
	{
		memorymanager__free((nuint)arg->mp3_data_buffer);
		status = TRUE;
	}
	pthread_rwlock_unlock(&clients_global_rwlock_guard);

	if (status == TRUE)
	{
		return;
	}

	for (int i = 0; i < MUSIC_BOT_MAX_FILE_COUNT; i++)
	{
		status = FALSE;
		pthread_rwlock_rdlock(&clients_global_rwlock_guard);

		song_in_loop = &arg->music_bot->music_bot_client_extension.songs[i];
		if (song_in_loop->is_existing == TRUE)
		{
			status = TRUE;
		}
		pthread_rwlock_unlock(&clients_global_rwlock_guard);

		if (status == TRUE)
		{
			continue;
		}

		pthread_rwlock_wrlock(&clients_global_rwlock_guard);

		song_in_loop->mp3_data_buffer = arg->mp3_data_buffer;
		song_in_loop->mp3_data_buffer_length = arg->mp3_data_buffer_length;

		DBG_MUSIC_BOT log_info("%s %s %s", "musicbot__add_song added song -> ", arg->song_name, "\n");

		clib__copy_memory((void *)arg->song_name, song_in_loop->song_name, clib__utf8_string_length(arg->song_name), SONG_NAME_MAX_LENGTH);
		clib__null_memory((void *)&mp3, sizeof(drmp3));

		pthread_rwlock_unlock(&clients_global_rwlock_guard);

		/* Decode MP3 */
		void *mp3_data_buffer1;
		int mp3_data_buffer_length1;

		pthread_rwlock_rdlock(&clients_global_rwlock_guard);
		mp3_data_buffer1 = song_in_loop->mp3_data_buffer;
		mp3_data_buffer_length1 = song_in_loop->mp3_data_buffer_length;
		pthread_rwlock_unlock(&clients_global_rwlock_guard);

		drmp3_init_memory(&mp3, mp3_data_buffer1, mp3_data_buffer_length1, NULL_POINTER);

		drmp3_uint64 frameCount = drmp3_get_pcm_frame_count(&mp3);
		uint64 seconds_length = frameCount / mp3.sampleRate;
		song_in_loop->length_seconds = seconds_length;
		DBG_MUSIC_BOT log_info("%s", "music bot add song success");
		drmp3_uninit(&mp3);

		pthread_rwlock_wrlock(&clients_global_rwlock_guard);
		arg->music_bot->music_bot_client_extension.music_bot_songs_count++;
		song_in_loop->is_existing = TRUE;
		pthread_rwlock_unlock(&clients_global_rwlock_guard);

		break;
	}

	pthread_rwlock_rdlock(&clients_global_rwlock_guard);
	server_msg__send_music_bot_song_list_to_single_client(arg->sender_client_index, arg->music_bot->client_id);
	pthread_rwlock_unlock(&clients_global_rwlock_guard);
	DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__send_music_bot_song_list_to_single_client");

	memorymanager__free((nuint)arg);
}

/**
 * @brief thread of music bot, each music bot runs in its own thread
 * @attention can cause future problems with tread safety
 * *
 * @return void
 * */
void musicbot__threadstart(client_t *music_bot_client)
{
	drmp3 mp3;
	int song_index = 0;

	music_bot_single_song_data_t *song_in_loop;

	music_bot_client->music_bot_client_extension.music_bot_songs_count = 0;

	while (music_bot_client->music_bot_client_extension.is_music_bot_running == TRUE)
	{
		base__sleep_for_milliseconds(1000);

		for (song_index = 0; song_index < music_bot_client->music_bot_client_extension.music_bot_songs_count; song_index++)
		{
			//
			//if music bot is stopped for some reason (deletion), break out of the loop,
			//while will run again, but because of condition in while it will also stop
			//

			if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE)
			{
				break;
			}

			song_in_loop = &music_bot_client->music_bot_client_extension.songs[song_index];

			if (song_in_loop->is_existing == FALSE)
			{
				music_bot_client->music_bot_client_extension.is_music_bot_running = FALSE;
				DBG_MUSIC_BOT log_info("%s", "BEEP, something is very wrong here, this should not happen \n");
				break;
			}

			clib__null_memory((void *)&mp3, sizeof(drmp3));

			drmp3_init_memory(&mp3, song_in_loop->mp3_data_buffer, song_in_loop->mp3_data_buffer_length, NULL_POINTER);

			const int opus_rate = 48000;
			const int frame_size = 960; // 20 ms @ 48 kHz

			/* Decode MP3 */
			drmp3_uint64 frameCount = drmp3_get_pcm_frame_count(&mp3);
			float *pcm = (float *)memorymanager__allocate(frameCount * mp3.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
			drmp3_read_pcm_frames_f32(&mp3, frameCount, pcm);

			uint64 seconds_length = frameCount / mp3.sampleRate;

			if (seconds_length == 0 || seconds_length > 1000)
			{
				goto label_single_music_bot_thread_end;
			}

			DBG_MUSIC_BOT log_info("%s %llu %s", "seconds length is ->", seconds_length, "\n");

			/* Resample to 48 kHz if necessary */
			float *resampled_pcm = pcm;
			drmp3_uint64 resampled_frames = frameCount;

			if (mp3.sampleRate != opus_rate)
			{
				resampled_frames = (drmp3_uint64)(frameCount * (double)opus_rate / mp3.sampleRate);
				resampled_pcm = (float *)memorymanager__allocate(resampled_frames * mp3.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
				for (drmp3_uint64 i = 0; i < resampled_frames; i++)
				{
					double src_idx = i * ((double)mp3.sampleRate / opus_rate);
					drmp3_uint64 idx = (drmp3_uint64)src_idx;
					double frac = src_idx - idx;
					for (int ch = 0; ch < mp3.channels; ch++)
					{
						if (idx + 1 < frameCount)
						{
							resampled_pcm[i * mp3.channels + ch] = pcm[idx * mp3.channels + ch] * (1.0 - frac) + pcm[(idx + 1) * mp3.channels + ch] * frac;
						}
						else
						{
							resampled_pcm[i * mp3.channels + ch] = pcm[idx * mp3.channels + ch];
						}
					}
				}
			}

			/* Create Opus encoder at 48 kHz */
			int err;
			OpusEncoder *enc = opus_encoder_create(opus_rate, mp3.channels, OPUS_APPLICATION_AUDIO, &err);
			if (err != OPUS_OK)
			{
				log_info("%s", "opus_encoder_create failed \n");
				return;
			}
			opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));

			/* Convert float → int16 with clipping */
			opus_int16 *pcm16 = (opus_int16 *)memorymanager__allocate(resampled_frames * mp3.channels * sizeof(opus_int16), MEMALLOC_MUSICBOT_SONG);
			for (drmp3_uint64 i = 0; i < resampled_frames * mp3.channels; i++)
			{
				float s = resampled_pcm[i];
				if (s > 1.0f)
				{
					s = 1.0f;
				}
				if (s < -1.0f)
				{
					s = -1.0f;
				}
				pcm16[i] = (opus_int16)(s * 32767.0f);
			}

			unsigned char opus_packet[2048];
			clib__null_memory(opus_packet, sizeof(opus_packet));

			uint64 timestamp_started_playing = base__get_timestamp_ms();

			clib__null_memory(music_bot_client->song_name, SONG_NAME_MAX_LENGTH);
			clib__copy_memory(song_in_loop->song_name, music_bot_client->song_name, clib__utf8_string_length(song_in_loop->song_name), SONG_NAME_MAX_LENGTH);
			server_msg__send_start_song_stream_message_to_clients_in_same_channel(music_bot_client);

			boole is_stop_reason_sudden_song_deletion = FALSE;
			for (drmp3_uint64 i = 0; i < resampled_frames; i += frame_size)
			{
				if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE || music_bot_client->music_bot_client_extension.songs[song_index].is_existing == FALSE)
				{
					is_stop_reason_sudden_song_deletion = TRUE;
					break;
				}

				/* Always encode EXACTLY frame_size samples, zero-padding if needed */
				opus_int16 temp[960 * 2] = { 0 };
				drmp3_uint64 available = resampled_frames - i;
				if (available > frame_size)
				{
					available = frame_size;
				}

				clib__copy_memory((void *)(pcm16 + i * mp3.channels), (void *)temp, available * mp3.channels * sizeof(opus_int16), available * mp3.channels * sizeof(opus_int16));

				int bytes = opus_encode(enc, temp, frame_size, opus_packet, sizeof(opus_packet));
				if (bytes < 0)
				{
					DBG_MUSIC_BOT log_info("%s", "opus_encode failed \n");
					break;
				}

				audio_channel__send_music_bot_data(music_bot_client->channel_id, opus_packet, bytes);

				base__sleep_for_milliseconds(15);
				//should be 20ms but that causes problems
			}

label_single_music_bot_thread_end:

			memorymanager__free((nuint)pcm16);
			if (resampled_pcm != pcm)
			{
				memorymanager__free((nuint)resampled_pcm);
			}
			memorymanager__free((nuint)pcm);
			opus_encoder_destroy(enc);
			drmp3_uninit(&mp3);

			uint64 timestamp_stopped_playing = base__get_timestamp_ms();

			if (seconds_length > 0 && is_stop_reason_sudden_song_deletion == FALSE)
			{
				if ((seconds_length * 1000) > (timestamp_stopped_playing - timestamp_started_playing))
				{
					//only start streaming next song after current song stream ended
					uint64 to_sleep = (seconds_length * 1000) - (timestamp_stopped_playing - timestamp_started_playing);
					DBG_MUSIC_BOT log_info("%s %llu %s", "sleeping for ->", to_sleep, "ms \n");

					base__sleep_for_milliseconds(to_sleep);
				}
			}
		}
	}
}