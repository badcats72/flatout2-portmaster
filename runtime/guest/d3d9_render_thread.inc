// R36S: D3D9 render thread (included by d3d9_bridge.cpp after dispatchGuestD3D9Inner).
//
// The game issues ~30k D3D9 calls per race frame from its one main thread. Executing them (D3D9->GLES
// translation and the Mali driver's CPU work) used to happen on that same thread, so translation and
// driver time added straight onto the game's frame time on one core of the R36S's four. Here every
// D3D9 call executes on a dedicated render thread, which owns the GL context:
//  - "deferred" calls (state setters, shader constants, draws, Present, texture AddRef/Release, state
//    block Capture/Apply) are queued and return D3D_OK at once. Arguments are resolved to native
//    objects and any data they point at (matrices, constants, user-pointer vertices...) is copied
//    into a per-frame arena, because the guest may reuse that memory as soon as the call returns.
//  - every other call is synchronous: the queue drains, the call runs on the render thread through the
//    normal dispatcher, and the guest waits for its result.
// The guest builds frame N+1 while the render thread executes frame N (at most one frame in flight:
// Present N waits for Present N-1). Only the render thread touches GL, the native D3D9 objects and,
// apart from enqueue-time lookups made while it executes only deferred commands, the bridge's maps.
// NFSU2_RENDER_THREAD=0 disables it.
#if defined(NFS_R36S)
#include <condition_variable>
#include <deque>
#include <mutex>
#include <exception>
#include <functional>
#include <pthread.h>
extern "C" void d3d9NoteGameThread();

