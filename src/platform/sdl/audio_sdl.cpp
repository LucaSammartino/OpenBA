// ============================================================================
// src/platform/sdl/audio_sdl.cpp -- Platform::Audio over SDL3 (Phase D.4 SDL-4, plan A4 / A5)
// ============================================================================
// Replaces src/platform/win/audio_xaudio2_win.cpp (XAudio 2.7 over COM), deleted in
// the same commit: one engine handle serves the one-shots and the music, so both
// switch at once, and two backends of platform_audio.h cannot link together.
//
// THE MAPPING (plans/SDL.md A.5):
//   engine  = SDL_InitSubSystem(SDL_INIT_AUDIO) (main's SDL_Init ran already)
//   master  = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL): the
//             default output at its own format, not paused at open; SDL follows a
//             default-device change itself
//   voice   = an SDL_AudioStream whose input is the buffer's PCM, bound to that
//             device while it is started; SDL converts and resamples to the device
//   SetVolume          -> SDL_SetAudioStreamGain
//   SetFrequencyRatio  -> SDL_SetAudioStreamFrequencyRatio
//   Start / Stop       -> SDL_BindAudioStream / SDL_UnbindAudioStream
//   Submit / GetState  -> the voice's AudioFeed (src/platform/common/audio_feed.h),
//                         fed to SDL from the stream's get-callback
//
// A failed engine or device = a silent game, no message box: the result is non-zero
// and *out stays untouched, which Device::PlatformStartup turns into a null engine
// (retail's XAudio2 path does the same when the DirectX runtime is missing).
//
// THE DATA IS PCM. Every shipped sound is xWMA; SDL plays PCM. A one-shot
// SoundBuffer is decoded at load (SoundBuffer::DecodeAtLoad) and arrives here as
// S16 PCM with a PCM format. The music voice is created with the streamed buffer's
// xWMA format, and its buffers are the S16 PCM the Stream worker decodes
// chunk-ahead (Audio::Stream::Run): a 0x0161 format therefore means "S16 at these
// channels and this rate". Nothing is decoded on the audio thread.
//
// THE CALLBACKS. The get-callback runs on SDL's device thread once per mixing pass,
// with `additional` = the bytes the stream needs now. It hands that much of the
// queued PCM to the stream (SDL_PutAudioStreamData, which copies; on a voice's
// first pull plus the resampler padding SDL leaves out of that first request, see
// kFirstPullMarginFrames) and reports, in
// IXAudio2VoiceCallback terms, to the voice's AudioVoiceCallback (the game's
// Device::PlatformChannel record):
//   OnVoiceProcessingPassStart(additional) / OnVoiceProcessingPassEnd() around it,
//   OnBufferStart / OnLoopEnd / OnBufferEnd per buffer (AudioFeed: a buffer ends when
//     its last byte is handed to SDL, at most one device period before it is heard),
//   OnStreamEnd once, after the END_OF_STREAM buffer was handed over, the stream
//     flushed (SDL_FlushAudioStream), and a later pass finds the stream's OUTPUT side
//     empty (SDL_GetAudioStreamAvailable == 0: the converted frames are all consumed;
//     SDL_GetAudioStreamQueued counts INPUT and would fire early).
// The game's bodies are an EventSet and a non-atomic `flags |= 2` (retail's race,
// kept). SDL calls the get-callback holding the stream's own mutex
// (SDL_audiocvt.c, SDL_GetAudioStreamDataAdjustGain); the two bodies take no lock,
// so no thread can deadlock on it, and the main thread's own calls on the stream
// (gain, ratio) wait at most one callback.
//
// AudioVoiceDestroy DOES NOT RETURN WHILE A CALLBACK FOR THE VOICE RUNS (XAudio2's
// DestroyVoice guarantee), proved from SDL 3.4.18's source:
//   1. SDL_UnbindAudioStream locks the PHYSICAL device's mutex before it unlinks the
//      stream (SDL_audio.c, SDL_UnbindAudioStreams: "lock the device first, and the
//      stream second, as that is the order the audio thread will do it"). The device
//      thread holds that same mutex for its whole mixing pass, which is where it
//      calls SDL_GetAudioStreamDataAdjustGain on each bound stream and so our
//      get-callback (SDL_PlaybackAudioThreadIterate). Unbind therefore waits for a
//      running callback to return, and afterwards the device thread no longer sees
//      the stream.
//   2. SDL_SetAudioStreamGetCallback(s, NULL, NULL) takes the stream's mutex, which a
//      running callback holds (SDL_audiocvt.c): a second wait, for a pull made by any
//      other thread, and no callback can start after it.
//   3. SDL_DestroyAudioStream then unbinds (a no-op now) and frees.
// The plan marked "does SDL_DestroyAudioStream wait" UNVERIFIED: by itself it does
// (its unbind takes the device lock), but the explicit order above does not rely on
// that.
//
// LIFETIME AT SHUTDOWN. Device::PlatformShutdown destroys the master then releases
// the engine; live voices are never destroyed after that (Engine::Shutdown frees the
// Device without FreeChannelPlatform), and the Stream worker may still ask a voice
// for BuffersQueued until ~Stream joins it. SDL_QuitSubSystem(SDL_INIT_AUDIO)
// destroys every stream unless SDL_PROP_AUDIOSTREAM_AUTO_CLEANUP_BOOLEAN is false,
// so each voice's stream sets it false: after the release a voice is an unbound,
// silent stream that still answers (the process exits next). Retail's XAudio2
// voices are released with the engine instead.
//
// WHAT REACHES THE SOUND SERVER AT EXIT (row 1282, read against SDL 3.4.18). The retail
// quit path (MainMenuDialog Quit -> MainLoop::Run returns -> Engine::Shutdown ->
// Device::PlatformShutdown) runs AudioMasterDestroy, then AudioEngineRelease, before
// main's SDL_Quit and the return from main:
//   1. SDL_CloseAudioDevice drops the one logical device; with none left it closes the
//      physical device (SDL_audio.c), which joins the device thread and, on PulseAudio,
//      runs pa_stream_disconnect + pa_stream_unref with no drain (SDL_pulseaudio.c,
//      PULSEAUDIO_CloseDevice): the server holds no stream of ours after it.
//   2. SDL_QuitSubSystem(SDL_INIT_AUDIO) is the last reference (main inits VIDEO only),
//      so the backend deinitializes: DisconnectFromPulseServer drops the context and
//      stops the threaded mainloop.
// The voices' SDL_AudioStreams outlive it, unbound; they are in-process converters
// and hold nothing on the server. The crash, hang and fatal-error exits (_exit) skip
// all of this, so the server sees only the socket close.
//
// THE HANDLES. platform_audio.h's three handle types are pointers to incomplete
// types; this file defines them (as cursor_sdl.cpp defines CursorObject), so no
// pointer is reinterpreted.
// ============================================================================

