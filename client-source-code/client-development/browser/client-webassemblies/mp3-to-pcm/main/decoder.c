#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NONSTANDARD_BUT_LOGICAL
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"
#include "../../common/clib.h"

#define WASM_EXPORT __attribute__((visibility("default")))

// memcpy/memset/memmove implementations

/**
 * @brief provides the memory copy ABI through the overlap-safe move routine
 *
 * @param void* dest -> destination buffer
 * @param const void* src -> source buffer
 * @param size_t n -> number of bytes
 *
 * @return void* -> destination pointer
 */
void *memcpy(void *dest, const void *src, size_t n)
{
    return memmove(dest, src, n);
}

/**
 * @brief fills a byte range for the decoder runtime ABI
 *
 * @param void* s -> destination buffer
 * @param int c -> value converted to a byte
 * @param size_t n -> number of bytes
 *
 * @return void* -> destination pointer
 */
void *memset(void *s, int c, size_t n)
{
    char *dst = s;
    while (n > 0)
    {
        *dst = (char)(c);
        dst++;
        n--;
    }
    return s;
}

// https://codereview.stackexchange.com/questions/174935/implementation-of-memmove
/**
 * @brief copies bytes in the direction needed to preserve overlapping input
 *
 * @param void* dest -> destination buffer
 * @param const void* src -> source buffer
 * @param size_t n -> number of bytes
 *
 * @return void* -> destination pointer
 */
void *memmove(void *dest, const void *src, size_t n)
{
    char *d = (char *)(dest);
    const char *s = (char const *)(src);

    if (s < d)
    {
        s += n;
        d += n;
        while (n--)
        {
            *--d = *--s;
        }
    }
    else
    {
        while (n--)
        {
            *d++ = *s++;
        }
    }

    return dest;
}

// Memory management

extern unsigned char __heap_base;

const size_t WASM_PAGE_SIZE = 1024 * 64;

/**
 * @brief reads available linear memory after the decoder heap base
 *
 * @return size_t -> heap capacity in bytes
 */
static size_t heap_size()
{
    uintptr_t heap_base = (uintptr_t)&__heap_base;
    return __builtin_wasm_memory_size(0) * WASM_PAGE_SIZE - heap_base;
}

#define DECODER_MAX_INPUT_BYTES (64U * 1024U * 1024U)
#define DECODER_MAX_WORKING_BYTES (256U * 1024U * 1024U)

/**
 * @brief grows linear memory within the decoder limit or traps on failure
 *
 * @param size_t required_bytes -> required byte capacity after the heap base
 *
 * @return void
 */
static void grow_memory_if_needed(size_t required_bytes)
{
    if (required_bytes > DECODER_MAX_WORKING_BYTES)
    {
        __builtin_trap();
    }
    size_t current_byte_size = heap_size();
    if (current_byte_size < required_bytes)
    {
        int num_pages = (required_bytes - current_byte_size) / WASM_PAGE_SIZE + 1;
        if (__builtin_wasm_memory_grow(0, num_pages) == (size_t)-1)
        {
            __builtin_trap();
        }
    }
}

/**
 * @brief aligns a size to four bytes within the decoder memory limit
 *
 * @param size_t sz -> unaligned byte count
 *
 * @return size_t -> aligned byte count
 */
static size_t roundup(size_t sz)
{
    if (sz > DECODER_MAX_WORKING_BYTES - 3)
    {
        __builtin_trap();
    }
    return (sz + 3) & ~(size_t)3;
}

// Decode results

typedef struct decode_results
{
    int sampling_rate;
    int num_channels;
    int num_samples;
} decode_results_t;

static decode_results_t g_decode_results;

/**
 * @brief reads the sampling rate from the last scan or decode
 *
 * @return int -> sampling rate in Hz
 */
WASM_EXPORT
int decode_results_sampling_rate()
{
    return g_decode_results.sampling_rate;
}

/**
 * @brief reads the channel count from the last scan or decode
 *
 * @return int -> decoded channel count
 */
WASM_EXPORT
int decode_results_num_channels()
{
    return g_decode_results.num_channels;
}

/**
 * @brief reads the sample count from the last scan or decode
 *
 * @return int -> sample count including all channels
 */