namespace rt {
enum Op:uint16_t {OpDevice,OpAddRef,OpRelease,OpStateBlock,OpSync,OpFunction,OpRefDelta,OpBufferUpdate,OpTexUpload,OpInstancedDraw,OpSurfaceUpload};
struct SyncSlot {uint32_t token=0;const uint32_t* args=nullptr;uint32_t count=0;uint32_t result=0;std::exception_ptr error;std::function<void()> fn;std::atomic<bool> done{false};};
struct Cmd {uint16_t op;uint16_t method;uint32_t a[9];const void* p0;const void* p1;void* obj;void* obj2;SyncSlot* sync;};

// Ring of queued calls (AS2_QUEUE_BITS, default 16: 65536 calls). A ring small enough for the L2 (12 bits)
// was measured in Alien Shooter 2: the cache misses on the stores went, but the game thread then waited on a
// full ring (sched_yield) for more than it saved.
const uint32_t queueSize=[]{const char* e=std::getenv("AS2_QUEUE_BITS");int b=e?std::atoi(e):16;return 1u<<(b<10?10:b>16?16:b);}();
Cmd* queue=nullptr;
std::atomic<uint32_t> head{0},tail{0};
std::atomic<bool> running{false},sleeping{false};
std::mutex wakeMutex;std::condition_variable wakeCv;     // producer -> render thread
std::mutex doneMutex;std::condition_variable doneCv;     // render thread -> producer (sync results, Present)
std::atomic<uint64_t> presentsQueued{0},presentsDone{0};
std::thread worker;
bool enabled=false;

struct Arena {std::vector<uint8_t> buf;size_t used=0;};
Arena arenas[2];
int arenaIndex=0;

// Redundant-call filter on the game thread. The game re-sets the same render/sampler/stage states,
// shaders and textures constantly (~18k state calls per race frame); a shadow of what was last queued
// drops repeats before they cost a queue slot and render-thread work. The shadow is forgotten at every
// synchronous call (state blocks, Reset, ...) and every state-block Apply, which change device state
// behind it; handles also carry handlesRetiredEpoch so a recreated object at the same handle is never skipped.
struct Shadow {
    uint32_t rs[256];bool rsValid[256];
    uint32_t ss[16][16];bool ssValid[16][16];
    uint32_t tss[8][33];bool tssValid[8][33];
    uint32_t tex[16];uint64_t texEpoch[16];bool texValid[16];
    uint32_t vs,ps,decl,fvf,ib;uint64_t vsE,psE,declE,ibE;bool vsValid,psValid,declValid,fvfValid,ibValid;
    uint32_t sb[4][3];uint64_t sbE[4];bool sbValid[4];
    float vsc[256][4];bool vscValid[256];
    float psc[32][4];bool pscValid[32];
};
Shadow* shadow=nullptr;
// Only the validity flags are cleared (~1KB): this runs at every synchronous call (buffer locks, ...).
void forgetShadow(){
    if(!shadow){shadow=new Shadow;std::memset(shadow,0,sizeof(Shadow));return;}
    Shadow& s=*shadow;
    std::memset(s.rsValid,0,sizeof s.rsValid);std::memset(s.ssValid,0,sizeof s.ssValid);std::memset(s.tssValid,0,sizeof s.tssValid);
    std::memset(s.texValid,0,sizeof s.texValid);std::memset(s.sbValid,0,sizeof s.sbValid);
    std::memset(s.vscValid,0,sizeof s.vscValid);std::memset(s.pscValid,0,sizeof s.pscValid);
    s.vsValid=s.psValid=s.declValid=s.fvfValid=s.ibValid=false;
}
uint64_t filtered=0;
// GetViewport (Alien Shooter 2: ~26 a frame) answered from the last viewport set or read, without a synchronous
// call. Unknown after SetRenderTarget (which resets it), a state-block Apply and any other synchronous call.
D3DVIEWPORT9 viewportShadow{};bool viewportKnown=false;
std::unordered_map<uint32_t,IUnknown*> plainSurfaces;   // CreateOffscreenPlainSurface handle -> native (surfaceLockAsync)
// State blocks and the viewport: which blocks hold one (CreateStateBlock(ALL), or a SetViewport recorded
// between Begin/EndStateBlock) and the value their Capture took, so Apply keeps the shadow known.
struct BlockViewport {bool has=false,known=false;D3DVIEWPORT9 value{};};
std::unordered_map<uint32_t,BlockViewport> blockViewports;
bool recordingBlock=false;BlockViewport recorded;
// Reference counts: the game AddRefs/Releases textures thousands of times per frame in pairs. Only the
// net change per object reaches the render thread, at the next synchronous call or Present (a final
// Release is always synchronous, so no object can be destroyed while queued commands use it).
std::vector<Object*> refDirty;  // objects with a pending refDelta (Object nodes are stable; erasure is synchronous)
void flushRefDeltas();

void drain();
const void* copyData(const void* src,size_t bytes){
    if(!src||!bytes)return src;
    Arena& a=arenas[arenaIndex];
    if(a.buf.empty())a.buf.resize(8u<<20);  // never used: nothing points into it yet
    size_t at=(a.used+15)&~size_t(15);
    // Full: let the render thread consume everything queued (both arenas), then reuse/grow this one.
    if(at+bytes>a.buf.size()){drain();at=0;a.used=0;if(bytes>a.buf.size())a.buf.resize(bytes*2);}
    std::memcpy(a.buf.data()+at,src,bytes);a.used=at+bytes;return a.buf.data()+at;
}

// Sync calls and Present are notified (the render thread sets the flag, then takes doneMutex and notifies),
// so their waits block for real (the 50 ms timeout is only a safety net); waking every millisecond cost
// the game thread ~3% of its CPU in futex calls while it waited for Present. drain() is not notified: polls.
extern "C" void nfsWaitAdd(int kind,uint64_t ns);
extern "C" void nfsEmulatePace();
extern "C" void nfsSetPaceOnGameThread(bool on);
void waitDone(const std::function<bool()>& ready,int pollMs=50,int waitKind=-1){
    if(ready())return;
    const auto t0=std::chrono::steady_clock::now();
    for(int i=0;i<64&&!ready();++i)std::this_thread::yield();
    if(!ready()){
        std::unique_lock<std::mutex> l(doneMutex);
        while(!ready())doneCv.wait_for(l,std::chrono::milliseconds(pollMs));
    }
    if(waitKind>=0)nfsWaitAdd(waitKind,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
}
void wake(){if(sleeping.load(std::memory_order_acquire)){std::lock_guard<std::mutex> l(wakeMutex);wakeCv.notify_one();}}
// Wake-ups are batched: one per 256 queued calls, or at once for calls the guest waits on (Present,
// synchronous calls, drains). Waking the render thread for every call cost a futex syscall per call
// (~1/3 of the game thread's CPU); a sleeping render thread also re-checks every millisecond.
void push(const Cmd& c,bool urgent=false){
    uint32_t h=head.load(std::memory_order_relaxed);
    while(h-tail.load(std::memory_order_acquire)>=queueSize){wake();std::this_thread::yield();}
    queue[h&(queueSize-1)]=c;head.store(h+1,std::memory_order_release);
    if(urgent||((h+1)&255u)==0)wake();
}
void flushRefDeltas(){
    if(refDirty.empty())return;
    for(Object* o:refDirty){o->refQueued=false;if(!o->refDelta)continue;Cmd c{};c.op=OpRefDelta;c.obj=o->native;c.a[0]=static_cast<uint32_t>(o->refDelta);push(c);o->refDelta=0;}
    refDirty.clear();
}
void drain(){
    uint32_t target=head.load(std::memory_order_relaxed);
    wake();
    waitDone([&]{return tail.load(std::memory_order_acquire)==target;},1);
}
// ---- DXT decoding on worker threads ----
// The GPU samples no DXT format here, so every DXT level is decoded to RGBA on the CPU before it is uploaded. That
// ran on the render thread, one level after another (~4900 levels at the race load; ~0.6 ms per upload on the
// device, decode included). Levels are now decoded by NFSU2_DECODE_THREADS workers (default 2; 0 = on the render
// thread as before) from the moment the game unlocks them. When the upload command comes up, the render thread takes
// the finished pixels, decodes the level itself if no worker has started it, or else waits for that worker.
struct DecodeJob {
    uint32_t mode,width,height;const uint8_t* raw;size_t size;  // raw: the level's bytes in the frame arena
    std::vector<uint8_t> pixels;   // RGBA8
    std::vector<uint8_t> rgb565;   // the same pixels as RGB565 when every texel is opaque, else empty
    bool done=false;bool started=false;  // under DecodePool::mutex
};
class DecodePool {
    std::mutex mutex;std::condition_variable workAvailable,jobFinished;
    std::deque<DecodeJob*> queued;std::vector<std::thread> workers;
    static void decode(DecodeJob& job){
        job.pixels=decodeDXT(job.mode,job.width,job.height,job.raw,job.size);
        // what the upload decides on the render thread: an opaque first level stores the texture as RGB565
        const size_t texels=size_t(job.width)*job.height;bool opaque=job.pixels.size()>=texels*4;
        for(size_t i=0;opaque&&i<texels;++i)opaque=job.pixels[i*4+3]==255;
        if(!opaque)return;
        job.rgb565.resize(texels*2);uint16_t* out=reinterpret_cast<uint16_t*>(job.rgb565.data());
        for(size_t i=0;i<texels;++i){const uint8_t* c=&job.pixels[i*4];out[i]=static_cast<uint16_t>(((c[0]>>3)<<11)|((c[1]>>2)<<5)|(c[2]>>3));}
    }
    void work(){
        pthread_setname_np(pthread_self(),"dxt-decode");
        std::unique_lock<std::mutex> lock(mutex);
        for(;;){
            workAvailable.wait(lock,[&]{return !queued.empty();});
            DecodeJob* job=queued.front();queued.pop_front();job->started=true;
            lock.unlock();decode(*job);lock.lock();
            job->done=true;jobFinished.notify_all();
        }
    }
public:
    bool enabled(){
        static const unsigned threads=[]{const char* e=std::getenv("NFSU2_DECODE_THREADS");return e?unsigned(std::atoi(e)):2u;}();
        if(threads&&workers.empty()){for(unsigned i=0;i<threads&&i<4;++i)workers.emplace_back([this]{work();});
            for(auto& w:workers)w.detach();}
        return threads!=0;
    }
    void submit(DecodeJob* job){{std::lock_guard<std::mutex> lock(mutex);queued.push_back(job);}workAvailable.notify_one();}
    // Waits until a submitted job is decoded (render thread).
    void finish(DecodeJob* job){
        std::unique_lock<std::mutex> lock(mutex);
        if(!job->started){  // still queued: decode it here rather than wait behind other levels
            queued.erase(std::find(queued.begin(),queued.end(),job));job->started=true;
            lock.unlock();decode(*job);lock.lock();job->done=true;
        }
        jobFinished.wait(lock,[&]{return job->done;});
    }
};
DecodePool decodePool;
std::vector<uint8_t> instancePayload;   // nfsD3dDrawInstanced scratch (game thread)
uint64_t statPushed=0,statSyncs=0;
std::unordered_map<uint32_t,uint32_t> syncCounts;  // per token, logged every 600 frames
struct HudRec;void hudRecordInvalidate();extern bool hudRecording;
extern bool capActive,capBad;
extern int passMode;extern bool passConflict;
void dropSurfaceShadows(uint32_t parent);
uint32_t syncCall(uint32_t token,const uint32_t* args,uint32_t* count){
    if(hudRecording)hudRecordInvalidate();
    if(passMode==2)passConflict=true;
    if(capActive)capBad=true;
    flushRefDeltas();forgetShadow();++statSyncs;
    SyncSlot slot;slot.token=token;slot.args=args;
    Cmd c{};c.op=OpSync;c.sync=&slot;push(c,true);
    nfsWaitAdd(5,1);syncCounts[token]++;
    const uint32_t syncKind=(token-methodBase)/4096,syncMethod=((token-methodBase)%4096)/16;
    if(syncKind==5&&syncMethod==2)plainSurfaces.erase(args[0]);
    if((syncKind==2&&syncMethod==16)||(syncKind==11&&syncMethod==5))viewportKnown=false;   // Reset, an Apply not queued
    // the calls that write pixels behind the surface copies: Reset, UpdateSurface ... ColorFill, texture locks
    if((syncKind==2&&(syncMethod==16||(syncMethod>=30&&syncMethod<=35)))||(syncKind==3&&syncMethod==19))dropSurfaceShadows(syncKind==3?args[0]:0);
    waitDone([&]{return slot.done.load(std::memory_order_acquire);},50,1);
    if(slot.error)std::rethrow_exception(slot.error);
    if(token==methodBase+2*4096+48*16&&slot.result==0&&args[1]){std::memcpy(&viewportShadow,pointer(args[1]),sizeof(D3DVIEWPORT9));viewportKnown=true;}
    if(syncKind==2&&slot.result==0){
        if(syncMethod==59&&args[2])   // created with the current state captured
            blockViewports[*static_cast<const uint32_t*>(pointer(args[2]))]=BlockViewport{args[1]==D3DSBT_ALL,viewportKnown,viewportShadow};
        if(syncMethod==60){recordingBlock=true;recorded=BlockViewport{};}
        if(syncMethod==61&&args[1]){recordingBlock=false;blockViewports[*static_cast<const uint32_t*>(pointer(args[1]))]=recorded;}
    }
    if(syncKind==2&&(syncMethod==37||syncMethod==47))viewportKnown=false;   // set through this path: read it back
    *count=slot.count;return slot.result;
}
void runOnRenderThread(std::function<void()> fn){
    if(hudRecording)hudRecordInvalidate();
    if(!enabled){fn();return;}
    flushRefDeltas();forgetShadow();
    SyncSlot slot;slot.fn=std::move(fn);
    Cmd c{};c.op=OpFunction;c.sync=&slot;push(c,true);
    waitDone([&]{return slot.done.load(std::memory_order_acquire);});
    if(slot.error)std::rethrow_exception(slot.error);
}

UINT vertexCount(D3DPRIMITIVETYPE t,UINT n){
    switch(t){case D3DPT_POINTLIST:return n;case D3DPT_LINELIST:return n*2;case D3DPT_LINESTRIP:return n+1;
        case D3DPT_TRIANGLELIST:return n*3;case D3DPT_TRIANGLESTRIP:case D3DPT_TRIANGLEFAN:return n+2;default:return 0;}
}
float f32(uint32_t bits){float v;std::memcpy(&v,&bits,4);return v;}

void execDevice(const Cmd& c){
    auto d=static_cast<IDirect3DDevice9*>(c.obj);const uint32_t* a=c.a;
    switch(c.method){
    case 17:d->Present(static_cast<const RECT*>(c.p0),static_cast<const RECT*>(c.p1),reinterpret_cast<HWND>(c.obj2),nullptr);
        presentsDone.fetch_add(1,std::memory_order_release);{std::lock_guard<std::mutex> l(doneMutex);}doneCv.notify_all();break;
    case 30:{POINT p{LONG(int32_t(a[6])),LONG(int32_t(a[7]))};   // UpdateSurface (tryDeferImpl): obj2 source, p1 destination
        d->UpdateSurface(static_cast<IDirect3DSurface9*>(c.obj2),static_cast<const RECT*>(c.p0),static_cast<IDirect3DSurface9*>(const_cast<void*>(c.p1)),a[8]?&p:nullptr);break;}
    case 32:d->GetRenderTargetData(static_cast<IDirect3DSurface9*>(c.obj2),static_cast<IDirect3DSurface9*>(const_cast<void*>(c.p1)));break;
    case 34:{const RECT* r=static_cast<const RECT*>(c.p0);   // StretchRect (tryDeferImpl): obj2 source, p1 destination
        d->StretchRect(static_cast<IDirect3DSurface9*>(c.obj2),(a[6]&1)?&r[0]:nullptr,static_cast<IDirect3DSurface9*>(const_cast<void*>(c.p1)),(a[6]&2)?&r[1]:nullptr,static_cast<D3DTEXTUREFILTERTYPE>(a[5]));break;}
    case 37:d->SetRenderTarget(a[1],static_cast<IDirect3DSurface9*>(c.obj2));break;
    case 39:d->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(c.obj2));break;
    case 41:d->BeginScene();break;
    case 42:d->EndScene();break;
    case 43:d->Clear(a[1],static_cast<const D3DRECT*>(c.p0),a[3],a[4],f32(a[5]),a[6]);break;
    case 44:d->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(a[1]),static_cast<const D3DMATRIX*>(c.p0));break;
    case 46:d->MultiplyTransform(static_cast<D3DTRANSFORMSTATETYPE>(a[1]),static_cast<const D3DMATRIX*>(c.p0));break;
    case 47:d->SetViewport(static_cast<const D3DVIEWPORT9*>(c.p0));break;
    case 49:d->SetMaterial(static_cast<const D3DMATERIAL9*>(c.p0));break;
    case 51:d->SetLight(a[1],static_cast<const D3DLIGHT9*>(c.p0));break;
    case 53:d->LightEnable(a[1],a[2]);break;
    case 55:d->SetClipPlane(a[1],static_cast<const float*>(c.p0));break;
    case 57:d->SetRenderState(static_cast<D3DRENDERSTATETYPE>(a[1]),a[2]);break;
    case 65:d->SetTexture(a[1],static_cast<IDirect3DBaseTexture9*>(c.obj2));break;
    case 67:d->SetTextureStageState(a[1],static_cast<D3DTEXTURESTAGESTATETYPE>(a[2]),a[3]);break;
    case 69:d->SetSamplerState(a[1],static_cast<D3DSAMPLERSTATETYPE>(a[2]),a[3]);break;
    case 75:d->SetScissorRect(static_cast<const RECT*>(c.p0));break;
    case 77:d->SetSoftwareVertexProcessing(a[1]);break;
    case 81:d->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(a[1]),a[2],a[3]);break;
    case 82:d->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(a[1]),static_cast<INT>(a[2]),a[3],a[4],a[5],a[6]);break;
    case 83:d->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(a[1]),a[2],c.p0,a[4]);break;
    case 84:d->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(a[1]),a[2],a[3],a[4],c.p0,static_cast<D3DFORMAT>(a[6]),c.p1,a[8]);break;
    case 87:d->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9*>(c.obj2));break;
    case 89:d->SetFVF(a[1]);break;
    case 92:d->SetVertexShader(static_cast<IDirect3DVertexShader9*>(c.obj2));break;
    case 94:d->SetVertexShaderConstantF(a[1],static_cast<const float*>(c.p0),a[3]);break;
    case 96:d->SetVertexShaderConstantI(a[1],static_cast<const int*>(c.p0),a[3]);break;
    case 98:d->SetVertexShaderConstantB(a[1],static_cast<const BOOL*>(c.p0),a[3]);break;
    case 100:d->SetStreamSource(a[1],static_cast<IDirect3DVertexBuffer9*>(c.obj2),a[3],a[4]);break;
    case 102:d->SetStreamSourceFreq(a[1],a[2]);break;
    case 104:d->SetIndices(static_cast<IDirect3DIndexBuffer9*>(c.obj2));break;
    case 107:d->SetPixelShader(static_cast<IDirect3DPixelShader9*>(c.obj2));break;
    case 109:d->SetPixelShaderConstantF(a[1],static_cast<const float*>(c.p0),a[3]);break;
    case 111:d->SetPixelShaderConstantI(a[1],static_cast<const int*>(c.p0),a[3]);break;
    case 113:d->SetPixelShaderConstantB(a[1],static_cast<const BOOL*>(c.p0),a[3]);break;
    }
}
// OpInstancedDraw payload: the register numbers (uint16), then the per-instance data from a 16-byte boundary.
inline size_t instanceDataOffset(uint32_t registerCount){return (size_t(registerCount)*2+15)&~size_t(15);}
void exec(const Cmd& c){
    switch(c.op){
    case OpDevice:execDevice(c);break;
    case OpAddRef:static_cast<IUnknown*>(c.obj)->AddRef();break;
    case OpRefDelta:{int32_t d=static_cast<int32_t>(c.a[0]);auto o=static_cast<IUnknown*>(c.obj);for(;d>0;--d)o->AddRef();for(;d<0;++d)o->Release();break;}
    case OpRelease:static_cast<IUnknown*>(c.obj)->Release();break;
    case OpBufferUpdate:{  // a[0] offset, a[1] size, a[2] lock flags, a[3] 1 = index buffer; p0 the bytes
        void* dst=nullptr;HRESULT hr=c.a[3]?static_cast<IDirect3DIndexBuffer9*>(c.obj)->Lock(c.a[0],c.a[1],&dst,c.a[2]&~D3DLOCK_READONLY)
                                           :static_cast<IDirect3DVertexBuffer9*>(c.obj)->Lock(c.a[0],c.a[1],&dst,c.a[2]&~D3DLOCK_READONLY);
        if(SUCCEEDED(hr)&&dst){std::memcpy(dst,c.p0,c.a[1]);
            if(c.a[3])static_cast<IDirect3DIndexBuffer9*>(c.obj)->Unlock();else static_cast<IDirect3DVertexBuffer9*>(c.obj)->Unlock();}
        break;}
    case OpTexUpload:{  // a[0] level, a[1] size, a[2] pitch, a[3] rows, a[4] row bytes; p0 the bytes; obj2 the DXT shadow
        auto tex=static_cast<IDirect3DTexture9*>(c.obj);const uint32_t level=c.a[0];D3DLOCKED_RECT lr{};
        if(c.obj2){
            auto& t=*static_cast<CompressedTexture*>(c.obj2);auto& raw=t.bytes(0,level);
            std::memcpy(raw.data(),c.p0,std::min<size_t>(raw.size(),c.a[1]));
            const uint32_t w=t.mipWidth(level),h=t.mipHeight(level);
            std::vector<uint8_t> pixels;
            if(c.p1){  // decoded by a worker: uploaded as it is, or through LockRect/UnlockRect when the texture needs that
                std::unique_ptr<DecodeJob> job(static_cast<DecodeJob*>(const_cast<void*>(c.p1)));decodePool.finish(job.get());
                if(job->pixels.size()==size_t(w)*h*4&&glesUploadDecodedLevel(tex,level,job->pixels.data(),job->rgb565.empty()?nullptr:job->rgb565.data()))break;
                pixels=std::move(job->pixels);
            }
            else pixels=decodeDXT(t.mode(),w,h,raw.data(),raw.size());
            if(SUCCEEDED(tex->LockRect(level,&lr,nullptr,0))&&lr.pBits){
                for(uint32_t row=0;row<h;++row)std::memcpy(static_cast<uint8_t*>(lr.pBits)+size_t(row)*lr.Pitch,pixels.data()+size_t(row)*w*4,size_t(w)*4);
                tex->UnlockRect(level);
            }
        }else if(SUCCEEDED(tex->LockRect(level,&lr,nullptr,0))&&lr.pBits){
            for(uint32_t row=0;row<c.a[3];++row)std::memcpy(static_cast<uint8_t*>(lr.pBits)+size_t(row)*lr.Pitch,static_cast<const uint8_t*>(c.p0)+size_t(row)*c.a[2],c.a[4]);
            tex->UnlockRect(level);
        }
        break;}
    case OpSurfaceUpload:{  // a[0] pitch, a[1] rows, a[2] row bytes; p0 the whole surface (surfaceLockAsync)
        auto surface=static_cast<IDirect3DSurface9*>(c.obj);D3DLOCKED_RECT lr{};
        if(SUCCEEDED(surface->LockRect(&lr,nullptr,D3DLOCK_DISCARD))&&lr.pBits){
            for(uint32_t row=0;row<c.a[1];++row)std::memcpy(static_cast<uint8_t*>(lr.pBits)+size_t(row)*lr.Pitch,static_cast<const uint8_t*>(c.p0)+size_t(row)*c.a[0],c.a[2]);
            surface->UnlockRect();
        }
        break;}
    case OpStateBlock:if(c.method==4)static_cast<IDirect3DStateBlock9*>(c.obj)->Capture();else static_cast<IDirect3DStateBlock9*>(c.obj)->Apply();break;
    case OpInstancedDraw:{  // a[0..5] DrawIndexedPrimitive arguments, a[6] registers per instance, a[7] instances; p0 registers then data
        auto device=static_cast<IDirect3DDevice9*>(c.obj);const uint32_t count=c.a[6],instances=c.a[7];
        const auto registers=static_cast<const uint16_t*>(c.p0);
        const auto data=reinterpret_cast<const float*>(static_cast<const uint8_t*>(c.p0)+instanceDataOffset(count));
        if(glesDrawIndexedInstanced(device,c.a,registers,count,instances,data))break;
        for(uint32_t i=0;i<instances;++i){  // no instanced variant: the same draws one by one
            for(uint32_t j=0;j<count;++j)device->SetVertexShaderConstantF(registers[j],data+(size_t(i)*count+j)*4,1);
            device->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(c.a[0]),static_cast<INT>(c.a[1]),c.a[2],c.a[3],c.a[4],c.a[5]);
        }
        break;}
    case OpSync:{SyncSlot& s=*c.sync;
        try{s.result=dispatchGuestD3D9Inner(s.token,s.args,&s.count);}catch(...){s.error=std::current_exception();}
        s.done.store(true,std::memory_order_release);{std::lock_guard<std::mutex> l(doneMutex);}doneCv.notify_all();break;}
    case OpFunction:{SyncSlot& s=*c.sync;
        try{s.fn();}catch(...){s.error=std::current_exception();}
        s.done.store(true,std::memory_order_release);{std::lock_guard<std::mutex> l(doneMutex);}doneCv.notify_all();break;}
    }
}
void loop(){
    pthread_setname_np(pthread_self(),"render");
    while(running.load(std::memory_order_acquire)){
        uint32_t t=tail.load(std::memory_order_relaxed);
        if(t==head.load(std::memory_order_acquire)){
            std::unique_lock<std::mutex> l(wakeMutex);sleeping.store(true,std::memory_order_seq_cst);
            if(t==head.load(std::memory_order_seq_cst)){
                const auto t0=std::chrono::steady_clock::now();
                wakeCv.wait_for(l,std::chrono::milliseconds(1));
                nfsWaitAdd(4,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
            }
            sleeping.store(false,std::memory_order_relaxed);
            continue;
        }
        const Cmd& c=queue[t&(queueSize-1)];
        try{exec(c);}catch(const std::exception& e){NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU2","render thread: %s",e.what());}
        tail.store(t+1,std::memory_order_release);
        // Producers waiting for the queue to drain (sync calls) are woken by the sync command itself;
        // a plain drain() polls with a short timed wait.
    }
}
void start(){
    if(running.load())return;
    const char* e=std::getenv("NFSU2_RENDER_THREAD");enabled=!(e&&e[0]=='0');nfsSetPaceOnGameThread(enabled);
    if(!enabled)return;
    queue=new Cmd[queueSize];running=true;worker=std::thread(loop);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 render thread started (NFSU2_RENDER_THREAD=0 disables it)");
}
void stop(){
    if(!running.load())return;
    drain();running=false;{std::lock_guard<std::mutex> l(wakeMutex);wakeCv.notify_all();}
    if(worker.joinable())worker.join();
}

// Handle -> native lookups for SetTexture/SetStreamSource/... (thousands per frame): a direct-mapped cache on the
// game thread, invalidated whenever a handle is retired (handlesRetiredEpoch). Unknown handles are not cached: an
// object created later at that handle does not bump the epoch. 2048 slots: a race frame binds a few hundred vertex
// buffers, which kept evicting each other in the former 256. Plain statics (not per thread: 48 KB each): D3D9 calls
// come from guest threads holding the machine, one at a time.
constexpr uint32_t handleCacheSlots=2048;
inline uint32_t handleCacheSlot(uint32_t address){return (address*0x9E3779B1u)>>21;}   // top 11 bits
template<class T>T* cachedNative(uint32_t address){
    if(!address)return nullptr;
    struct Entry{uint32_t address;uint64_t epoch;void* native;};
    static Entry cache[handleCacheSlots];
    Entry& e=cache[handleCacheSlot(address)];
    const uint64_t curEpoch=handlesRetiredEpoch.load(std::memory_order_acquire);
    if(e.address==address&&e.epoch==curEpoch)return static_cast<T*>(e.native);
    T* native=nativeObject<T>(address);
    if(native)e={address,curEpoch,native};
    return native;
}
// Same for the Object records (AddRef/Release of textures: thousands per frame).
Object* cachedObject(uint32_t address){
    if(!address)return nullptr;
    struct Entry{uint32_t address;uint64_t epoch;Object* object;};
    static Entry cache[handleCacheSlots];
    Entry& e=cache[handleCacheSlot(address)];
    const uint64_t curEpoch=handlesRetiredEpoch.load(std::memory_order_acquire);
    if(e.address==address&&e.epoch==curEpoch)return e.object;
    std::lock_guard<std::recursive_mutex> lock(objectsMutex);
    auto found=objects.find(address);Object* o=found==objects.end()?nullptr:&found->second;
    if(o)e={address,curEpoch,o};
    return o;
}
// Dynamic vertex/index buffers: Lock/Unlock without waiting for the render thread. Each dynamic buffer gets a
// permanent guest-memory copy (filled once from the native buffer); Lock returns a pointer into it, Unlock
// queues the locked range (copied into the frame arena) as a native Lock/memcpy/Unlock on the render thread.
// The game locked a dynamic buffer every race frame, and each synchronous call made the game thread wait
// until the render thread had finished the whole previous frame. Static buffers keep the synchronous path.
struct BufferShadow {uint32_t guest=0,size=0,offset=0,lockSize=0,flags=0;bool locked=false;bool dynamic=false;bool touched=false;uint32_t temp=0;};
std::unordered_map<uint32_t,BufferShadow> bufferShadows;
void forgetBufferShadow(uint32_t address){
    auto it=bufferShadows.find(address);if(it==bufferShadows.end())return;
    if(it->second.guest)freeGuest(it->second.guest);if(it->second.temp)freeGuest(it->second.temp);bufferShadows.erase(it);
}
// A breakpoint for gdb: FO2_VB_LOCK_LOG=1 calls it on some locks of the big dynamic vertex buffers.
extern "C" __attribute__((noinline)) void fo2_lock_mark(uint32_t size){(void)size;__asm__("");}
bool lockBufferAsync(uint32_t kind,const uint32_t* args,uint32_t* count,uint32_t* result){
    Object* o=cachedObject(args[0]);if(!o||static_cast<uint32_t>(o->kind)!=kind)return false;
    auto it=bufferShadows.find(args[0]);
    if(it==bufferShadows.end()){
        uint32_t usage=0,size=0;
        if(kind==6){D3DVERTEXBUFFER_DESC d{};if(FAILED(static_cast<IDirect3DVertexBuffer9*>(o->native)->GetDesc(&d)))return false;usage=d.Usage;size=d.Size;}
        else{D3DINDEXBUFFER_DESC d{};if(FAILED(static_cast<IDirect3DIndexBuffer9*>(o->native)->GetDesc(&d)))return false;usage=d.Usage;size=d.Size;}
        BufferShadow sh;sh.size=size;sh.dynamic=(usage&D3DUSAGE_DYNAMIC)&&size&&size<=(4u<<20);
        if(sh.dynamic){
            sh.guest=allocateGuest(size);if(!sh.guest)return false;
            // Current contents, once (the render thread owns the native buffer).
            void* host=pointer(sh.guest);IUnknown* native=o->native;
            runOnRenderThread([&]{void* src=nullptr;
                HRESULT hr=kind==6?static_cast<IDirect3DVertexBuffer9*>(native)->Lock(0,0,&src,D3DLOCK_READONLY):static_cast<IDirect3DIndexBuffer9*>(native)->Lock(0,0,&src,D3DLOCK_READONLY);
                if(SUCCEEDED(hr)&&src){std::memcpy(host,src,size);if(kind==6)static_cast<IDirect3DVertexBuffer9*>(native)->Unlock();else static_cast<IDirect3DIndexBuffer9*>(native)->Unlock();}
                else std::memset(host,0,size);});
        }
        it=bufferShadows.emplace(args[0],sh).first;
    }
    BufferShadow& sh=it->second;
    // FO2_VB_LOCK_LOG=1: gdb's breakpoint fo2_lock_mark on one lock in 16 of the big dynamic vertex buffers (`bt` names the lifted callers).
    static const bool logLocks=std::getenv("FO2_VB_LOCK_LOG")!=nullptr;
    if(logLocks&&kind==6&&sh.size>=(512u<<10)){static uint32_t calls=0;if(args[2]>96&&(++calls&15)==0)fo2_lock_mark(args[2]);}
    if(!sh.dynamic){
        // Static buffers (world geometry streamed in while driving): the first lock of a new buffer fills
        // it, so it needs no previous contents. Lock hands out a zeroed temporary; Unlock queues the bytes.
        // Later locks (the contents then matter) stay synchronous. ~5 synchronous calls per frame before.
        static const bool off=[]{const char* e=std::getenv("NFSU2_ASYNC_STATIC_BUFFERS");return e&&e[0]=='0';}();
        const uint32_t offset=args[1];uint32_t size=args[2];
        // FlatOut 2 locks some static buffers again every race frame (~1.2 lock/unlock pairs a frame): at its
        // second lock a buffer gets the dynamic buffers' permanent guest copy (read once here), so this one and
        // the next locks no longer wait. FO2_ASYNC_RELOCKS=0: those locks stay synchronous.
        static const bool relocks=[]{const char* e=std::getenv("FO2_ASYNC_RELOCKS");return !(e&&e[0]=='0');}();
        if(!off&&relocks&&sh.touched&&!sh.locked&&!sh.temp&&sh.size&&sh.size<=(4u<<20)&&args[3]){
            sh.guest=allocateGuest(sh.size);
            if(sh.guest){
                void* host=pointer(sh.guest);IUnknown* native=o->native;const uint32_t bytes=sh.size;
                runOnRenderThread([&]{void* src=nullptr;
                    HRESULT hr=kind==6?static_cast<IDirect3DVertexBuffer9*>(native)->Lock(0,0,&src,D3DLOCK_READONLY):static_cast<IDirect3DIndexBuffer9*>(native)->Lock(0,0,&src,D3DLOCK_READONLY);
                    if(SUCCEEDED(hr)&&src){std::memcpy(host,src,bytes);if(kind==6)static_cast<IDirect3DVertexBuffer9*>(native)->Unlock();else static_cast<IDirect3DIndexBuffer9*>(native)->Unlock();}
                    else std::memset(host,0,bytes);});
                sh.dynamic=true;
                return lockBufferAsync(kind,args,count,result);
            }
        }
        if(off||sh.touched||sh.locked||!args[3]||(args[4]&D3DLOCK_READONLY)||!sh.size||offset>sh.size)return false;
        if(!size)size=sh.size-offset;
        if(size>sh.size-offset||size>(8u<<20))return false;
        sh.touched=true;
        const uint32_t temp=allocateGuest(size);if(!temp)return false;
        std::memset(pointer(temp),0,size);
        sh.temp=temp;sh.offset=offset;sh.lockSize=size;sh.flags=args[4];sh.locked=true;
        *count=5;write(args[3],temp);*result=D3D_OK;return true;
    }
    *count=5;
    if(sh.locked||!args[3]){*result=D3DERR_INVALIDCALL;return true;}
    uint32_t offset=args[1],size=args[2];
    if(offset>sh.size||(size&&size>sh.size-offset)){*result=D3DERR_INVALIDCALL;return true;}
    if(!size)size=sh.size-offset;
    sh.offset=offset;sh.lockSize=size;sh.flags=args[4];sh.locked=true;
    write(args[3],sh.guest+offset);*result=D3D_OK;return true;
}
bool unlockBufferAsync(uint32_t kind,const uint32_t* args,uint32_t* count,uint32_t* result){
    auto it=bufferShadows.find(args[0]);if(it==bufferShadows.end())return false;
    BufferShadow& sh=it->second;
    if(!sh.dynamic){
        if(!sh.temp||!sh.locked)return false;
        *count=1;sh.locked=false;*result=D3D_OK;
        Object* o=cachedObject(args[0]);
        if(o){Cmd c{};c.op=OpBufferUpdate;c.obj=o->native;c.a[0]=sh.offset;c.a[1]=sh.lockSize;c.a[2]=sh.flags;c.a[3]=kind==7;
            c.p0=copyData(pointer(sh.temp),sh.lockSize);push(c);}
        freeGuest(sh.temp);sh.temp=0;return true;
    }
    *count=1;
    if(!sh.locked){*result=D3DERR_INVALIDCALL;return true;}
    sh.locked=false;*result=D3D_OK;
    if(sh.flags&D3DLOCK_READONLY||!sh.lockSize)return true;
    Object* o=cachedObject(args[0]);if(!o)return true;
    Cmd c{};c.op=OpBufferUpdate;c.obj=o->native;c.a[0]=sh.offset;c.a[1]=sh.lockSize;c.a[2]=sh.flags;c.a[3]=kind==7;
    c.p0=copyData(pointer(sh.guest+sh.offset),sh.lockSize);push(c);return true;
}
// Texture uploads (streamed world textures while driving): LockRect/UnlockRect of a whole level that was
// never locked before (or is locked with DISCARD) no longer wait for the render thread. Lock hands out a
// zeroed guest buffer (what the synchronous path returns for a fresh level); Unlock queues the bytes, and
// the render thread stores them (DXT shadow + CPU decode for the Mali, which has no DXT) and uploads.
// Each synchronous lock/unlock used to wait for the render thread to finish the whole queued frame: the
// game thread spent 75-165 ms in them on frames that streamed 10-30 textures (NFSU2 HITCH lines, v14).
struct TexLock {uint32_t guest=0,size=0,pitch=0,rows=0,rowBytes=0;};
std::unordered_map<uint64_t,TexLock> texLocks;
std::unordered_set<uint64_t> texTouched;  // texture levels locked at least once: their contents matter
void forgetTextureLocks(uint32_t address){
    for(uint32_t level=0;level<16;++level){
        const uint64_t key=(uint64_t(address)<<32)|level;texTouched.erase(key);
        auto it=texLocks.find(key);if(it!=texLocks.end()){freeGuest(it->second.guest);texLocks.erase(it);}
    }
}
bool textureLockAsync(uint32_t method,const uint32_t* args,uint32_t* count,uint32_t* result){
    static const bool off=[]{const char* e=std::getenv("NFSU2_ASYNC_TEX");return e&&e[0]=='0';}();
    const uint32_t level=args[1];const uint64_t key=(uint64_t(args[0])<<32)|level;
    if(method==19){
        const uint32_t out=args[2],rect=args[3],flags=args[4];
        Object* o=cachedObject(args[0]);if(!o||o->kind!=Kind::Texture||!out||level>=16)return false;
        const bool fresh=texTouched.insert(key).second;
        if(off||rect||(flags&D3DLOCK_READONLY)||(!fresh&&!(flags&D3DLOCK_DISCARD))||texLocks.count(key))return false;
        TexLock L;CompressedTexture* ct=o->compressed.texture.get();
        if(ct){if(level>=ct->levels||ct->faces!=1)return false;L.pitch=ct->pitch(level);L.rows=(ct->mipHeight(level)+3)/4;L.rowBytes=L.pitch;}
        else{
            D3DSURFACE_DESC d{};if(FAILED(static_cast<IDirect3DTexture9*>(o->native)->GetLevelDesc(level,&d)))return false;
            if(d.Pool==D3DPOOL_DEFAULT||(d.Usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return false;
            try{L.rowBytes=rowBytes(d.Format,d.Width);}catch(...){return false;}
            L.rows=blockCompressed(d.Format)?(d.Height+3)/4:d.Height;L.pitch=(L.rowBytes+3)&~3u;
        }
        L.size=L.pitch*L.rows;if(!L.size||L.size>(8u<<20))return false;
        L.guest=allocateGuest(L.size);if(!L.guest)return false;std::memset(pointer(L.guest),0,L.size);
        texLocks[key]=L;write(out,L.pitch);write(out+4,L.guest);*count=5;*result=D3D_OK;return true;
    }
    auto it=texLocks.find(key);if(it==texLocks.end())return false;
    TexLock L=it->second;texLocks.erase(it);
    Object* o=cachedObject(args[0]);
    if(o&&o->kind==Kind::Texture){
        Cmd c{};c.op=OpTexUpload;c.obj=o->native;c.obj2=o->compressed.texture.get();
        c.a[0]=level;c.a[1]=L.size;c.a[2]=L.pitch;c.a[3]=L.rows;c.a[4]=L.rowBytes;
        c.p0=copyData(pointer(L.guest),L.size);
        if(CompressedTexture* compressed=o->compressed.texture.get();compressed&&decodePool.enabled()){
            // the same bytes the render thread copies into the shadow (c.p0 lives until the command has run)
            const size_t levelBytes=std::min<size_t>(compressed->bytes(0,level).size(),L.size);
            c.p1=new DecodeJob{compressed->mode(),compressed->mipWidth(level),compressed->mipHeight(level),static_cast<const uint8_t*>(c.p0),levelBytes,{},{},false,false};
            decodePool.submit(static_cast<DecodeJob*>(const_cast<void*>(c.p1)));
        }
        push(c);
    }
    freeGuest(L.guest);*count=2;*result=D3D_OK;return true;
}
// ---- Surface locks without waiting (Alien Shooter 2: its HUD text textures, ~20-45 lock/unlock pairs a frame) ----
// A plain texture level (not a render target, not DXT) gets a guest copy of its pixels, read once through the
// render thread. Its locks hand out that copy at once; an unlock that may have written queues the whole surface
// for upload. The copies are dropped by what can change pixels behind them: the device's copy calls
// (UpdateSurface ... ColorFill), Reset, and a lock of the owning texture. A copy belongs to one native surface:
// a handle reused by another object is caught there (the campaign creates and frees textures by the thousand,
// so a global epoch would refill every copy every frame). AS2_ASYNC_SURFACE_LOCKS=0: the synchronous path.
struct SurfaceShadow {uint32_t guest=0,pitch=0,rows=0,rowBytes=0,bpp=0,flags=0,parent=0;IUnknown* native=nullptr;bool locked=false,stale=false;};
std::unordered_map<uint32_t,SurfaceShadow> surfaceShadows;
void dropSurfaceShadows(uint32_t parent=0){   // 0: all of them
    for(auto it=surfaceShadows.begin();it!=surfaceShadows.end();){
        if(parent&&it->second.parent!=parent){++it;continue;}
        if(it->second.locked){it->second.stale=true;++it;continue;}
        freeGuest(it->second.guest);it=surfaceShadows.erase(it);
    }
}
// The copies of surfaces released since (their handle gone or reused): their guest memory back.
void sweepSurfaceShadows(){
    for(auto it=surfaceShadows.begin();it!=surfaceShadows.end();){
        Object* o=cachedObject(it->first);
        if(!it->second.locked&&(!o||o->native!=it->second.native)){freeGuest(it->second.guest);it=surfaceShadows.erase(it);}
        else ++it;
    }
}
// Surfaces from CreateOffscreenPlainSurface: CPU pixels only, so their UnlockRect (the copy of the guest's
// pixels back into the native surface) runs on this thread; their LockRect stays synchronous, since it
// must see a queued GetRenderTargetData. A handle reused by another object leaves the set (sweep).
bool surfaceLockAsync(uint32_t method,const uint32_t* args,uint32_t* count,uint32_t* result){
    static const bool on=[]{const char* e=std::getenv("AS2_ASYNC_SURFACE_LOCKS");return !(e&&e[0]=='0');}();
    if(!on)return false;
    if(method==14){
        auto plain=plainSurfaces.find(args[0]);
        if(plain!=plainSurfaces.end()){
            Object* o=cachedObject(args[0]);
            if(o&&o->native==plain->second){*result=dispatchGuestD3D9Inner(methodBase+5*4096+14*16,args,count);return true;}
            plainSurfaces.erase(plain);
        }
    }
    Object* self=cachedObject(args[0]);
    auto found=surfaceShadows.find(args[0]);
    // the handle now names another object: the copy is not its pixels
    if(found!=surfaceShadows.end()&&!found->second.locked&&(!self||self->native!=found->second.native)){
        freeGuest(found->second.guest);surfaceShadows.erase(found);found=surfaceShadows.end();
    }
    if(method==14){  // UnlockRect
        if(found==surfaceShadows.end()||!found->second.locked)return false;
        SurfaceShadow& s=found->second;s.locked=false;
        if(!(s.flags&D3DLOCK_READONLY)&&s.native){
            Cmd c{};c.op=OpSurfaceUpload;c.obj=s.native;c.a[0]=s.pitch;c.a[1]=s.rows;c.a[2]=s.rowBytes;
            c.p0=copyData(pointer(s.guest),size_t(s.pitch)*s.rows);push(c);
        }
        if(s.stale){freeGuest(s.guest);surfaceShadows.erase(found);}
        *count=1;*result=D3D_OK;return true;
    }
    // LockRect(surface, out D3DLOCKED_RECT, rect, flags)
    const uint32_t out=args[1],rect=args[2],flags=args[3];
    if(!out)return false;
    if(found==surfaceShadows.end()){
        Object* o=self;if(!o||o->kind!=Kind::Surface||!o->parent||o->compressed.texture)return false;
        Object* parent=cachedObject(o->parent);if(!parent||parent->compressed.texture)return false;
        auto native=static_cast<IDirect3DSurface9*>(o->native);
        D3DSURFACE_DESC d{};if(FAILED(native->GetDesc(&d))||d.Usage||blockCompressed(d.Format))return false;
        SurfaceShadow s;
        try{s.rowBytes=rowBytes(d.Format,d.Width);}catch(...){return false;}
        s.bpp=s.rowBytes/d.Width;s.pitch=(s.rowBytes+3)&~3u;s.rows=d.Height;
        const size_t bytes=size_t(s.pitch)*s.rows;if(!bytes||bytes>(256u<<10))return false;
        s.guest=allocateGuest(uint32_t(bytes));if(!s.guest)return false;
        uint8_t* dst=static_cast<uint8_t*>(pointer(s.guest));bool ok=false;
        runOnRenderThread([&]{D3DLOCKED_RECT lr{};
            if(SUCCEEDED(native->LockRect(&lr,nullptr,D3DLOCK_READONLY))&&lr.pBits){
                for(uint32_t row=0;row<s.rows;++row)std::memcpy(dst+size_t(row)*s.pitch,static_cast<const uint8_t*>(lr.pBits)+size_t(row)*lr.Pitch,s.rowBytes);
                native->UnlockRect();ok=true;}});
        if(!ok){freeGuest(s.guest);return false;}
        s.native=o->native;s.parent=o->parent;found=surfaceShadows.emplace(args[0],s).first;
    }
    SurfaceShadow& s=found->second;if(s.locked)return false;
    uint32_t offset=0;
    if(rect){const RECT* r=static_cast<const RECT*>(pointer(rect));offset=uint32_t(r->top)*s.pitch+uint32_t(r->left)*s.bpp;}
    s.locked=true;s.flags=flags;
    write(out,s.pitch);write(out+4,s.guest+offset);*count=4;*result=D3D_OK;return true;
}
// Redundant state calls (same value as last queued): true = drop it (count set). Otherwise the shadow is
// updated, as the caller queues the call (none of these methods can fail to queue afterwards, except
// Set*ShaderConstantF with a null pointer, which is checked first).
inline bool filterState(uint32_t method,const uint32_t* args,uint32_t* count){
    Shadow& sh=*shadow;
    switch(method){
    case 57:*count=3;if(args[1]<256){if(sh.rsValid[args[1]]&&sh.rs[args[1]]==args[2])return true;sh.rs[args[1]]=args[2];sh.rsValid[args[1]]=true;}return false;
    case 65:*count=3;
        if(args[1]<16){if(sh.texValid[args[1]]&&sh.tex[args[1]]==args[2]&&sh.texEpoch[args[1]]==handlesRetiredEpoch)return true;
            sh.tex[args[1]]=args[2];sh.texEpoch[args[1]]=handlesRetiredEpoch;sh.texValid[args[1]]=true;}
        return false;
    case 67:*count=4;if(args[1]<8&&args[2]<33){uint32_t& v=sh.tss[args[1]][args[2]];bool& ok=sh.tssValid[args[1]][args[2]];
        if(ok&&v==args[3])return true;v=args[3];ok=true;}return false;
    case 69:*count=4;if(args[1]<16&&args[2]<16){uint32_t& v=sh.ss[args[1]][args[2]];bool& ok=sh.ssValid[args[1]][args[2]];
        if(ok&&v==args[3])return true;v=args[3];ok=true;}return false;
    case 87:*count=2;if(sh.declValid&&sh.decl==args[1]&&sh.declE==handlesRetiredEpoch)return true;
        sh.decl=args[1];sh.declE=handlesRetiredEpoch;sh.declValid=true;sh.fvfValid=false;return false;
    case 89:*count=2;if(sh.fvfValid&&sh.fvf==args[1])return true;sh.fvf=args[1];sh.fvfValid=true;sh.declValid=false;return false;
    case 92:*count=2;if(sh.vsValid&&sh.vs==args[1]&&sh.vsE==handlesRetiredEpoch)return true;
        sh.vs=args[1];sh.vsE=handlesRetiredEpoch;sh.vsValid=true;return false;
    case 107:*count=2;if(sh.psValid&&sh.ps==args[1]&&sh.psE==handlesRetiredEpoch)return true;
        sh.ps=args[1];sh.psE=handlesRetiredEpoch;sh.psValid=true;return false;
    case 94:case 109:{*count=4;if(!args[2])return false;
        // Constants equal to what was last queued for those registers are dropped.
        const bool vs=method==94;const uint32_t limit=vs?256u:32u;
        if(args[1]+args[3]<=limit&&args[3]){
            float (*regs)[4]=vs?sh.vsc:sh.psc;bool* valid=vs?sh.vscValid:sh.pscValid;
            const void* src=pointer(args[2]);bool same=true;
            for(uint32_t r=0;r<args[3]&&same;++r)same=valid[args[1]+r];
            if(same&&std::memcmp(regs[args[1]],src,size_t(args[3])*16)==0)return true;
            std::memcpy(regs[args[1]],src,size_t(args[3])*16);for(uint32_t r=0;r<args[3];++r)valid[args[1]+r]=true;
        }
        return false;}
    case 100:*count=5;
        if(args[1]<4){uint32_t* v=sh.sb[args[1]];
            if(sh.sbValid[args[1]]&&v[0]==args[2]&&v[1]==args[3]&&v[2]==args[4]&&sh.sbE[args[1]]==handlesRetiredEpoch)return true;
            v[0]=args[2];v[1]=args[3];v[2]=args[4];sh.sbE[args[1]]=handlesRetiredEpoch;sh.sbValid[args[1]]=true;}
        return false;
    case 104:*count=2;if(sh.ibValid&&sh.ib==args[1]&&sh.ibE==handlesRetiredEpoch)return true;
        sh.ib=args[1];sh.ibE=handlesRetiredEpoch;sh.ibValid=true;return false;
    }
    return false;
}
// Guest thread: queue a deferrable call, or return false (the caller then runs it synchronously).
// ---- Vertex/index buffers created and released without waiting for the render thread ----
// A few game systems create and release dynamic buffers every frame (~2-3 CreateIndexBuffer/VertexBuffer and
// final Releases per frame in a race); each was a synchronous call that drained the render queue (~1 ms each,
// "sync calls 5.5 ms/frame" on the device). The GLES buffer object is created here, on the game thread, with
// its GL buffer deferred to first use on the render thread (g_glesDeferBufferGL); the guest wrapper is made
// at once. A final Release retires the guest wrapper here and queues the native Release behind every command
// that still uses the buffer. NFSU2_ASYNC_BUFFERS=0: synchronous as before.
uint64_t statAsyncCreates=0,statAsyncReleases=0;
static bool asyncBuffersOn(){static const bool on=[]{const char* e=std::getenv("NFSU2_ASYNC_BUFFERS");return !(e&&e[0]=='0');}();return on;}
bool createBufferAsync(IUnknown* deviceObject,uint32_t method,const uint32_t* args,uint32_t* count,uint32_t* result){
    if(!asyncBuffersOn()||!args[5]||args[6]||!args[1]||args[1]>(16u<<20))return false;
    auto device=static_cast<IDirect3DDevice9*>(deviceObject);
    IUnknown* native=nullptr;HRESULT status;
    g_glesDeferBufferGL=true;
    if(method==26){IDirect3DVertexBuffer9* b=nullptr;status=device->CreateVertexBuffer(args[1],args[2],args[3],static_cast<D3DPOOL>(args[4]),&b,nullptr);native=b;}
    else{IDirect3DIndexBuffer9* b=nullptr;status=device->CreateIndexBuffer(args[1],args[2],static_cast<D3DFORMAT>(args[3]),static_cast<D3DPOOL>(args[4]),&b,nullptr);native=b;}
    g_glesDeferBufferGL=false;
    if(FAILED(status)||!native)return false;
    const uint32_t handle=wrap(native,method==26?Kind::VertexBuffer:Kind::IndexBuffer);
    write(args[5],handle);
    // A new buffer's contents are zeros: give dynamic ones their lock shadow now, so the first Lock does not
    // read the contents back through the render thread (lockBufferAsync).
    BufferShadow sh;sh.size=args[1];sh.dynamic=(args[2]&D3DUSAGE_DYNAMIC)&&sh.size<=(4u<<20);
    if(sh.dynamic){sh.guest=allocateGuest(sh.size);if(sh.guest)std::memset(pointer(sh.guest),0,sh.size);else sh.dynamic=false;}
    if(sh.dynamic)bufferShadows[handle]=sh;
    *count=7;*result=D3D_OK;++statAsyncCreates;return true;
}
// ---- Textures created without waiting for the render thread ----
// A world section load creates ~500 textures at once. Each CreateTexture was a synchronous call: the game thread
// waited while the render thread drained the queue and allocated the whole GL mip chain (~2 ms per texture on the
// device: a 1.4 s freeze, and hitches whenever streaming brings in a few). The GLES texture is created here with
// its GL storage deferred to first use on the render thread (g_glesDeferTextureGL); the guest wrapper is made at
// once. Render-target and depth textures keep the synchronous path. NFSU2_ASYNC_TEXTURES=0: synchronous as before.
uint64_t statAsyncTextureCreates=0;
static bool asyncTexturesOn(){static const bool on=[]{const char* e=std::getenv("NFSU2_ASYNC_TEXTURES");return !(e&&e[0]=='0');}();return on;}
bool createTextureAsync(IUnknown* deviceObject,const uint32_t* args,uint32_t* count,uint32_t* result){
    // CreateTexture(device, width, height, levels, usage, format, pool, out texture, shared handle)
    const uint32_t width=args[1],height=args[2],levels=args[3],usage=args[4],outTexture=args[7],sharedHandle=args[8];
    const auto requestedFormat=static_cast<D3DFORMAT>(args[5]);const auto pool=static_cast<D3DPOOL>(args[6]);
    if(!asyncTexturesOn()||sharedHandle||!outTexture||!width||!height||(usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return false;
    // nativeTextureFormat() without its device queries: the GLES device samples no DXT format (CPU decode to RGBA)
    const D3DFORMAT nativeFormat=compressedFormat(requestedFormat)?D3DFMT_A8B8G8R8:requestedFormat;
    auto device=static_cast<IDirect3DDevice9*>(deviceObject);IDirect3DTexture9* texture=nullptr;
    g_glesDeferTextureGL=true;
    const HRESULT status=device->CreateTexture(width,height,levels,usage,nativeFormat,pool,&texture,nullptr);
    g_glesDeferTextureGL=false;
    if(FAILED(status)||!texture)return false;
    const uint32_t handle=wrap(texture,Kind::Texture);
    if(nativeFormat!=requestedFormat){
        std::lock_guard<std::recursive_mutex> lock(objectsMutex);
        objects.at(handle).compressed.texture=std::make_shared<CompressedTexture>(requestedFormat,width,height,texture->GetLevelCount(),1);
    }
    write(outTexture,handle);*count=9;*result=D3D_OK;++statAsyncTextureCreates;return true;
}
// The game's device (one): its guest handle and native object, valid while handlesRetiredEpoch is unchanged.
uint32_t deviceHandle=0;IUnknown* deviceNative=nullptr;uint64_t deviceHandleEpoch=~0ull;
// ---- Render target and depth surface queries without waiting (FlatOut 2) ----
// A race frame asks GetRenderTarget ~2x and GetDepthStencilSurface ~1x (its post effects), each a synchronous call
// that waited for the render thread to finish the queued frame (~4.5 ms each). The surfaces bound are known on
// this side: what the game set through the queue, and before that what a first synchronous query returned
// (noteSurfaceQuery). The answer is that guest surface with one more reference, as D3D does. Reset forgets
// them. FO2_ASYNC_TARGETS=0: synchronous as before.
uint32_t knownTarget[4]={};bool targetKnown[4]={};uint32_t knownDepth=0;bool depthKnown=false;
static bool asyncTargetsOn(){static const bool on=[]{const char* e=std::getenv("FO2_ASYNC_TARGETS");return !(e&&e[0]=='0');}();return on;}
// The game Releases what it got at once; its count then fell to zero, the wrapper was retired and the next query
// went synchronous again to make a new one. A surface learnt from a query keeps one reference of the bridge
// (a "hold", with its native AddRef queued like any other) until Reset.
std::vector<uint32_t> heldSurfaces;
void holdSurface(uint32_t h){
    Object* o=cachedObject(h);if(!o||o->kind!=Kind::Surface)return;
    for(uint32_t held:heldSurfaces)if(held==h)return;
    if(!o->refQueued){o->refQueued=true;refDirty.push_back(o);}
    ++o->refDelta;++o->references;heldSurfaces.push_back(h);
}
void releaseHeldSurfaces(){   // before Reset: the default surfaces must have no references left
    for(uint32_t h:heldSurfaces){Object* o=cachedObject(h);if(!o)continue;
        if(!o->refQueued){o->refQueued=true;refDirty.push_back(o);}
        --o->refDelta;--o->references;}
    heldSurfaces.clear();
}
void noteSurfaceQuery(uint32_t method,const uint32_t* args){
    auto at=[](uint32_t address){uint32_t v;std::memcpy(&v,pointer(address),4);return v;};
    if(method==38&&args[1]<4&&args[2]){knownTarget[args[1]]=at(args[2]);targetKnown[args[1]]=true;holdSurface(knownTarget[args[1]]);}
    if(method==40&&args[1]){knownDepth=at(args[1]);depthKnown=true;holdSurface(knownDepth);}
}
// The guest surface h handed out with one more reference; false if it is no longer a live surface.
bool handOutSurface(uint32_t h,uint32_t out){
    Object* o=cachedObject(h);if(!o||o->kind!=Kind::Surface)return false;
    if(!o->refQueued){o->refQueued=true;refDirty.push_back(o);}
    ++o->refDelta;++o->references;write(out,h);return true;
}
uint32_t streamBind[4][3]={};   // SetStreamSource as the game issued it: buffer handle, offset, stride
// FO2_SPARKS=0: FlatOut 2's particle strips (sparks, dust: 24-byte vertices, triangle strips from a big dynamic
// buffer) are not drawn at all (their streaks are otherwise repaired on the render side,
// IDirect3DDevice9GLES::patchParticleStrip). The HUD is drawn the same way from the same buffer but flat on the
// screen (c3, the clip w row, is 0,0,0,1): only perspective draws count, and with c3 unknown here (the
// constant shadow is forgotten at synchronous calls) the draw is left alone.
bool particleStrip(uint32_t type,uint32_t vertices){
    const uint32_t handle=streamBind[0][0],stride=streamBind[0][2];
    if(type!=D3DPT_TRIANGLESTRIP||stride!=24||!handle||vertices<4)return false;
    if(!shadow||!shadow->vscValid[3])return false;
    const float* w=shadow->vsc[3];
    if(std::fabs(w[0])+std::fabs(w[1])+std::fabs(w[2])<1e-6f)return false;
    auto it=bufferShadows.find(handle);
    return it!=bufferShadows.end()&&it->second.dynamic&&it->second.size>=(512u<<10);
}
// A device method call queued for the render thread, after the redundant-call filter: every deferrable method but
// Present and the resource creations. false = not deferrable here (the caller makes it a synchronous call).
__attribute__((always_inline)) inline bool queueDeviceCall(uint32_t method,const uint32_t* args,uint32_t* count,IUnknown* device){
    if(method==100&&args[1]<4){streamBind[args[1]][0]=args[2];streamBind[args[1]][1]=args[3];streamBind[args[1]][2]=args[4];}
    else if(method==83||method==84)streamBind[0][0]=0;   // a UP draw unsets stream 0
    else if(method==81){
        static const bool sparks=[]{const char* e=std::getenv("FO2_SPARKS");return !(e&&e[0]=='0');}();
        if(!sparks&&particleStrip(args[1],vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[3]))){*count=4;return true;}
    }
    if(filterState(method,args,count)){++filtered;return true;}
    Cmd c{};c.op=OpDevice;c.method=static_cast<uint16_t>(method);c.obj=device;
    for(int i=0;i<9;++i)c.a[i]=args[i];
        switch(method){
        case 3:*count=1;return true;  // TestCooperativeLevel: never lost here (was a synchronous call every frame)
        case 37:*count=3;c.obj2=cachedNative<IDirect3DSurface9>(args[2]);viewportKnown=false;  // resets the viewport to the target
            if(args[1]==0&&c.obj2){D3DSURFACE_DESC d{};if(SUCCEEDED(static_cast<IDirect3DSurface9*>(c.obj2)->GetDesc(&d))){viewportShadow={0,0,d.Width,d.Height,0.f,1.f};viewportKnown=true;}}
            break;
        case 39:*count=2;c.obj2=cachedNative<IDirect3DSurface9>(args[1]);break;
        case 41:case 42:*count=1;break;
        case 43:*count=7;c.p0=args[2]&&args[1]?copyData(pointer(args[2]),size_t(args[1])*sizeof(D3DRECT)):nullptr;break;
        case 44:case 46:*count=3;if(!args[2])return false;c.p0=copyData(pointer(args[2]),sizeof(D3DMATRIX));break;
        case 47:*count=2;if(!args[1])return false;c.p0=copyData(pointer(args[1]),sizeof(D3DVIEWPORT9));
            if(recordingBlock){recorded.has=recorded.known=true;std::memcpy(&recorded.value,c.p0,sizeof(D3DVIEWPORT9));}  // recorded, not applied
            else{std::memcpy(&viewportShadow,c.p0,sizeof(D3DVIEWPORT9));viewportKnown=true;}
            break;
        case 49:*count=2;if(!args[1])return false;c.p0=copyData(pointer(args[1]),sizeof(D3DMATERIAL9));break;
        case 51:*count=3;if(!args[2])return false;c.p0=copyData(pointer(args[2]),sizeof(D3DLIGHT9));break;
        case 53:*count=3;break;
        case 55:*count=3;if(!args[2])return false;c.p0=copyData(pointer(args[2]),4*sizeof(float));break;
        case 57:*count=3;break;
        case 65:*count=3;c.obj2=cachedNative<IDirect3DBaseTexture9>(args[2]);break;
        case 67:*count=4;break;
        case 69:*count=4;break;
        case 75:*count=2;if(!args[1])return false;c.p0=copyData(pointer(args[1]),sizeof(RECT));break;
        case 77:*count=2;break;
        case 81:*count=4;break;
        case 82:*count=7;break;
        case 83:{*count=5;if(!args[3])return false;shadow->sbValid[0]=false;
            size_t bytes=size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[2]))*args[4];
            c.p0=copyData(pointer(args[3]),bytes);break;}
        case 84:{*count=9;if(!args[5]||!args[7])return false;shadow->sbValid[0]=false;shadow->ibValid=false;
            size_t idx=size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[4]))*(args[6]==D3DFMT_INDEX16?2:4);
            size_t vtx=size_t(args[2]+args[3])*args[8];
            c.p0=copyData(pointer(args[5]),idx);c.p1=copyData(pointer(args[7]),vtx);break;}
        case 87:*count=2;c.obj2=cachedNative<IDirect3DVertexDeclaration9>(args[1]);break;
        case 89:*count=2;break;
        case 92:*count=2;c.obj2=cachedNative<IDirect3DVertexShader9>(args[1]);break;
        case 107:*count=2;c.obj2=cachedNative<IDirect3DPixelShader9>(args[1]);break;
        case 94:case 109:*count=4;if(!args[2])return false;c.p0=copyData(pointer(args[2]),size_t(args[3])*16);break;
        case 96:case 111:*count=4;if(!args[2])return false;c.p0=copyData(pointer(args[2]),size_t(args[3])*16);break;
        case 98:case 113:*count=4;if(!args[2])return false;c.p0=copyData(pointer(args[2]),size_t(args[3])*4);break;
        case 100:*count=5;c.obj2=cachedNative<IDirect3DVertexBuffer9>(args[2]);break;
        case 102:*count=3;break;
        case 104:*count=2;c.obj2=cachedNative<IDirect3DIndexBuffer9>(args[1]);break;
        default:return false;
        }
        ++statPushed;push(c);return true;
}
bool tryDeferImpl(uint32_t kind,uint32_t method,const uint32_t* args,uint32_t* count,uint32_t* result){
    *result=0;
    if(!shadow)forgetShadow();
    // IDirect3DTexture9::GetLevelCount (~1 a race frame in FlatOut 2): fixed when the texture was created.
    if(kind==3&&method==13&&asyncTargetsOn()){
        Object* o=cachedObject(args[0]);
        if(o&&o->kind==Kind::Texture){*count=1;*result=static_cast<IDirect3DTexture9*>(o->native)->GetLevelCount();return true;}
    }
    if(kind==2){
        // The game has one device: remember its handle -> native mapping (deviceHandle, deviceNative).
        const uint64_t curEpoch=handlesRetiredEpoch.load(std::memory_order_acquire);
        if(args[0]!=deviceHandle||deviceHandleEpoch!=curEpoch){
            std::lock_guard<std::recursive_mutex> lock(objectsMutex);
            auto found=objects.find(args[0]);if(found==objects.end()||found->second.kind!=Kind::Device)return false;
            deviceHandle=args[0];deviceNative=found->second.native;deviceHandleEpoch=curEpoch;
        }
        if(method==26||method==27)return createBufferAsync(deviceNative,method,args,count,result);
        if(method==23)return createTextureAsync(deviceNative,args,count,result);
        if(method==48){if(!viewportKnown||!args[1])return false;std::memcpy(pointer(args[1]),&viewportShadow,sizeof(D3DVIEWPORT9));*count=2;return true;}
        if(asyncBuffersOn()&&(method==30||method==32||method==36)){
            // Alien Shooter 2's ground streaming (~1 of each a frame), without waiting for the render thread:
            // CreateOffscreenPlainSurface makes a CPU-only surface (no GL), so it runs here; UpdateSurface and
            // GetRenderTargetData are queued (the game's next lock of the destination is synchronous anyway).
            if(method==36){if(args[6])return false;*result=dispatchGuestD3D9Inner(methodBase+2*4096+36*16,args,count);
                if(*result==D3D_OK&&args[5]){const uint32_t h=*static_cast<const uint32_t*>(pointer(args[5]));if(Object* o=cachedObject(h))plainSurfaces[h]=o->native;}
                return true;}
            if(hudRecording)hudRecordInvalidate();
            if(passMode==2)passConflict=true;
            if(capActive)capBad=true;
            Cmd c{};c.op=OpDevice;c.method=static_cast<uint16_t>(method);c.obj=deviceNative;
            for(int i=0;i<9;++i)c.a[i]=args[i];
            if(method==30){
                c.obj2=cachedNative<IDirect3DSurface9>(args[1]);c.p1=cachedNative<IDirect3DSurface9>(args[3]);
                c.p0=args[2]?copyData(pointer(args[2]),sizeof(RECT)):nullptr;
                c.a[8]=0;if(args[4]){POINT p;std::memcpy(&p,pointer(args[4]),sizeof p);c.a[6]=uint32_t(p.x);c.a[7]=uint32_t(p.y);c.a[8]=1;}
                *count=5;
            }else{c.obj2=cachedNative<IDirect3DSurface9>(args[1]);c.p1=cachedNative<IDirect3DSurface9>(args[2]);*count=3;}
            if(!c.obj2||!c.p1)return false;
            dropSurfaceShadows(0);   // the destination's pixels change behind any guest copy
            push(c);*result=D3D_OK;return true;
        }
        if(asyncTargetsOn()){
            if(method==16){for(auto& k:targetKnown)k=false;depthKnown=false;releaseHeldSurfaces();flushRefDeltas();}   // Reset: synchronous, recreates the defaults
            if(method==38&&args[1]<4&&args[2]&&targetKnown[args[1]]){
                const uint32_t h=knownTarget[args[1]];*count=3;
                if(!h){write(args[2],0);*result=D3DERR_NOTFOUND;return true;}
                if(handOutSurface(h,args[2])){*result=D3D_OK;return true;}
                targetKnown[args[1]]=false;return false;
            }
            if(method==40&&args[1]&&depthKnown){
                const uint32_t h=knownDepth;*count=2;
                if(!h){write(args[1],0);*result=D3DERR_NOTFOUND;return true;}
                if(handOutSurface(h,args[1])){*result=D3D_OK;return true;}
                depthKnown=false;return false;
            }
            if(method==34){   // StretchRect(source, source rect, destination, destination rect, filter)
                Cmd c{};c.op=OpDevice;c.method=34;c.obj=deviceNative;
                for(int i=0;i<9;++i)c.a[i]=args[i];
                c.obj2=cachedNative<IDirect3DSurface9>(args[1]);c.p1=cachedNative<IDirect3DSurface9>(args[3]);
                if(!c.obj2||!c.p1)return false;
                RECT rects[2]{};c.a[6]=0;
                if(args[2]){std::memcpy(&rects[0],pointer(args[2]),sizeof(RECT));c.a[6]|=1;}
                if(args[4]){std::memcpy(&rects[1],pointer(args[4]),sizeof(RECT));c.a[6]|=2;}
                c.p0=copyData(rects,sizeof rects);
                if(hudRecording)hudRecordInvalidate();
                if(passMode==2)passConflict=true;
                if(capActive)capBad=true;
                dropSurfaceShadows(0);
                *count=6;push(c);*result=D3D_OK;return true;
            }
        }
        if(method!=17){
            const bool queued=queueDeviceCall(method,args,count,deviceNative);
            if(method==37&&args[1]<4){knownTarget[args[1]]=args[2];targetKnown[args[1]]=queued;}  // a synchronous Set: asked again
            if(method==39){knownDepth=args[1];depthKnown=queued;}
            return queued;
        }
        // Present
        Cmd c{};c.op=OpDevice;c.method=17;c.obj=deviceNative;
        for(int i=0;i<9;++i)c.a[i]=args[i];
        {if(args[4])return false;*count=5;
            static bool noted=false;if(!noted){noted=true;d3d9NoteGameThread();}
            c.p0=args[1]?copyData(pointer(args[1]),sizeof(RECT)):nullptr;c.p1=args[2]?copyData(pointer(args[2]),sizeof(RECT)):nullptr;
            c.obj2=dxvkAndroidGetWindowHandle();if(!c.obj2)return false;
            flushRefDeltas();
            uint64_t frame=presentsQueued.fetch_add(1)+1;push(c,true);
            if(frame%60==0)sweepSurfaceShadows();
            if(frame%240==0){static uint64_t f0=0,p0=0,s0=0;
                std::fprintf(stderr,"[D3D9 queue] per frame: queued=%.0f filtered=%.0f sync=%.1f\n",(statPushed-p0)/240.0,(filtered-f0)/240.0,(statSyncs-s0)/240.0);
                f0=filtered;p0=statPushed;s0=statSyncs;
                std::vector<std::pair<uint32_t,uint32_t>> v(syncCounts.begin(),syncCounts.end());
                std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second>b.second;});
                std::fprintf(stderr,"[D3D9 queue] synchronous calls per frame by method:");
                for(size_t i=0;i<v.size()&&i<8;++i)std::fprintf(stderr," kind%u/m%u=%.1f",(v[i].first-methodBase)/4096,((v[i].first-methodBase)%4096)/16,v[i].second/240.0);
                std::fprintf(stderr," (async buffer creates %llu, releases %llu, texture creates %llu so far)\n",(unsigned long long)statAsyncCreates,(unsigned long long)statAsyncReleases,(unsigned long long)statAsyncTextureCreates);syncCounts.clear();}
            // At most one frame in flight: wait for the previous Present, then reuse its arena.
            // The wait releases the guest machine so the game's other threads (sound) can run meanwhile.
            d3d9BridgeWaitWithoutMachine([&]{nfsEmulatePace();waitDone([&]{return presentsDone.load(std::memory_order_acquire)+1>=frame;},50,0);});
            arenaIndex^=1;arenas[arenaIndex].used=0;
            return true;}
    }
    // Descriptions (Surface GetDesc, texture GetLevelDesc) read immutable object data: answered on this
    // thread, without waiting for the render thread to drain the queue (they were synchronous every frame).
    if((kind==5&&method==12)||((kind==3||kind==4)&&method==17)){
        *result=dispatchGuestD3D9Inner(methodBase+kind*4096+method*16,args,count);return true;
    }
    if(kind==5&&(method==13||method==14))return surfaceLockAsync(method,args,count,result);
    // GetSurfaceLevel hands out the texture's existing surface object (no GL work): answered on this thread.
    if(kind==3&&method==18&&asyncBuffersOn()){*result=dispatchGuestD3D9Inner(methodBase+kind*4096+method*16,args,count);return true;}
    if(kind==3&&(method==19||method==20)){dropSurfaceShadows(args[0]);return textureLockAsync(method,args,count,result);}
    if((kind==6||kind==7)&&method==11)return lockBufferAsync(kind,args,count,result);
    if((kind==6||kind==7)&&method==12)return unlockBufferAsync(kind,args,count,result);
    if(kind==11&&(method==4||method==5)){  // state block Capture / Apply (~120-150 a frame)
        Object* block=cachedObject(args[0]);if(!block||block->kind!=Kind::StateBlock)return false;
        if(method==5)forgetShadow();  // Apply changes device state behind the shadow
        {   // the viewport: unchanged by a block without one, restored by one that has it
            auto vb=blockViewports.find(args[0]);
            if(method==4&&vb!=blockViewports.end()&&vb->second.has){vb->second.known=viewportKnown;vb->second.value=viewportShadow;}
            if(method==5){
                if(vb==blockViewports.end())viewportKnown=false;
                else if(vb->second.has){viewportKnown=vb->second.known;viewportShadow=vb->second.value;}
            }
        }
        Cmd c{};c.op=OpStateBlock;c.method=static_cast<uint16_t>(method);c.obj=block->native;*count=1;push(c);return true;
    }
    // AddRef/Release: the guest-visible count is the bridge's own (see dispatchGuestD3D9Inner); only the
    // native count change is queued. Releases that retire the guest wrapper run synchronously.
    if(method==1||method==2){
        Object* op=cachedObject(args[0]);if(!op||static_cast<uint32_t>(op->kind)!=kind)return false;
        Object& o=*op;
        if(method==2&&o.references<=1){
            // Buffers, textures without surface wrappers, and surfaces (Alien Shooter 2 frees ~2 of each a
            // frame while it streams its ground tiles). A surface of a texture retires its parent's wrapper
            // too when that was the last one (the parent's native reference went with its own last Release).
            const bool kindOk=kind==6||kind==7||kind==5||(kind==3&&!o.parent);
            if(o.references!=1||!kindOk||o.children||!asyncBuffersOn())return false;
            flushRefDeltas();                       // o may have a pending count change (refDirty points at it)
            IUnknown* native=o.native;const uint32_t parent=o.parent;
            if(kind==5)plainSurfaces.erase(args[0]);
            retireGuestObject(args[0]);             // erases o and its buffer shadow; bumps handlesRetiredEpoch
            Cmd c{};c.op=OpRelease;c.obj=native;push(c);
            if(parent){
                std::lock_guard<std::recursive_mutex> lock(objectsMutex);
                auto owner=objects.find(parent);
                if(owner!=objects.end()&&!--owner->second.children&&!owner->second.references)retireGuestObject(parent);
            }
            *count=1;*result=0;++statAsyncReleases;return true;
        }
        if(!o.refQueued){o.refQueued=true;refDirty.push_back(&o);}
        if(method==1){*count=1;++o.refDelta;*result=++o.references;return true;}
        *count=1;--o.refDelta;*result=--o.references;return true;
    }
    return false;
}
// ---- HUD at half rate (NFSU2_HUD_HALF, src/runtime/hud_half.c) ----
// While driving, the game's front-end/HUD renderer (sub_005361B0: ~200 quads, ~8% of the game thread)
// runs every other frame. On the frames it runs, every D3D9 call it makes is recorded (method, arguments
// and the bytes they point at; the shadow is forgotten first so no state call is filtered out of the
// recording); on the frames in between the recording is replayed instead. A recording that contains
// anything else (synchronous calls, locks, state blocks) is not replayed, nor one whose objects were released
// since (checked when handlesRetiredEpoch moved): the HUD then simply runs.
bool hudRecording=false;
struct HudCall {uint32_t kind,method,a[9],off[2],len[2],arg[2];};
struct HudRec {std::vector<HudCall> calls;std::vector<uint8_t> blob;std::vector<std::pair<uint32_t,IUnknown*>> objs;bool valid=false;uint64_t epoch=0;};
HudRec hudRec;
uint32_t hudScratch=0,hudScratchSize=0;
void hudRecordInvalidate(){hudRec.valid=false;}
void hudRecordBegin(){hudRec.calls.clear();hudRec.blob.clear();hudRec.objs.clear();hudRec.valid=true;hudRec.epoch=handlesRetiredEpoch;forgetShadow();hudRecording=true;}
void hudRecordEnd(){hudRecording=false;}
static void hudData(HudCall& c,int slot,uint32_t argIndex,size_t bytes){
    c.arg[slot]=argIndex;c.len[slot]=0;c.off[slot]=0;
    if(!c.a[argIndex]||!bytes)return;
    if(bytes>(1u<<20)){hudRec.valid=false;return;}
    c.off[slot]=static_cast<uint32_t>(hudRec.blob.size());c.len[slot]=static_cast<uint32_t>(bytes);
    const uint8_t* src=static_cast<const uint8_t*>(pointer(c.a[argIndex]));hudRec.blob.insert(hudRec.blob.end(),src,src+bytes);
}
void hudRecordCall(uint32_t kind,uint32_t method,const uint32_t* args,bool deferred){
    if(!hudRec.valid)return;
    if(!deferred){hudRec.valid=false;return;}
    if(kind!=2){
        if(method==1||method==2)return;                       // AddRef/Release pairs: no device effect
        if((kind==5&&method==12)||((kind==3||kind==4)&&method==17))return;  // descriptions
        hudRec.valid=false;return;
    }
    HudCall c{};c.kind=kind;c.method=method;for(int i=0;i<9;++i)c.a[i]=args[i];c.arg[0]=c.arg[1]=99;
    // Objects the calls refer to: checked at replay instead of handlesRetiredEpoch (which changes whenever the
    // world streams a texture in or out: the HUD then replayed on only ~40% of freeroam frames).
    auto obj=[&](uint32_t h){if(!h)return;Object* o=cachedObject(h);if(!o){hudRec.valid=false;return;}hudRec.objs.push_back({h,o->native});};
    switch(method){case 65:case 100:case 37:obj(args[2]);break;case 87:case 92:case 107:case 104:case 39:obj(args[1]);break;default:break;}
    switch(method){
    case 17:case 3:hudRec.valid=false;return;
    case 43:hudData(c,0,2,size_t(args[1])*sizeof(D3DRECT));break;
    case 44:case 46:hudData(c,0,2,sizeof(D3DMATRIX));break;
    case 47:hudData(c,0,1,sizeof(D3DVIEWPORT9));break;
    case 49:hudData(c,0,1,sizeof(D3DMATERIAL9));break;
    case 51:hudData(c,0,2,sizeof(D3DLIGHT9));break;
    case 55:hudData(c,0,2,16);break;
    case 75:hudData(c,0,1,sizeof(RECT));break;
    case 83:hudData(c,0,3,size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[2]))*args[4]);break;
    case 84:hudData(c,0,5,size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[4]))*(args[6]==D3DFMT_INDEX16?2:4));
            hudData(c,1,7,size_t(args[2]+args[3])*args[8]);break;
    case 94:case 109:case 96:case 111:hudData(c,0,2,size_t(args[3])*16);break;
    case 98:case 113:hudData(c,0,2,size_t(args[3])*4);break;
    default:break;
    }
    hudRec.calls.push_back(c);
}
// ---- Generic call capture (src/runtime/world_cull.c: per-item fast path learning) ----
// Records every D3D9 call reaching tryDefer between nfsCapBegin and nfsCapEnd, with the bytes pointer
// arguments refer to (shader constants, matrices, user-pointer vertices). A synchronous call makes the
// capture invalid.
struct CapRec {uint32_t kind,method,a[9],off[2],len[2],arg[2];};
bool capActive=false,capBad=false;
std::vector<CapRec> capRecs;std::vector<uint8_t> capBytes;
static void capData(CapRec& c,int slot,uint32_t argIndex,size_t bytes){
    c.arg[slot]=argIndex;c.len[slot]=0;c.off[slot]=0;
    if(!c.a[argIndex]||!bytes)return;
    if(bytes>(1u<<20)){capBad=true;return;}
    c.off[slot]=static_cast<uint32_t>(capBytes.size());c.len[slot]=static_cast<uint32_t>(bytes);
    const uint8_t* src=static_cast<const uint8_t*>(pointer(c.a[argIndex]));capBytes.insert(capBytes.end(),src,src+bytes);
}
static void capRecord(uint32_t kind,uint32_t method,const uint32_t* args,bool ok){
    if(capBad)return;
    if(!ok){capBad=true;return;}
    if(kind!=2){if(method==1||method==2)return;if((kind==5&&method==12)||((kind==3||kind==4)&&method==17))return;capBad=true;return;}
    CapRec c{};c.kind=kind;c.method=method;for(int i=0;i<9;++i)c.a[i]=args[i];c.arg[0]=c.arg[1]=99;
    switch(method){
    case 17:case 3:capBad=true;return;
    case 43:capData(c,0,2,size_t(args[1])*sizeof(D3DRECT));break;
    case 44:case 46:capData(c,0,2,sizeof(D3DMATRIX));break;
    case 47:capData(c,0,1,sizeof(D3DVIEWPORT9));break;
    case 49:capData(c,0,1,sizeof(D3DMATERIAL9));break;
    case 51:capData(c,0,2,sizeof(D3DLIGHT9));break;
    case 55:capData(c,0,2,16);break;
    case 75:capData(c,0,1,sizeof(RECT));break;
    case 83:capData(c,0,3,size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[2]))*args[4]);break;
    case 84:capData(c,0,5,size_t(vertexCount(static_cast<D3DPRIMITIVETYPE>(args[1]),args[4]))*(args[6]==D3DFMT_INDEX16?2:4));
            capData(c,1,7,size_t(args[2]+args[3])*args[8]);break;
    case 94:case 109:case 96:case 111:capData(c,0,2,size_t(args[3])*16);break;
    case 98:case 113:capData(c,0,2,size_t(args[3])*4);break;
    default:break;
    }
    capRecs.push_back(c);
}
// ---- Pass state watch (src/runtime/hud_half.c, lazy effect passes) ----
// Mode 1 (during a real BeginPass): remember which device states the pass sets (a 4096-bit set of hashed
// keys: render state, texture stage, sampler, texture stage, shader, each constant register, ...).
// Mode 2 (while an EndPass is deferred): any call that sets one of those states, any synchronous call or
// lock, and Present mark a conflict, so the next BeginPass is a real one. Hash collisions only cause
// extra real BeginPasses.
int passMode=0;bool passConflict=false;uint64_t passKeys[64];
static inline void passMark(uint32_t m,uint32_t a,uint32_t b){
    const uint32_t k=(m*0x9E3779B1u^a*0x85EBCA6Bu^b*0xC2B2AE35u)>>20;
    if(passMode==1)passKeys[k>>6]|=1ull<<(k&63);
    else if(passKeys[k>>6]&(1ull<<(k&63)))passConflict=true;
}
void passNote(uint32_t kind,uint32_t method,const uint32_t* args,bool ok){
    if(kind!=2){
        if(method==1||method==2)return;
        if((kind==5&&method==12)||((kind==3||kind==4)&&method==17))return;
        if(passMode==2)passConflict=true;
        return;
    }
    if(!ok){if(passMode==2)passConflict=true;return;}
    switch(method){
    case 81:case 82:case 83:case 84:case 43:return;                 // draws, Clear
    case 17:case 3:if(passMode==2)passConflict=true;return;          // Present
    case 94:case 109:case 96:case 111:case 98:case 113:
        for(uint32_t r=0;r<args[3]&&r<256;++r)passMark(method,args[1]+r,0);return;
    case 57:case 65:case 44:case 46:passMark(method,args[1],0);return;
    case 67:case 69:passMark(method,args[1],args[2]);return;
    case 87:case 89:passMark(87,0,0);return;                         // declaration / FVF
    default:passMark(method,0,0);return;
    }
}
bool tryDefer(uint32_t kind,uint32_t method,const uint32_t* args,uint32_t* count,uint32_t* result){
    const bool ok=tryDeferImpl(kind,method,args,count,result);
    if(passMode)passNote(kind,method,args,ok);
    if(hudRecording)hudRecordCall(kind,method,args,ok);
    if(capActive)capRecord(kind,method,args,ok);
    return ok;
}
bool hudReplay(){
    if(!hudRec.valid||hudRec.calls.empty())return false;
    const uint64_t curEpoch=handlesRetiredEpoch.load(std::memory_order_acquire);
    if(hudRec.epoch!=curEpoch){
        std::lock_guard<std::recursive_mutex> lock(objectsMutex);
        for(const auto& o:hudRec.objs){auto f=objects.find(o.first);if(f==objects.end()||f->second.native!=o.second)return false;}
        hudRec.epoch=curEpoch;  // all still alive: no need to check again until the next change
    }
    if(hudRec.blob.size()>hudScratchSize){
        if(hudScratch)freeGuest(hudScratch);
        hudScratchSize=static_cast<uint32_t>(hudRec.blob.size()*2+4096);hudScratch=allocateGuest(hudScratchSize);
        if(!hudScratch){hudScratchSize=0;return false;}
    }
    if(!hudRec.blob.empty())std::memcpy(pointer(hudScratch),hudRec.blob.data(),hudRec.blob.size());
    forgetShadow();
    for(const HudCall& c:hudRec.calls){
        uint32_t a[12]={};for(int i=0;i<9;++i)a[i]=c.a[i];
        for(int k=0;k<2;++k)if(c.arg[k]<9&&c.len[k])a[c.arg[k]]=hudScratch+c.off[k];
        uint32_t count=0,result=0;tryDeferImpl(c.kind,c.method,a,&count,&result);
    }
    return true;
}
}  // namespace rt
// Native D3DX effect engine (src/runtime/fx_cache.c): a device method call (args[0] = the device handle),
// queued or filtered without the guest COM dispatch layers. 0 = must take the regular (synchronous) path.
extern "C" int nfsD3dDeviceCall(uint32_t method,const uint32_t* args,uint32_t* result){
    if(!rt::enabled)return 0;
    uint32_t a[12]={};std::memcpy(a,args,4*sizeof(uint32_t));uint32_t count=0;
    return rt::tryDefer(2,method,a,&count,result)?1:0;
}
// Pass state watch (hud_half.c): 1 record the states a BeginPass sets, 2 watch them, 0 off.
// 3: watch again after a pause, keeping a conflict already seen.
extern "C" void nfsPassWatch(int mode){
    if(mode==1){std::memset(rt::passKeys,0,sizeof rt::passKeys);}
    if(mode==2)rt::passConflict=false;
    if(mode==3)mode=2;
    rt::passMode=rt::enabled?mode:0;
}
extern "C" int nfsPassConflict(){return rt::passConflict||!rt::enabled?1:0;}
// Device calls queued so far (world_cull.c: commit-run verification).
extern "C" uint64_t nfsD3dPushedCount(){return rt::statPushed;}
extern "C" void nfsCapBegin(){rt::capRecs.clear();rt::capBytes.clear();rt::capBad=false;rt::capActive=rt::enabled;}
extern "C" int nfsCapEnd(){rt::capActive=false;return rt::capBad||!rt::enabled?-1:static_cast<int>(rt::capRecs.size());}
extern "C" const void* nfsCapRecs(){return rt::capRecs.data();}
extern "C" const uint8_t* nfsCapBytes(){return rt::capBytes.data();}
extern "C" uint32_t nfsCapBytesSize(){return static_cast<uint32_t>(rt::capBytes.size());}
// Any deferrable call by kind/method (args[0] = the object handle); 0 = not deferrable here.
// ---- Shortcuts for device calls from the lifted code ----
// Every D3D9 call from the game took the generic path: the arguments copied off the guest stack, the method decoded
// from the call address, the recording hooks checked one by one, the call dispatched by kind and method. The call-site
// cache (arm64_guest.h) keeps one handler per method address, so guest_runtime.cpp gives each device method its own
// handler, which passes the guest stack straight here. The recording hooks run as tryDefer runs them (pass watch, HUD
// recording, capture, in that order, for deferred calls); a device handle not seen yet goes the generic way.
// NFSU2_D3D_SHORTCUTS=0: generic path only.
extern "C" int nfsD3dShortcutsEnabled(){static const bool on=[]{const char* e=std::getenv("NFSU2_D3D_SHORTCUTS");return !(e&&e[0]=='0');}();return on;}
// AddRef/Release of textures and buffers (D3DX changes a texture's count every time an effect sets it: ~1500 calls
// a frame): the count change only, as tryDeferImpl does. A Release that would retire the object goes the generic way.
extern "C" int nfsD3dReferenceShortcut(uint32_t kind,uint32_t method,const uint32_t* args,uint32_t* result){
    using namespace rt;
    if(!enabled)return 0;
    Object* object=cachedObject(args[0]);if(!object||static_cast<uint32_t>(object->kind)!=kind)return 0;
    if(method==2&&object->references<=1)return 0;
    if(!object->refQueued){object->refQueued=true;refDirty.push_back(object);}
    if(method==1){++object->refDelta;*result=++object->references;}else{--object->refDelta;*result=--object->references;}
    if(passMode)passNote(kind,method,args,true);
    if(hudRecording)hudRecordCall(kind,method,args,true);
    if(capActive)capRecord(kind,method,args,true);
    return 1;
}
// One instanced draw for consecutive replayed world items with the same mesh (src/runtime/world_cull.c replayGroup):
// dip = the DrawIndexedPrimitive arguments after the device, registers = the vertex-shader registers that follow the
// world matrix, data = instances x registers vec4. The filter's shadow ends with the last instance's values, as the
// device's register file does. 0 = not queued (render thread off, or something is being recorded): draw them singly.
extern "C" int nfsD3dDrawInstanced(const uint32_t* dip,uint32_t registerCount,const uint16_t* registers,uint32_t instances,const float* data){
    using namespace rt;
    static const bool off=[]{const char* e=std::getenv("NFSU2_INSTANCING");return e&&e[0]=='0';}();
    if(off||!enabled||!shadow||!deviceNative||passMode||hudRecording||capActive||!registerCount||registerCount>16||instances<2||instances>64)return 0;
    const size_t dataBytes=size_t(instances)*registerCount*16,payloadBytes=instanceDataOffset(registerCount)+dataBytes;
    std::vector<uint8_t>& payload=instancePayload;payload.assign(payloadBytes,0);
    std::memcpy(payload.data(),registers,size_t(registerCount)*2);
    std::memcpy(payload.data()+instanceDataOffset(registerCount),data,dataBytes);
    Cmd c{};c.op=OpInstancedDraw;c.obj=deviceNative;
    for(int i=0;i<6;++i)c.a[i]=dip[i];
    c.a[6]=registerCount;c.a[7]=instances;c.p0=copyData(payload.data(),payloadBytes);
    for(uint32_t j=0;j<registerCount;++j){
        const uint32_t reg=registers[j];if(reg>=256)continue;
        std::memcpy(shadow->vsc[reg],data+(size_t(instances-1)*registerCount+j)*4,16);shadow->vscValid[reg]=true;
    }
    ++statPushed;push(c);return 1;
}
extern "C" int nfsD3dDeviceCallShortcut(uint32_t method,const uint32_t* args,uint32_t* count){
    using namespace rt;
    if(!enabled||!shadow)return 0;
    if(args[0]!=deviceHandle||deviceHandleEpoch!=handlesRetiredEpoch.load(std::memory_order_acquire))return 0;
    if(method==17||method==23||method==26||method==27)return 0;   // Present and the creations: generic path
    if(!queueDeviceCall(method,args,count,deviceNative))return 0;
    if(passMode)passNote(2,method,args,true);
    if(hudRecording)hudRecordCall(2,method,args,true);
    if(capActive)capRecord(2,method,args,true);
    return 1;
}
extern "C" int nfsD3dCall(uint32_t kind,uint32_t method,const uint32_t* args){
    if(!rt::enabled)return 0;
    uint32_t a[12]={};std::memcpy(a,args,9*sizeof(uint32_t));uint32_t count=0,result=0;
    return rt::tryDefer(kind,method,a,&count,&result)?1:0;
}
extern "C" void nfsHudRecordBegin(){if(rt::enabled)rt::hudRecordBegin();}
extern "C" void nfsHudRecordEnd(){rt::hudRecordEnd();}
extern "C" int nfsHudReplay(){return rt::enabled&&rt::hudReplay()?1:0;}
#endif
