#include "audio_mixer.h"
constexpr uint32_t soundMethodBase=0x7c000000;
constexpr uint32_t soundBadFormat=0x88780064;
PcmMixer soundMixer;
struct SoundObject {
    uint32_t kind,table,references=1,flags=0,guestSamples=0,volume=0,pan=0;
    std::shared_ptr<PcmVoice> voice;
    bool locked=false;
    uint32_t lockOffset=0,lockBytes=0;
    std::chrono::steady_clock::time_point lastPublish{};
    std::chrono::microseconds maxPublishGap{};
};
std::map<uint32_t,SoundObject> soundObjects;
std::map<uint32_t,uint32_t> soundMethodCalls;
thread_local uint32_t soundToken{};
// Diagnostics for silent or missing music ([NFSU2 AUDIO] lines): buffer creation, Play, Stop, and SetVolume /
// SetPan when the value changes. Only the first audioLogBudget lines of a run are written.
uint32_t audioLogBudget=1500;
std::map<uint64_t,int32_t> audioLastValue;
#define AUDIO_LOG(...) do { if(audioLogBudget){ --audioLogBudget; NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2 AUDIO",__VA_ARGS__); \
    if(!audioLogBudget) NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2 AUDIO","log budget used up; later audio calls are not logged"); } } while(0)