WASM_EXPORT
int decode_results_num_samples()
{
    return g_decode_results.num_samples;
}

// Decoder

static struct
{
    mp3dec_t mp3d;

    size_t mp3_data_size;
    size_t pcm_data_size;

    size_t byte_offset;
    double current_time;
} g_decoder;

/**
 * @brief initializes the MP3 codec state
 *
 * @return void
 */
WASM_EXPORT
void decoder_init()
{
    mp3dec_init(&g_decoder.mp3d);
}

/**
 * @brief exposes the input region at the heap base
 *
 * @return const uint8_t* -> address where the caller places MP3 bytes
 */
WASM_EXPORT
const uint8_t *decoder_mp3_data_offset()
{
    return &__heap_base;
}

/**
 * @brief reads the configured MP3 input length
 *
 * @return size_t -> input byte count
 */
WASM_EXPORT
size_t decoder_mp3_data_size()
{
    return g_decoder.mp3_data_size;
}

/**
 * @brief reserves input memory and resets the read position and PCM length
 *
 * @param size_t size -> MP3 byte count within the input limit
 *
 * @return void
 */
WASM_EXPORT
void decoder_set_mp3_data_size(size_t size)
{
    if (size > DECODER_MAX_INPUT_BYTES)
    {
        __builtin_trap();
    }
    size_t aligned = roundup(size);
    grow_memory_if_needed(aligned);

    g_decoder.mp3_data_size = size;
    g_decoder.pcm_data_size = 0;

    g_decoder.byte_offset = 0;
    g_decoder.current_time = 0.0;
}

/**
 * @brief locates PCM output after the aligned input region
 *
 * @return const uint8_t* -> PCM output address in linear memory
 */
WASM_EXPORT
const uint8_t *decoder_pcm_data_offset()
{
    return decoder_mp3_data_offset() + roundup(g_decoder.mp3_data_size);
}

/**
 * @brief reads the last decoded PCM output length
 *
 * @return size_t -> PCM byte count
 */
WASM_EXPORT
size_t decoder_pcm_data_size()
{
    return g_decoder.pcm_data_size;
}

/**
 * @brief reads the current position tracked by scan and decode operations
 *
 * @return double -> position in seconds
 */
WASM_EXPORT
double decoder_current_time()
{
    return g_decoder.current_time;
}

/**
 * @brief scans complete frames before the requested duration or input end
 *
 * @param const uint8_t* mp3 -> MP3 input bytes
 * @param size_t mp3_bytes -> available byte count
 * @param double target_duration_in_seconds -> duration limit, or a negative value to scan all input
 * @param decode_results_t* results -> output sample rate, channel count and total samples
 * @param size_t* num_bytes -> output consumed byte count
 * @param double* actual_duration -> output scanned duration in seconds
 *
 * @return void
 */
static void seek_internal(const uint8_t *mp3, size_t mp3_bytes, double target_duration_in_seconds, decode_results_t *results, size_t *num_bytes, double *actual_duration)
{
    memset(results, 0, sizeof(decode_results_t));
    *num_bytes = 0;
    *actual_duration = 0.0;

    mp3dec_frame_info_t frame;
    int samples;
    int first_frame = 1; // Becomes zero after the first frame.

    while (*num_bytes < mp3_bytes)
    {
        samples = mp3dec_decode_frame(&g_decoder.mp3d, mp3 + *num_bytes, mp3_bytes - *num_bytes, NULL, &frame);
        if (frame.frame_bytes < 0 || (size_t)frame.frame_bytes > mp3_bytes - *num_bytes)
        {
            __builtin_trap();
        }
        if (samples == 0)
        {
            if (frame.frame_bytes > 0)
            {
                // Skipped ID3 or invalid data.
                *num_bytes += frame.frame_bytes;
                continue;
            }
            // Insufficient data
            break;
        }

        if (first_frame)
        {
            results->sampling_rate = frame.hz;
            results->num_channels = frame.channels;
            first_frame = 0;
        }
        else
        {
            if (frame.hz != results->sampling_rate || frame.channels != results->num_channels)
            {
                break;
            }
        }

        double advance_in_seconds = (double)samples / (double)frame.hz;
        if (target_duration_in_seconds >= 0.0 && *actual_duration + advance_in_seconds >= target_duration_in_seconds)
        {
            break;
        }
        if (frame.frame_bytes <= 0 || (size_t)frame.frame_bytes > mp3_bytes - *num_bytes || samples < 0 || samples > MINIMP3_MAX_SAMPLES_PER_FRAME || frame.channels < 1 || frame.channels > 2 || (size_t)results->num_samples + (size_t)samples * frame.channels > (DECODER_MAX_WORKING_BYTES - DECODER_MAX_INPUT_BYTES - 8) / sizeof(mp3d_sample_t))
        {
            __builtin_trap();
        }
        *actual_duration += advance_in_seconds;
        *num_bytes += frame.frame_bytes;
        results->num_samples += samples * frame.channels;
    }
}