#include "platform_audio.h"
#include "audio_feed.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>

#include <cstdint>
#include <cstdio>

struct Platform::AudioEngineObject {
    SDL_AudioDeviceID device = 0;          // set by AudioMasterCreate
};

struct Platform::AudioMasterObject {
    AudioEngineObject* engine;
};

struct Platform::AudioVoiceObject {
    explicit AudioVoiceObject(uint32_t fb) : frameBytes(fb ? fb : 1u), feed(fb) {}
    SDL_AudioStream*    stream = nullptr;
    SDL_AudioDeviceID   device = 0;
    AudioVoiceCallback* callback = nullptr;
    float               maxRatio = 2.0f;
    bool                bound = false;            // main thread only (Start / Stop / Destroy)
    // Under the stream's mutex (Submit / BuffersQueued / the get-callback):
    bool                drainPending = false;     // END_OF_STREAM handed over and flushed
    bool                ended = false;            // OnStreamEnd delivered
    bool                primed = false;           // the first pull handed something over
    uint32_t            frameBytes;
    AudioFeed           feed;
};

namespace Platform {

namespace {

// HRESULT-shaped failures (platform_audio.h: 0 = success). E_FAIL / E_INVALIDARG /
// XAUDIO2_E_INVALID_CALL, the codes XAudio2 would have returned for the same cases.
constexpr int32_t kFail        = static_cast<int32_t>(0x80004005u);
constexpr int32_t kInvalidArg  = static_cast<int32_t>(0x80070057u);
constexpr int32_t kInvalidCall = static_cast<int32_t>(0x88960001u);

// XAUDIO2_MIN_FREQ_RATIO is 1/1024; SDL accepts 0.01 .. 100 (SDL_audio.h).
constexpr float kSdlMinRatio = 0.01f;

// The input spec for a voice's format: PCM 8 / 16 / 32-bit, IEEE float, or xWMA
// (the music: the Stream worker submits S16 it decoded). False for anything else.
bool SpecFor(const AudioWaveFormat& f, SDL_AudioSpec* spec) {
    if (f.channels == 0 || f.samplesPerSec == 0) return false;
    switch (f.formatTag) {
    case kAudioFormatPcm:
        if (f.bitsPerSample == 8)       spec->format = SDL_AUDIO_U8;
        else if (f.bitsPerSample == 16) spec->format = SDL_AUDIO_S16LE;
        else if (f.bitsPerSample == 32) spec->format = SDL_AUDIO_S32LE;
        else return false;
        break;
    case 0x0003:                                  // WAVE_FORMAT_IEEE_FLOAT
        if (f.bitsPerSample != 32) return false;
        spec->format = SDL_AUDIO_F32LE;
        break;
    case kAudioFormatXwma:
        spec->format = SDL_AUDIO_S16LE;
        break;
    default:
        return false;
    }
    spec->channels = f.channels;
    spec->freq     = static_cast<int>(f.samplesPerSec);
    return true;
}

// AudioFeed's sink inside the get-callback: the bytes go to SDL, the events to the
// game's record.
class CallbackSink final : public AudioFeed::Sink {
public:
    explicit CallbackSink(AudioVoiceObject* v) : _v(v) {}
    void Put(const uint8_t* data, uint32_t bytes) override {
        SDL_PutAudioStreamData(_v->stream, data, static_cast<int>(bytes));
    }
    void BufferStart() override { _v->callback->OnBufferStart(nullptr); }
    void LoopEnd() override     { _v->callback->OnLoopEnd(nullptr); }
    void BufferEnd() override   { _v->callback->OnBufferEnd(nullptr); }
    void EndOfStreamPut() override {
        SDL_FlushAudioStream(_v->stream);
        _v->drainPending = true;
    }
private:
    AudioVoiceObject* _v;
};

// THE FIRST PULL IS UNDER-ASKED (Phase D.4 CLICK). SDL sizes `additional` as the input
// frames that convert to the output it needs (SDL_audiocvt.c, SDL_GetAudioStreamDataAdjustGain:
// SDL_GetResamplerInputFrames of the request minus what is available). While the voice's
// track is not flushed, its resampler holds back RESAMPLER_MAX_PADDING_FRAMES of input as
// the filter's right-hand context (SDL_audioresample.c: 7 frames with SSE); from the second
// pull on that hold-back is inside "available" and so inside the request, but on a fresh
// stream nothing is queued and the first request leaves it out. Handing over exactly
// `additional` therefore converts 7 to 8 frames fewer than the device period, and the device
// fills the end of that period with silence: a 0.15 ms notch one period into every resampled
// voice (every one-shot and every ambience, since the device runs at its own rate and the
// pitch ratio resamples the rest). XAudio2's SRC primes itself and has no such gap. The first
// pull hands over the padding too; the surplus sits in the queue and shrinks the next
// request, so nothing else moves. Measured in tools/audio_tail/sdl_pull_check.cpp: 375 short
// first pulls out of 375 resampled scenarios without the margin, 0 with it, and 0 short
// pulls anywhere else in 20000 passes per scenario.
constexpr uint32_t kFirstPullMarginFrames = 8;   // RESAMPLER_MAX_PADDING_FRAMES (7) + rounding

// SDL's device thread, the stream's mutex held (see the banner).
void SDLCALL GetCallback(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
    AudioVoiceObject* const v = static_cast<AudioVoiceObject*>(userdata);   // CAST-OK: user_data
    v->callback->OnVoiceProcessingPassStart(static_cast<uint32_t>(additional > 0 ? additional : 0));
    if (v->drainPending && !v->ended && SDL_GetAudioStreamAvailable(stream) == 0) {
        v->ended = true;
        v->callback->OnStreamEnd();
    }
    CallbackSink sink(v);
    uint32_t want = static_cast<uint32_t>(additional > 0 ? additional : 0);
    if (!v->primed && want > 0) {
        want += kFirstPullMarginFrames * v->frameBytes;
        v->primed = true;
    }
    v->feed.Pull(want, sink);
    v->callback->OnVoiceProcessingPassEnd();
}

}  // namespace

// ---- the engine and the master ------------------------------------------------
int32_t AudioEngineCreate(AudioEngine* out) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[Audio] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
        return kFail;
    }
    *out = new AudioEngineObject();
    return 0;
}