// Guards soundObjects/soundMethodCalls and every DirectSound object: buffer methods also run on the free
// sound thread without the guest machine (R36S). soundDispatch never takes the machine while holding it.
std::recursive_mutex soundStateMutex;
#ifdef __ANDROID__
AAudioStream* soundStream=nullptr;
int32_t soundLastXruns=0;
std::atomic<uint32_t> soundCallbackPeakUs{0};
aaudio_data_callback_result_t soundCallback(AAudioStream*,void*,void* data,int32_t frames){
    auto start=std::chrono::steady_clock::now();
    soundMixer.render(static_cast<float*>(data),unsigned(frames));
    uint32_t us=uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
    auto peak=soundCallbackPeakUs.load(std::memory_order_relaxed);
    while(us>peak&&!soundCallbackPeakUs.compare_exchange_weak(peak,us,std::memory_order_relaxed)){}
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
bool startSoundStream(){
    if(soundStream)return true;AAudioStreamBuilder* builder=nullptr;
    if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)return false;
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder,soundCallback,nullptr);
    auto status=AAudioStreamBuilder_openStream(builder,&soundStream);AAudioStreamBuilder_delete(builder);
    if(status!=AAUDIO_OK){soundStream=nullptr;return false;}
    soundMixer.rate=unsigned(AAudioStream_getSampleRate(soundStream));
    int32_t burst=AAudioStream_getFramesPerBurst(soundStream);
    if(burst>0)AAudioStream_setBufferSizeInFrames(soundStream,burst*2);
    soundLastXruns=0;soundCallbackPeakUs=0;
    status=AAudioStream_requestStart(soundStream);
    if(status!=AAUDIO_OK){AAudioStream_close(soundStream);soundStream=nullptr;return false;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","AAudio stereo float stream started: %u Hz",soundMixer.rate);return true;
}
// Called on the guest thread after Present, never from the real-time callback.
// Grow only after an actual underrun, one burst at a time, bounded by capacity.
void serviceGuestSound(){
    if(!soundStream)return;
    int32_t xruns=AAudioStream_getXRunCount(soundStream),burst=AAudioStream_getFramesPerBurst(soundStream);
    int32_t size=AAudioStream_getBufferSizeInFrames(soundStream),capacity=AAudioStream_getBufferCapacityInFrames(soundStream);
    if(xruns>soundLastXruns&&burst>0&&size<capacity&&size<burst*8)
        AAudioStream_setBufferSizeInFrames(soundStream,std::min({size+burst,capacity,burst*8}));
    if(xruns>=0)soundLastXruns=xruns;
    uint64_t clipped;float peak;unsigned active=0;double publishGapMs=0,ringMs=0;
    {std::lock_guard<std::mutex> lock(soundMixer.mutex);
        clipped=soundMixer.clippedSamples;peak=soundMixer.peakAmplitude;
        soundMixer.clippedSamples=0;soundMixer.peakAmplitude=0;
        for(const auto& voice:soundMixer.voices)if(voice&&voice->playing)++active;
        for(auto& item:soundObjects){auto& object=item.second;auto& voice=object.voice;
            if(voice&&voice->playing&&voice->loop){
                publishGapMs=std::max(publishGapMs,double(object.maxPublishGap.count())/1000);
                ringMs=std::max(ringMs,1000.0*voice->samples.size()/voice->block()/voice->frequency);
            }
            object.maxPublishGap={};}}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","Audio producer: maxPublishGap=%.2f ms ring=%.2f ms",publishGapMs,ringMs);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","Audio mix: active=%u peak=%.4f clipped=%llu",active,double(peak),static_cast<unsigned long long>(clipped));
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","Audio health: xruns=%d buffer=%d/%d burst=%d rate=%u callbackPeak=%u us mode=%d",
        xruns,AAudioStream_getBufferSizeInFrames(soundStream),capacity,burst,soundMixer.rate,
        soundCallbackPeakUs.exchange(0,std::memory_order_relaxed),int(AAudioStream_getPerformanceMode(soundStream)));
}
#elif defined(NFS_SDL_AUDIO)
static SDL_AudioDeviceID s_sdlAudioDevice = 0;
static void sdlAudioCallback(void*, Uint8* stream, int len) {
    unsigned frames = unsigned(len / (sizeof(float) * 2));
    soundMixer.render(reinterpret_cast<float*>(stream), frames);
    // Every ~2 s: the output's peak and the voices playing (is the game's sound silence, or not there?)
    static unsigned rendered = 0; static float peak = 0;
    const float* out = reinterpret_cast<const float*>(stream);
    for (unsigned i = 0; i < frames * 2; ++i) peak = std::max(peak, std::fabs(out[i]));
    if ((rendered += frames) >= soundMixer.rate * 2) {
        unsigned playing = 0, voices = 0; size_t bytes = 0; uint64_t stale = 0, played = 0;
        {
            std::lock_guard<std::mutex> lock(soundMixer.mutex);
            for (auto& v : soundMixer.voices) if (v) { ++voices; if (v->playing) { ++playing; bytes += v->samples.size(); }
                stale += v->staleGranules; played += v->playedGranules; v->staleGranules = v->playedGranules = 0; }
        }
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU2", "[AS2 AUDIO] peak %.3f, %u voices, %u playing (%zu bytes); stream stretches replayed %llu of %llu",
                        peak, voices, playing, bytes, (unsigned long long)stale, (unsigned long long)played);
        rendered = 0; peak = 0;
    }
}
void serviceGuestSound() {}
bool startSoundStream() {
    if (s_sdlAudioDevice) return true;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN, "NFSU2", "SDL audio init failed: %s", SDL_GetError());
            return false;
        }
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU2", "SDL audio driver: %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = 44100;
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    // FO2_AUDIO_SAMPLES: frames per callback (the play cursor the game sees moves by this much at a time)
    desired.samples = Uint16([] { const char* e = std::getenv("FO2_AUDIO_SAMPLES"); const int n = e ? std::atoi(e) : 1024; return n >= 64 && n <= 8192 ? n : 1024; }());
    desired.callback = sdlAudioCallback;
    s_sdlAudioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_sdlAudioDevice) {
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN, "NFSU2", "SDL audio device open failed: %s", SDL_GetError());
        return false;
    }
    soundMixer.rate = unsigned(obtained.freq);
    SDL_PauseAudioDevice(s_sdlAudioDevice, 0);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU2", "SDL2 stereo float audio started: %u Hz", soundMixer.rate);
    return true;
}
#else
void serviceGuestSound(){}
bool startSoundStream(){return true;} // Host tests inspect the mixer; no physical audio output.
#endif
void shutdownGuestSound(){
#ifdef __ANDROID__
    if(soundStream){AAudioStream_requestStop(soundStream);AAudioStream_close(soundStream);soundStream=nullptr;}
#elif defined(NFS_SDL_AUDIO)
    if(s_sdlAudioDevice){SDL_CloseAudioDevice(s_sdlAudioDevice);s_sdlAudioDevice=0;}
#endif
    for(auto& item:soundObjects){auto& object=item.second;if(object.voice)soundMixer.remove(object.voice);
        if(object.guestSamples)heaps->free(GuestHeaps::process,object.guestSamples);
        heaps->free(GuestHeaps::process,object.table);heaps->free(GuestHeaps::process,item.first);}
    soundObjects.clear();
}
uint32_t createSoundObject(uint32_t kind,uint32_t flags=0){std::lock_guard<std::recursive_mutex> l(soundStateMutex);
    uint32_t address=allocate(4),table=allocate(24*4);
    for(uint32_t slot=0;slot<24;++slot)write32(table+4*slot,soundMethodBase+kind*0x1000+slot*16);
    write32(address,table);soundObjects.emplace(address,SoundObject{kind,table,1,flags});return address;
}
uint16_t soundRead16(uint32_t address){uint16_t value;std::memcpy(&value,ptr(address,2),2);return value;}
bool soundFormat(uint32_t address,PcmVoice& voice){
    if(!address)return false;unsigned channels=soundRead16(address+2),rate=read32(address+4),bits=soundRead16(address+14);
    if(soundRead16(address)!=1||(channels!=1&&channels!=2)||(bits!=8&&bits!=16)||rate<100||rate>200000)return false;
    if(soundRead16(address+12)!=channels*(bits/8)||read32(address+8)!=rate*channels*(bits/8))return false;
    voice.channels=channels;voice.rate=voice.frequency=rate;voice.bits=bits;return true;
}
void directSoundCreate(){
    if(!arg(1)){ret(0x80004003,3);return;}write32(arg(1),0);
    if(arg(2)){ret(0x80040110,3);return;}
    // A device GUID (Alien Shooter 2 passes one, e.g. DSDEVID_DefaultPlayback) gets the one output there
    // is, the host's default playback device, rather than DSERR_NODRIVER and a silent game.
    if(arg(0))NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","DirectSoundCreate for device %08x-...: the default output",read32(arg(0)));
    // AS2_SOUND_TRACE: the code addresses on the guest stack (who set the sound system up, with what)
    static const bool trace=std::getenv("AS2_SOUND_TRACE")!=nullptr;
    if(trace){std::string line;char item[16];
        for(uint32_t i=0;i<128;++i){std::snprintf(item,sizeof item," %08x",read32(g_esp+4*i));line+=item;if(i%16==15){std::printf("[FO2] DirectSoundCreate stack%s\n",line.c_str());line.clear();}}}
    if(!startSoundStream()){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","DirectSoundCreate: host audio stream failed");ret(0x88780078,3);return;}
    write32(arg(1),createSoundObject(1));ret(0,3);
}
// FO2_FMOD_WATCH=1: FMOD 3.75's software channels (an array of 0x1DC-byte records at [[0x684570]+0x14A8], "playing"
// is the dword at +0x5C, 0x61EF01), looked at each time FMOD publishes a mixed block: when a channel starts and
// ends, with its record dumped for the first events. Looks for why the debris sounds end almost at once.
// FMOD starts every sound at the listener (0x616054: gain 255). The game then gives its position through
// FSOUND_3D_SetAttributes (0x61AB73), but the gain follows only at the next FSOUND_Update (0x61D72F), so the mixer
// could play the first block of a distant sound at full volume: a loud attack that drops at once, heard as a sound
// cut half way (56 of 130 attenuated sounds). At that call, for a channel not yet mixed further than its first
// block, the gain 0x61D375 will compute is applied now. It only lowers the gain. FO2_FMOD_STARTGAIN=0 disables.
void fmodStartGain(uint32_t handle,uint32_t position){
    static const bool on=[]{const char* e=std::getenv("FO2_FMOD_STARTGAIN");return !e||e[0]!='0';}();
    if(!on||!position||handle==0xFFFFFFFDu)return;
    const uint32_t state=read32(0x684570u);if(!state)return;
    const uint32_t channels=read32(state+0x14A8),index=handle&0xFFFu;
    if(!channels||index>=read32(state+0x14EC)+read32(state+0x14D4))return;
    const uint32_t c=channels+index*0x1DC;
    // one-shots only: a loop (engine) comes back under 2048 at every turn, and its first block is not what is heard as a cut
    const uint32_t startSample=read32(c+0x5C);
    if(!startSample||(read32(startSample+0x124)&2)||read32(c+0x80)>=2048u||(read32(c+0x40)&0x82000u)||(read32(c+0x3C)&0x20u))return;
    const uint32_t oldGain=read32(c+0x78);if(!oldGain||oldGain>255u)return;
    const auto rf=[](uint32_t va){uint32_t raw=read32(va);float v;std::memcpy(&v,&raw,4);return v;};
    const float rolloff=rf(0x68460Cu);if(rolloff==0.f)return;
    const float dx=rf(position)-rf(0x6A31C8u),dy=rf(position+4)-rf(0x6A31CCu),dz=rf(position+8)-rf(0x6A31D0u),mn=rf(c+0xBC),mx=rf(c+0xC0);
    float d=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(d>mx)d=mx;
    if(d<mn)d=mn;
    if(d>mn&&rolloff!=1.f)d=mn+(d-mn)*rolloff;
    const float gainF=std::min(1.f,std::max(0.f,mn/std::max(d,0.00001f)));
    const uint32_t gain=uint32_t(gainF*255.f);
    if(gain>=oldGain)return;
    write32(c+0x78,gain);
    for(uint32_t o:{0x6Cu,0x70u,0x74u})write32(c+o,read32(c+o)*gain/oldGain);
}
// FO2_FMOD_WATCH=1: a one-shot the first time it is seen around a mixed block (where: 0 at Lock, 1 at Unlock), with its
// play position, FMOD's 3D gain (+0x78), mix volumes (+0x6C..) and the gain its distance calls for.
void fmodFirstLook(int where){
    static const bool on=std::getenv("FO2_FMOD_WATCH")!=nullptr;
    if(!on)return;
    const uint32_t state=read32(0x684570u);if(!state)return;
    const uint32_t channels=read32(state+0x14A8);const int count=int(read32(state+0x14EC)+read32(state+0x14D4));
    if(!channels||count<=0||count>1024)return;
    static std::vector<uint32_t> seen[2]={std::vector<uint32_t>(1024),std::vector<uint32_t>(1024)};
    static uint32_t budget=12000;
    const auto rf=[](uint32_t va){uint32_t raw=read32(va);float v;std::memcpy(&v,&raw,4);return v;};
    for(int i=0;i<count;++i){
        const uint32_t c=channels+uint32_t(i)*0x1DC,sample=read32(c+0x5C);
        if(sample==seen[where][i])continue;
        seen[where][i]=sample;
        if(!sample||(read32(sample+0x124)&2)||!budget)continue;
        --budget;
        const float dx=rf(c+0x44)-rf(0x6A31C8u),dy=rf(c+0x48)-rf(0x6A31CCu),dz=rf(c+0x4C)-rf(0x6A31D0u),dist=std::sqrt(dx*dx+dy*dy+dz*dz),mn=rf(c+0xBC);
        const float wanted=255.f*std::min(1.f,mn/std::max(dist,mn));
        std::printf("[FMODFIRST] %s %d %08x at %u gain %u out %u %u %u vol %u dist %.2f min %.2f wants %.0f flags %08x\n",where?"unlock":"lock",i,sample,read32(c+0x80),read32(c+0x78),
                    read32(c+0x6C),read32(c+0x70),read32(c+0x74),read32(c+0x1C),dist,mn,wanted,read32(c+0x3C));
    }
}
void fmodChannelWatch(){
    fmodFirstLook(1);
    // FO2_FMOD_ROLLOFF=<factor>: FMOD's 3D rolloff (0x68460C, read by 0x61D375). A sound stays where it was made
    // while the car drives away from it; a smaller factor keeps the debris sounds audible for longer.
    static const char* const rolloffEnv=std::getenv("FO2_FMOD_ROLLOFF");
    if(rolloffEnv){const float value=float(std::atof(rolloffEnv));uint32_t bits;std::memcpy(&bits,&value,4);write32(0x68460Cu,bits);}
    static const bool on=std::getenv("FO2_FMOD_WATCH")!=nullptr;
    if(!on)return;
    {static int shown=0;if(shown<20&&(++shown%5)==0){float d,f,r;uint32_t a=read32(0x684604u),b=read32(0x684608u),c=read32(0x68460Cu);std::memcpy(&d,&a,4);std::memcpy(&f,&b,4);std::memcpy(&r,&c,4);
        std::printf("[FMOD3D] doppler %g distance factor %g rolloff %g\n",d,f,r);}}
    const uint32_t state=read32(0x684570u);if(!state)return;
    const uint32_t channels=read32(state+0x14A8);const int count=int(read32(state+0x14EC)+read32(state+0x14D4));
    if(!channels||count<=0||count>1024)return;
    {   // the loops (engines), once a second: are they as loud as their distance calls for?
        static auto lastLoops=std::chrono::steady_clock::now();
        if(std::chrono::steady_clock::now()-lastLoops>=std::chrono::seconds(1)){
            lastLoops=std::chrono::steady_clock::now();
            const auto rf=[](uint32_t va){uint32_t raw=read32(va);float v;std::memcpy(&v,&raw,4);return v;};
            for(int i=0;i<count;++i){
                const uint32_t c=channels+uint32_t(i)*0x1DC,sample=read32(c+0x5C);
                if(!sample||!(read32(sample+0x124)&2))continue;
                const float dx=rf(c+0x44)-rf(0x6A31C8u),dy=rf(c+0x48)-rf(0x6A31CCu),dz=rf(c+0x4C)-rf(0x6A31D0u),dist=std::sqrt(dx*dx+dy*dy+dz*dz),mn=rf(c+0xBC);
                std::printf("[FMODLOOP] %d %08x at %u gain %u out %u %u %u vol %u dist %.2f min %.2f wants %.0f flags %08x %08x pos %.1f %.1f %.1f\n",i,sample,read32(c+0x80),read32(c+0x78),
                            read32(c+0x6C),read32(c+0x70),read32(c+0x74),read32(c+0x1C),dist,mn,255.f*std::min(1.f,mn/std::max(dist,mn)),read32(c+0x3C),read32(c+0x40),rf(c+0x44),rf(c+0x48),rf(c+0x4C));
            }
        }
    }
    struct Track{uint32_t sample=0;std::chrono::steady_clock::time_point start;};
    static std::vector<Track> tracks(1024);
    const auto now=std::chrono::steady_clock::now();
    for(int i=0;i<count;++i){
        const uint32_t c=channels+uint32_t(i)*0x1DC,sample=read32(c+0x5C);Track& t=tracks[i];
        // one-shot channels, block by block: the volumes FMOD derives (first 400 lines of each run of 2 s are enough)
        if(sample&&sample==t.sample&&!(read32(sample+0x124)&2)){
            static uint32_t budget=6000;
            if(budget){--budget;
                float gainF;uint32_t raw=read32(c+0x7C);std::memcpy(&gainF,&raw,4);
                std::printf("[FMODV] %d %.0f %08x vol %u pan %d out %u %u %u %u f %.3f x80 %08x x84 %08x x88 %08x x8c %08x\n",i,
                            std::chrono::duration<double,std::milli>(now-t.start).count(),sample,read32(c+0x1C),int32_t(read32(c+0x24)),
                            read32(c+0x6C),read32(c+0x70),read32(c+0x74),read32(c+0x78),gainF,read32(c+0x80),read32(c+0x84),read32(c+0x88),read32(c+0x8C));
            }
        }
        // the inputs of FMOD's 3D attenuation (0x61D375), block by block: channel position +0x44, its velocity +0x50,
        // min +0xBC, max +0xC0, listener position 0x6A31C8; +0x78 is the 3D gain out of 255, +0x7C the Doppler ratio
        if(sample&&sample==t.sample&&!(read32(sample+0x124)&2)){
            static uint32_t budget3d=40000;
            if(budget3d){--budget3d;
                const auto rf=[](uint32_t va){uint32_t raw=read32(va);float v;std::memcpy(&v,&raw,4);return v;};
                const float x=rf(c+0x44),y=rf(c+0x48),z=rf(c+0x4C),lx=rf(0x6A31C8u),ly=rf(0x6A31CCu),lz=rf(0x6A31D0u);
                const float dx=x-lx,dy=y-ly,dz=z-lz,dist=std::sqrt(dx*dx+dy*dy+dz*dz);
                std::printf("[FMOD3DCH] %d %.0f %08x flags %08x %08x pos %.1f %.1f %.1f vel %.1f %.1f %.1f listener %.1f %.1f %.1f dist %.2f min %.2f max %.1f gain %u doppler %.3f vol %u\n",
                            i,std::chrono::duration<double,std::milli>(now-t.start).count(),sample,read32(c+0x3C),read32(c+0x40),x,y,z,rf(c+0x50),rf(c+0x54),rf(c+0x58),lx,ly,lz,dist,rf(c+0xBC),rf(c+0xC0),read32(c+0x78),rf(c+0x7C),read32(c+0x1C));
            }
        }
        if(sample==t.sample)continue;
        if(sample){     // first time a sample plays: how its energy spreads over its length (10 slices)
            static std::set<uint32_t> seen;
            if(seen.insert(sample).second){
                const uint32_t frames=read32(sample+0x10C),mode=read32(sample+0x124),data=read32(sample+0x104);
                if(data&&frames&&frames<(1u<<24)&&(mode&0x30)==0x30){   // 16-bit mono
                    const int16_t* pcm=static_cast<const int16_t*>(ptr(data,frames*2));
                    std::printf("[FMODSAMPLE] %08x %u frames mode %08x rms:",sample,frames,mode);
                    for(int s=0;s<10;++s){
                        const uint32_t a=uint64_t(frames)*s/10,b=uint64_t(frames)*(s+1)/10;
                        double sum=0;for(uint32_t n=a;n<b;++n)sum+=double(pcm[n])*pcm[n];
                        std::printf(" %.0f",std::sqrt(sum/(b>a?b-a:1)));
                    }
                    std::printf("\n");
                }
            }
        }
        if(t.sample){   // a sound ended, or was replaced by another on this channel
            const uint32_t frames=read32(t.sample+0x10C),mode=read32(t.sample+0x124),freq=read32(c+0x20);
            std::printf("[FMODCH] END ch %d handle %08x played %.0f ms expected %.0f ms (%u frames at %u Hz) mode %08x %s\n",i,read32(c+0x14),
                        std::chrono::duration<double,std::milli>(now-t.start).count(),freq?1000.0*frames/freq:0.0,frames,freq,mode,sample?"REPLACED":"");
        }
        t.sample=sample;t.start=now;
    }
}
void soundDispatchBody();
void soundDispatch(){std::lock_guard<std::recursive_mutex> l(soundStateMutex);soundDispatchBody();}
void soundDispatchBody(){
    uint32_t kind=(soundToken-soundMethodBase)/0x1000,method=((soundToken-soundMethodBase)%0x1000)/16,self=arg(0);
    auto found=soundObjects.find(self);if(found==soundObjects.end()||found->second.kind!=kind)throw GuestStop("Invalid DirectSound object");
    auto& object=found->second;
    // AS2_SOUND_TRACE=1: every call in order, with its first arguments (the first 400 calls)
    static const bool soundTrace=std::getenv("AS2_SOUND_TRACE")!=nullptr;static unsigned traced=0;
    if(soundTrace&&traced<400){++traced;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","DS trace kind=%u method=%u self=%08x a1=%08x a2=%08x a3=%08x",kind,method,self,arg(1),arg(2),arg(3));}
    else if(++soundMethodCalls[soundToken]<=3)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","DirectSound kind=%u method=%u",kind,method);
    if(method==0){
        if(!arg(2)){ret(0x80004003,3);return;}write32(arg(2),0);
        const std::array<uint8_t,16> dsGuid={0x83,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
        const std::array<uint8_t,16> bufferGuid={0x85,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
        const std::array<uint8_t,16> unknown={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        if(!inputGuidEquals(arg(1),kind==1?dsGuid:bufferGuid)&&!inputGuidEquals(arg(1),unknown)){ret(0x80004002,3);return;}
        ++object.references;write32(arg(2),self);ret(0,3);return;
    }
    if(method==1){ret(++object.references,1);return;}
    if(method==2){uint32_t refs=--object.references;if(!refs){if(object.voice)soundMixer.remove(object.voice);if(object.guestSamples)heaps->free(GuestHeaps::process,object.guestSamples);heaps->free(GuestHeaps::process,object.table);heaps->free(GuestHeaps::process,self);soundObjects.erase(found);}ret(refs,1);return;}
    if(kind==1){switch(method){
        case 3:{uint32_t desc=arg(1),output=arg(2);if(!output){ret(0x80004003,4);return;}write32(output,0);
            if(!desc||(read32(desc)!=20&&read32(desc)!=36)||arg(3)){ret(0x80070057,4);return;}
            uint32_t flags=read32(desc+4),bytes=read32(desc+8),format=read32(desc+16);
            if(soundTrace)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","DS CreateSoundBuffer size=%u flags=0x%x bytes=%u format=%08x tag=%u ch=%u rate=%u bits=%u",
                read32(desc),flags,bytes,format,format?soundRead16(format):0,format?soundRead16(format+2):0,format?read32(format+4):0,format?soundRead16(format+14):0);
            auto voice=std::make_shared<PcmVoice>();
            if(!(flags&1)&&(!soundFormat(format,*voice)||!bytes||bytes>16*1024*1024||bytes%voice->block())){ret(soundBadFormat,4);return;}
            // 3D spatialization is not implemented. FMOD 3.75 (FlatOut 2) asks for the primary buffer with
            // CTRL3D (0x11) and gives up the whole output when refused: on the primary it only asks for the
            // listener, so it is accepted there (QueryInterface for the listener still fails).
            if((flags&0x10)&&!(flags&1)){ret(0x8878001e,4);return;}
            if(!(flags&1)){voice->samples.resize(bytes,voice->bits==8?128:0);if(!soundMixer.add(voice)){ret(0x8878000a,4);return;}}
            uint32_t buffer=createSoundObject(2,flags);auto& created=soundObjects.at(buffer);created.voice=voice;
            if(!(flags&1)){created.guestSamples=allocate(bytes);std::memcpy(ptr(created.guestSamples,bytes),voice->samples.data(),bytes);}
            AUDIO_LOG("CreateSoundBuffer #%u: flags=0x%x bytes=%u %u Hz %u ch %u bit%s",buffer,flags,bytes,voice->rate,voice->channels,voice->bits,(flags&1)?" (primary)":"");
            write32(output,buffer);ret(0,4);return;}
        case 5:{  // DuplicateSoundBuffer(original, duplicate*): Alien Shooter 2 plays one sound several times at once.
            // The copy has the original's format, data, volume and pan, and its own play position and state.
            uint32_t original=arg(1),output=arg(2);if(!output){ret(0x80004003,3);return;}write32(output,0);
            auto source=soundObjects.find(original);
            if(source==soundObjects.end()||source->second.kind!=2||!source->second.voice||(source->second.flags&1)){ret(0x80070057,3);return;}
            const SoundObject& from=source->second;
            auto voice=std::make_shared<PcmVoice>(*from.voice);voice->playing=false;voice->position=0;
            if(!soundMixer.add(voice)){ret(0x8878000a,3);return;}
            const uint32_t bytes=static_cast<uint32_t>(voice->samples.size());
            uint32_t buffer=createSoundObject(2,from.flags);auto& created=soundObjects.at(buffer);
            created.voice=voice;created.volume=from.volume;created.pan=from.pan;
            created.guestSamples=allocate(bytes);std::memcpy(ptr(created.guestSamples,bytes),voice->samples.data(),bytes);
            write32(output,buffer);ret(0,3);return;}
        case 4:{uint32_t out=arg(1);if(!out||read32(out)!=96){ret(0x80070057,2);return;}std::memset(ptr(out+4,92),0,92);
            // Match the PCM formats and continuous resampling we implement. The old
            // 0x147 omitted PRIMARY16BIT, SECONDARY16BIT and SECONDARYSTEREO.
            // A Windows DirectSound probe reports 0xF1F for the same formats.
            write32(out+4,0xf1f);write32(out+8,100);write32(out+12,200000);write32(out+16,1);ret(0,2);return;}
        case 6:if(!windows.count(arg(1))||arg(2)<1||arg(2)>3){ret(0x80070057,3);return;}ret(0,3);return;
        case 8:if(!arg(1)){ret(0x80004003,2);return;}write32(arg(1),4);ret(0,2);return; // Stereo speaker configuration.
        default:throw GuestStop("DirectSound interface method pending: "+std::to_string(method));
    }}
    auto voice=object.voice;if(!voice)throw GuestStop("DirectSound buffer missing PCM voice");
    switch(method){
    case 3:{uint32_t out=arg(1);if(!out||read32(out)!=20){ret(0x80070057,2);return;}write32(out+4,(object.flags&~4u)|8u);write32(out+8,uint32_t(voice->samples.size()));write32(out+12,0);write32(out+16,0);ret(0,2);return;}
    case 4:{std::lock_guard<std::mutex> lock(soundMixer.mutex);uint32_t bytes=uint32_t(voice->samples.size()),position=uint32_t(voice->position)*voice->block();
        if(arg(1))write32(arg(1),position);if(arg(2))write32(arg(2),bytes?(position+soundMixer.rate/100*voice->block())%bytes:0);ret(0,3);return;}
    case 6:case 7:case 8:case 9:{if(!arg(1)){ret(0x80004003,2);return;}
        std::lock_guard<std::mutex> lock(soundMixer.mutex);write32(arg(1),method==6?object.volume:method==7?object.pan:method==8?voice->frequency:(voice->playing?1u|(voice->loop?4u:0u):0u));ret(0,2);return;}
    case 11:{uint32_t bytes=uint32_t(voice->samples.size()),offset=arg(1),length=arg(2),flags=arg(7);
        if(!bytes||object.locked||flags&~3u||!arg(3)||!arg(4)){ret(0x80070057,8);return;}
        if(flags&1){std::lock_guard<std::mutex> lock(soundMixer.mutex);offset=(uint32_t(voice->position)*voice->block()+soundMixer.rate/100*voice->block())%bytes;}
        if(flags&2){offset=0;length=bytes;}
        if(offset>=bytes||length>bytes){ret(0x80070057,8);return;}uint32_t first=std::min(length,bytes-offset),second=length-first;
        if(second&&(!arg(5)||!arg(6))){ret(0x80070057,8);return;}
        fmodFirstLook(0);
        object.locked=true;object.lockOffset=offset;object.lockBytes=length;
        write32(arg(3),object.guestSamples+offset);write32(arg(4),first);if(arg(5))write32(arg(5),second?object.guestSamples:0);if(arg(6))write32(arg(6),second);ret(0,8);return;}
    case 12:{if(arg(1)||arg(2)||arg(3)&~1u){ret(0x80070057,4);return;}std::lock_guard<std::mutex> lock(soundMixer.mutex);voice->loop=arg(3)&1;voice->playing=true;
        unsigned active=0;for(auto& slot:soundMixer.voices)if(slot&&slot->playing)++active;
        AUDIO_LOG("Play #%u: bytes=%u loop=%u volume=%d pan=%d frequency=%u, %u voices playing",self,unsigned(voice->samples.size()),unsigned(voice->loop),int(object.volume),int(object.pan),voice->frequency,active);
        ret(0,4);return;}
    case 13:{if(arg(1)>=voice->samples.size()||arg(1)%voice->block()){ret(0x80070057,2);return;}std::lock_guard<std::mutex> lock(soundMixer.mutex);voice->position=arg(1)/voice->block();ret(0,2);return;}
    case 14:{if(!(object.flags&1)){ret(0x80070057,2);return;}std::lock_guard<std::mutex> lock(soundMixer.mutex);ret(soundFormat(arg(1),*voice)?0:soundBadFormat,2);return;}
    case 15:case 16:{int32_t value=int32_t(arg(1));if(value < -10000||value>(method==15?0:10000)){ret(0x80070057,2);return;}
        std::lock_guard<std::mutex> lock(soundMixer.mutex);if(method==15)object.volume=arg(1);else object.pan=arg(1);
        float gain=std::pow(10.f,int32_t(object.volume)/2000.f),pan=int32_t(object.pan);
        voice->left=gain*(pan>0?std::pow(10.f,-pan/2000.f):1);voice->right=gain*(pan<0?std::pow(10.f,pan/2000.f):1);
        uint64_t key=(uint64_t(self)<<1)|(method==16?1u:0u);
        auto last=audioLastValue.find(key);
        if(last==audioLastValue.end()||last->second!=value){
            audioLastValue[key]=value;
            if(method==15)AUDIO_LOG("SetVolume #%u: %d (%.1f dB, gain %.3f) bytes=%u playing=%u",self,value,value/100.0,double(gain),unsigned(voice->samples.size()),unsigned(voice->playing));
            else AUDIO_LOG("SetPan #%u: %d bytes=%u playing=%u",self,value,unsigned(voice->samples.size()),unsigned(voice->playing));
        }
        ret(0,2);return;}
    case 17:{uint32_t frequency=arg(1)?arg(1):voice->rate;if(frequency<100||frequency>200000){ret(0x80070057,2);return;}std::lock_guard<std::mutex> lock(soundMixer.mutex);voice->frequency=frequency;ret(0,2);return;}
    case 18:{std::lock_guard<std::mutex> lock(soundMixer.mutex);voice->playing=false;AUDIO_LOG("Stop #%u: bytes=%u",self,unsigned(voice->samples.size()));ret(0,1);return;}
    case 19:{uint32_t bytes=uint32_t(voice->samples.size()),first=std::min(object.lockBytes,bytes-object.lockOffset),second=object.lockBytes-first;
        if(!object.locked||arg(1)!=object.guestSamples+object.lockOffset||arg(2)!=first||arg(3)!=(second?object.guestSamples:0)||arg(4)!=second){ret(0x80070057,5);return;}
        // Publish only what the guest locked. Copying the whole ring under the
        // mixer lock made every Unlock stall the AAudio callback and could
        // overwrite samples still being played with a stale copy.
        {std::lock_guard<std::mutex> lock(soundMixer.mutex);
            std::memcpy(voice->samples.data()+object.lockOffset,ptr(object.guestSamples+object.lockOffset,first),first);
            if(second)std::memcpy(voice->samples.data(),ptr(object.guestSamples,second),second);
            if(first<bytes||voice->playing){voice->markWritten(object.lockOffset,first);voice->markWritten(0,second);}}
        auto published=std::chrono::steady_clock::now();
        if(object.lastPublish!=std::chrono::steady_clock::time_point{})object.maxPublishGap=std::max(object.maxPublishGap,
            std::chrono::duration_cast<std::chrono::microseconds>(published-object.lastPublish));
        object.lastPublish=published;
        fmodChannelWatch();
        object.locked=false;ret(0,5);return;}
    case 20:ret(0,1);return; // Software-owned buffers cannot be lost by the driver.
    default:throw GuestStop("DirectSound buffer method pending: "+std::to_string(method));
    }
}