/**
 * @brief scans from the input start and updates the decoder position
 *
 * @param double position_in_seconds -> requested position in seconds, or a negative value for input end
 *
 * @return void
 */
WASM_EXPORT
void decoder_seek(double position_in_seconds)
{
    decode_results_t results;
    size_t num_bytes;
    double duration;

    const uint8_t *mp3 = decoder_mp3_data_offset();
    size_t mp3_bytes = decoder_mp3_data_size();

    seek_internal(mp3, mp3_bytes, position_in_seconds, &results, &num_bytes, &duration);

    clib__copy_memory(&results, &g_decode_results, sizeof(results), sizeof(g_decode_results));
    g_decoder.byte_offset = num_bytes;
    g_decoder.current_time = duration;
}

/**
 * @brief decodes frames into PCM storage using the preceding scan results
 *
 * @param const uint8_t* mp3 -> MP3 input bytes
 * @param size_t mp3_bytes -> available byte count
 * @param decode_results_t* results -> expected sample count from the scan
 *
 * @return void
 */
static void decode_internal(const uint8_t *mp3, size_t mp3_bytes, decode_results_t *results)
{
    mp3dec_frame_info_t frame;
    int samples;
    size_t num_bytes = 0;
    int num_samples = 0;

    mp3d_sample_t *pcm = (mp3d_sample_t *)decoder_pcm_data_offset();
    mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (num_bytes < mp3_bytes && num_samples < results->num_samples)
    {
        samples = mp3dec_decode_frame(&g_decoder.mp3d, mp3 + num_bytes, mp3_bytes - num_bytes, frame_pcm, &frame);
        if (frame.frame_bytes <= 0 || (size_t)frame.frame_bytes > mp3_bytes - num_bytes || samples < 0 || frame.channels < 0 || frame.channels > 2 || samples * frame.channels > results->num_samples - num_samples)
        {
            __builtin_trap();
        }
        clib__copy_memory(frame_pcm, pcm + num_samples, (size_t)samples * frame.channels * sizeof(mp3d_sample_t), (size_t)(results->num_samples - num_samples) * sizeof(mp3d_sample_t));
        num_bytes += frame.frame_bytes;
        num_samples += samples * frame.channels;
    }
}

/**
 * @brief decodes the next bounded duration and advances the stored input position
 *
 * @param double duration_in_seconds -> duration to decode, or a negative value for the remaining input
 *
 * @return void
 */
WASM_EXPORT
void decoder_decode(double duration_in_seconds)
{
    decode_results_t results;
    size_t num_bytes = 0;
    double duration = 0;

    const uint8_t *mp3 = decoder_mp3_data_offset() + g_decoder.byte_offset;
    size_t mp3_bytes = decoder_mp3_data_size() - g_decoder.byte_offset;

    // Seek to the target duration to calculate frames to decode.
    seek_internal(mp3, mp3_bytes, duration_in_seconds, &results, &num_bytes, &duration);

    // Allocate memory for PCM.
    size_t pcm_data_size = (size_t)results.num_samples * sizeof(mp3d_sample_t);
    size_t required_bytes = roundup(decoder_mp3_data_size()) + pcm_data_size;
    grow_memory_if_needed(required_bytes);
    g_decoder.pcm_data_size = pcm_data_size;

    // Decode frames.
    decode_internal(mp3, mp3_bytes, &results);

    // Update decoder state.
    clib__copy_memory(&results, &g_decode_results, sizeof(results), sizeof(g_decode_results));
    g_decoder.byte_offset += num_bytes;
    g_decoder.current_time += duration;
}