void AudioEngineRelease(AudioEngine engine) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    delete engine;
}

int32_t AudioMasterCreate(AudioEngine engine, AudioMaster* out) {
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (dev == 0) {
        fprintf(stderr, "[Audio] SDL_OpenAudioDevice(default playback) failed: %s\n", SDL_GetError());
        return kFail;
    }
    engine->device = dev;
    *out = new AudioMasterObject{engine};
    return 0;
}

void AudioMasterDestroy(AudioMaster master) {
    // Closing the device unbinds every stream still bound to it.
    SDL_CloseAudioDevice(master->engine->device);
    master->engine->device = 0;
    delete master;
}

// ---- a source voice --------------------------------------------------------------
int32_t AudioVoiceCreate(AudioEngine engine, AudioVoice* out, const AudioWaveFormat* format,
                         float maxFrequencyRatio, AudioVoiceCallback* callback) {
    SDL_AudioSpec spec{};
    if (!SpecFor(*format, &spec)) return kInvalidArg;
    SDL_AudioStream* const stream = SDL_CreateAudioStream(&spec, nullptr);   // output side: the device's, at bind
    if (stream == nullptr) return kFail;

    AudioVoiceObject* const v = new AudioVoiceObject(static_cast<uint32_t>(SDL_AUDIO_FRAMESIZE(spec)));
    v->stream   = stream;
    v->device   = engine->device;
    v->callback = callback;
    v->maxRatio = maxFrequencyRatio;
    SDL_SetBooleanProperty(SDL_GetAudioStreamProperties(stream),
                           SDL_PROP_AUDIOSTREAM_AUTO_CLEANUP_BOOLEAN, false);   // the banner's LIFETIME
    SDL_SetAudioStreamGetCallback(stream, GetCallback, v);
    
    *out = v;
    return 0;
}

void AudioVoiceDestroy(AudioVoice voice) {
    // The order is the guarantee (the banner's proof): unbind waits out the device
    // thread's pass, clearing the callback waits out any pull on the stream's mutex.
    SDL_UnbindAudioStream(voice->stream);
    SDL_SetAudioStreamGetCallback(voice->stream, nullptr, nullptr);
    SDL_DestroyAudioStream(voice->stream);
    delete voice;
}

int32_t AudioVoiceStart(AudioVoice voice) {
    // XAudio2's Start(0): the voice consumes its queue from where it stands. A
    // started voice stays started.
    if (voice->bound) return 0;
    if (!SDL_BindAudioStream(voice->device, voice->stream)) return kInvalidCall;
    voice->bound = true;
    return 0;
}

int32_t AudioVoiceStop(AudioVoice voice) {
    // XAudio2's Stop(0): consumption stops at once, the queue and the position stay.
    // Unbinding returns after the device thread's current pass (the banner, point 1),
    // so nothing more is pulled; the up-to-one-period SDL already converted is kept
    // in the stream and plays if the voice starts again.
    if (!voice->bound) return 0;
    SDL_UnbindAudioStream(voice->stream);
    voice->bound = false;
    return 0;
}

int32_t AudioVoiceSetVolume(AudioVoice voice, float volume) {
    // XAudio2 applies it at the next 10 ms pass, SDL at the next pull.
    return SDL_SetAudioStreamGain(voice->stream, volume) ? 0 : kInvalidArg;
}

int32_t AudioVoiceSetFrequencyRatio(AudioVoice voice, float ratio) {
    // XAudio2 clamps to [XAUDIO2_MIN_FREQ_RATIO, the create call's maximum] and
    // succeeds; SDL refuses values outside 0.01 .. 100, so clamp to what both allow.
    if (ratio > voice->maxRatio) ratio = voice->maxRatio;
    if (ratio < kSdlMinRatio) ratio = kSdlMinRatio;
    return SDL_SetAudioStreamFrequencyRatio(voice->stream, ratio) ? 0 : kInvalidArg;
}

int32_t AudioVoiceSubmit(AudioVoice voice, bool endOfStream, const uint8_t* data,
                         uint32_t bytes, uint32_t playBegin, uint32_t loopCount) {
    SDL_LockAudioStream(voice->stream);
    const bool queued = voice->feed.Submit(data, bytes, playBegin, loopCount, endOfStream);
    if (queued) {
        voice->drainPending = false;   // a new buffer after an end restarts the voice
        voice->ended = false;
    }
    SDL_UnlockAudioStream(voice->stream);
    return queued ? 0 : kInvalidCall;
}

uint32_t AudioVoiceBuffersQueued(AudioVoice voice) {
    SDL_LockAudioStream(voice->stream);
    const uint32_t n = voice->feed.Queued();
    SDL_UnlockAudioStream(voice->stream);
    return n;
}

}  // namespace Platform
