// FlatOut 2: the Win32 imports of FlatOut2.exe that neither the NFSU2 runtime nor as2_imports.inc has.
// Mostly answers that steer the game away from what the handheld does not have (WinMM wave devices, CD
// audio, network adapters, ACM codecs) or bookkeeping (keyboard hooks, time). Included after fo2_msvcrt.inc;
// handlers are stdcall: ret(value, argument count).

namespace fo2 {

// --- time -----------------------------------------------------------------------------------------------
void writeSystemTime(uint32_t out,const std::tm& t,unsigned milliseconds){
    const uint16_t fields[8]={uint16_t(t.tm_year+1900),uint16_t(t.tm_mon+1),uint16_t(t.tm_wday),uint16_t(t.tm_mday),
                              uint16_t(t.tm_hour),uint16_t(t.tm_min),uint16_t(t.tm_sec),uint16_t(milliseconds)};
    std::memcpy(ptr(out,16),fields,16);
}
void systemTime(bool local){
    auto now=std::chrono::system_clock::now();std::time_t t=std::chrono::system_clock::to_time_t(now);
    unsigned ms=unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()%1000);
    std::tm parts{};if(local)localtime_r(&t,&parts);else gmtime_r(&t,&parts);
    writeSystemTime(arg(0),parts,ms);
}
void getSystemTime(){systemTime(false);ret(0,1);}
void getLocalTime(){systemTime(true);ret(0,1);}
void systemTimeToFileTime(){  // SYSTEMTIME (UTC) -> 100 ns ticks since 1601
    uint16_t f[8];std::memcpy(f,ptr(arg(0),16),16);
    std::tm parts{};parts.tm_year=f[0]-1900;parts.tm_mon=f[1]-1;parts.tm_mday=f[3];parts.tm_hour=f[4];parts.tm_min=f[5];parts.tm_sec=f[6];
    const int64_t seconds=int64_t(timegm(&parts));
    const uint64_t ticks=uint64_t(seconds+11644473600ll)*10000000ull+uint64_t(f[7])*10000ull;
    std::memcpy(ptr(arg(1),8),&ticks,8);ret(1,2);
}
void getUserDefaultLangID(){ret(0x409,0);}

// --- kernel32 odds and ends -----------------------------------------------------------------------------
void peekNamedPipe(){lastError=6;ret(0,6);}
void fileInformationByHandle(){lastError=1;ret(0,2);}
void virtualLock(){ret(1,2);}
// GlobalAlloc / GlobalFree on the process heap; the exe imports no GlobalLock, so a handle is the pointer
void globalAlloc(){ret(heaps->alloc(GuestHeaps::process,(arg(0)&0x40)?8:0,std::max<uint32_t>(arg(1),1)),2);}  // GMEM_ZEROINIT
void globalFree(){if(arg(0))heaps->free(GuestHeaps::process,arg(0));ret(0,1);}
void openFile(){  // (name, OFSTRUCT*, style): only OF_EXIST (0x4000) probes are expected
    const auto path=gamePath(guestString(arg(0)));std::error_code error;
    if(arg(2)&0x4000){ret(std::filesystem::exists(path,error)?1:uint32_t(-1),3);return;}
    logOnce("OpenFile (not OF_EXIST)");lastError=2;ret(uint32_t(-1),3);
}
void windowsDirectory(const char* dir){ret(as2::putString(arg(0),arg(1),dir)-1,2);}
void getSystemDirectoryA(){windowsDirectory("C:\\WINDOWS\\system32");}
void getWindowsDirectoryA(){windowsDirectory("C:\\WINDOWS");}
void getTempPathA(){ret(as2::putString(arg(1),arg(0),"C:\\nfsu2\\Savegame\\")-1,2);}
void deviceIoControl(){lastError=1;ret(0,8);}
void isBadCodePtr(){ret(0,1);}
void terminateThread(){logOnce("TerminateThread");ret(1,2);}
void expandEnvironmentStringsA(){ret(as2::putString(arg(1),arg(2),guestString(arg(0))),3);}
void isDBCSLeadByte(){ret(0,1);}
void disableThreadLibraryCalls(){ret(1,1);}
void formatMessageA(){lastError=317;ret(0,7);}

// --- user32 ---------------------------------------------------------------------------------------------
uint32_t nextImageHandle=0x00f10000u;
void loadImageA(){ret(nextImageHandle++,6);}
void charLowerBuffA(){
    for(uint32_t i=0;i<arg(1);++i){char* c=static_cast<char*>(ptr(arg(0)+i,1));*c=char(std::tolower(static_cast<unsigned char>(*c)));}
    ret(arg(1),2);
}
void toUnicode(){  // (vk, scan, state[256], out, size, flags): printable ASCII keys only
    const uint32_t vk=arg(0);
    if(arg(4)&&((vk>=0x30&&vk<=0x39)||(vk>=0x41&&vk<=0x5a)||vk==0x20)){
        const bool shift=(*static_cast<uint8_t*>(ptr(arg(2)+0x10,1))&0x80)!=0;
        as2::write16(arg(3),uint16_t(vk>=0x41&&vk<=0x5a&&!shift?vk+32:vk));ret(1,6);return;
    }
    ret(0,6);
}
void getKeyState(){ret(0,1);}
uint32_t nextHook=0x00f20000u;
// (idHook, proc, module, thread): FlatOut 2 installs a WH_KEYBOARD (2) hook on its own thread (0x55A816,
// procedure 0x55AAD0) and takes all its keys from it; PeekMessage / GetMessage call it (guest_window.inc).
// Other hook kinds are accepted and never called.
uint32_t keyboardHookHandle{};
void setWindowsHookExA(){
    logOnce("SetWindowsHookExA");uint32_t handle=nextHook++;
    if(arg(0)==2&&arg(1)){keyboardHookProc=arg(1);keyboardHookHandle=handle;}
    ret(handle,4);
}
void unhookWindowsHookEx(){if(arg(0)&&arg(0)==keyboardHookHandle){keyboardHookProc=0;keyboardHookHandle=0;}ret(1,1);}
void callNextHookEx(){ret(0,4);}
void getKeyboardLayout(){ret(0x04090409u,1);}
void getClassNameA(){ret(as2::putString(arg(1),arg(2),"FlatOut2")-1,3);}
void unregisterClassA(){ret(1,2);}
void getTitleBarInfo(){lastError=1;ret(0,2);}
void sendDlgItemMessageW(){ret(0,5);}

// --- advapi32, ole32 ------------------------------------------------------------------------------------
void regCreateKeyExA(){  // the game's settings key: opened like RegOpenKeyEx, reported as existing
    if(arg(8))write32(arg(8),2);   // REG_OPENED_EXISTING_KEY
    registryOpen(arg(7),9);
}
void regEnumKeyA(){ret(259,4);}   // ERROR_NO_MORE_ITEMS
void coInitializeEx(){ret(0,2);}
void clsidFromString(){  // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (UTF-16)
    std::string s;for(uint32_t i=0;i<38;++i)s+=char(as2::read16(arg(0)+i*2)&0x7f);
    unsigned d1,d2,d3,b[8];
    if(std::sscanf(s.c_str(),"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",&d1,&d2,&d3,&b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7])!=11){ret(0x800401f3u,2);return;}
    write32(arg(1),d1);as2::write16(arg(1)+4,uint16_t(d2));as2::write16(arg(1)+6,uint16_t(d3));
    for(int i=0;i<8;++i)*static_cast<uint8_t*>(ptr(arg(1)+8+i,1))=uint8_t(b[i]);
    ret(0,2);
}

// --- winmm: no wave devices (FMOD then uses DirectSound), no CD audio ----------------------------------
void waveGetNumDevs(){ret(0,0);}
void waveNoDriver1(){ret(6,1);}   // MMSYSERR_NODRIVER
void waveNoDriver2(){ret(6,2);}
void waveNoDriver3(){ret(6,3);}
void waveNoDriver6(){ret(6,6);}
void mciSendCommandA(){logOnce("mciSendCommandA");ret(0x113,4);}   // MCIERR_DEVICE_NOT_INSTALLED-like failure

// --- networking: none ------------------------------------------------------------------------------------
void socketError(uint32_t argc){lastError=10050;ret(uint32_t(-1),argc);}   // WSAENETDOWN
void wsaFail1(){socketError(1);}void wsaFail2(){socketError(2);}void wsaFail4(){socketError(4);}
void wsaFail5(){socketError(5);}void wsaFail7(){socketError(7);}void wsaFail9(){socketError(9);}
// LAN play: the game's overlapped UDP calls, over the non-blocking sockets of guest_network.inc. A send completes at once;
// a receive with nothing to read is queued and looked at again whenever the game waits or asks for a result.
// Not finished: a console that receives a packet crashes (docs/NOTES.md), so these calls fail as before unless FO2_LAN=1.
namespace wsaov {
bool lanOn(){static const bool on=[]{const char* e=std::getenv("FO2_LAN");return e&&e[0]=='1';}();return on;}
std::map<uint32_t,bool> events;
uint32_t nextEvent=0x00f30001u;
struct PendingRecv{uint32_t socket,buffers,count,flags,from,fromLength,overlapped;};
std::vector<PendingRecv> pending;
void complete(uint32_t overlapped,uint32_t bytes,uint32_t status){
    if(!overlapped)return;
    write32(overlapped,status);write32(overlapped+4,bytes);
    auto event=events.find(read32(overlapped+16));if(event!=events.end())event->second=true;
}
// 1 a datagram was read, 0 nothing to read, -1 error
int receive(const net::Socket& socket,uint32_t buffers,uint32_t count,uint32_t flags,uint32_t from,uint32_t fromLength,uint32_t& bytes){
    static std::vector<uint8_t> message(65536);
    sockaddr_storage peer{};socklen_t peerLength=sizeof peer;
    const ssize_t got=::recvfrom(socket.fd,message.data(),message.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&peer),&peerLength);
    if(got<0)return errno==EAGAIN||errno==EWOULDBLOCK?0:-1;
    size_t done=0;
    for(uint32_t i=0;i<count&&done<size_t(got);++i){
        const uint32_t length=read32(buffers+i*8),data=read32(buffers+i*8+4);
        const size_t part=std::min<size_t>(length,size_t(got)-done);
        if(part)std::memcpy(ptr(data,uint32_t(part)),message.data()+done,part);
        done+=part;
    }
    bytes=uint32_t(done);
    if(from&&fromLength)net::storeAddress(from,fromLength,peer,std::min<socklen_t>(peerLength,16));
    if(flags)write32(flags,0);
    return 1;
}
void poll(){
    for(size_t i=0;i<pending.size();){
        const PendingRecv op=pending[i];
        auto* socket=net::find(op.socket);
        uint32_t bytes=0;
        const int result=socket?receive(*socket,op.buffers,op.count,op.flags,op.from,op.fromLength,bytes):-1;
        if(result==0){++i;continue;}
        if(result>0)complete(op.overlapped,bytes,0);else complete(op.overlapped,0,0xC0000120u);   // STATUS_CANCELLED
        pending.erase(pending.begin()+long(i));
    }
}
bool isPending(uint32_t overlapped){for(const auto& op:pending)if(op.overlapped==overlapped)return true;return false;}
void pause(){net::released([]{::usleep(2000);return 0;});checkGuestProgress();}
}
void wsaCreateEvent(){const uint32_t handle=wsaov::nextEvent++;wsaov::events[handle]=false;ret(handle,0);}
void wsaResetEvent(){auto event=wsaov::events.find(arg(0));if(event!=wsaov::events.end())event->second=false;ret(1,1);}
void wsaCloseEvent(){wsaov::events.erase(arg(0));ret(1,1);}
void wsaSendTo(){   // (socket, WSABUF*, count, sent*, flags, to, toLength, overlapped, completion)
    if(!wsaov::lanOn()){socketError(9);return;}
    auto* socket=net::find(arg(0));if(!socket){ret(net::fail(net::wsaEnotsock),9);return;}
    std::vector<uint8_t> message;
    for(uint32_t i=0;i<arg(2);++i){
        const uint32_t length=read32(arg(1)+i*8),data=read32(arg(1)+i*8+4);
        if(length){const auto* bytes=static_cast<const uint8_t*>(ptr(data,length));message.insert(message.end(),bytes,bytes+length);}
    }
    sockaddr_storage address;socklen_t addressLength=0;
    if(arg(5)){addressLength=net::guestAddress(arg(5),arg(6),address);if(!addressLength){ret(net::fail(net::wsaEfault),9);return;}}
    static int shown=0;if(shown<40&&addressLength){++shown;std::printf("[FO2 NET] send %zu bytes to %s\n",message.size(),net::describe(address).c_str());}
    const ssize_t sent=::sendto(socket->fd,message.data(),message.size(),MSG_DONTWAIT|MSG_NOSIGNAL,addressLength?reinterpret_cast<sockaddr*>(&address):nullptr,addressLength);
    if(sent<0){ret(net::failErrno(),9);return;}
    if(arg(3))write32(arg(3),uint32_t(sent));
    wsaov::complete(arg(7),uint32_t(sent),0);
    ret(0,9);
}
void wsaRecvFrom(){ // (socket, WSABUF*, count, received*, flags*, from, fromLength*, overlapped, completion)
    if(!wsaov::lanOn()){socketError(9);return;}
    auto* socket=net::find(arg(0));if(!socket){ret(net::fail(net::wsaEnotsock),9);return;}
    uint32_t bytes=0;
    const int result=wsaov::receive(*socket,arg(1),arg(2),arg(4),arg(5),arg(6),bytes);
    if(result<0){ret(net::failErrno(),9);return;}
    if(result>0){if(arg(3))write32(arg(3),bytes);wsaov::complete(arg(7),bytes,0);ret(0,9);return;}
    if(!arg(7)){ret(net::fail(net::wsaEwouldblock),9);return;}
    wsaov::pending.push_back({arg(0),arg(1),arg(2),arg(4),arg(5),arg(6),arg(7)});
    ret(net::fail(997),9);  // WSA_IO_PENDING
}
void wsaWaitForMultipleEvents(){    // (count, events*, waitAll, timeout, alertable)
    if(!wsaov::lanOn()){socketError(5);return;}
    const uint32_t count=arg(0),list=arg(1),all=arg(2),timeout=arg(3);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    for(;;){
        wsaov::poll();
        uint32_t ready=0,first=count;
        for(uint32_t i=0;i<count;++i){
            auto event=wsaov::events.find(read32(list+i*4));
            if(event!=wsaov::events.end()&&event->second){++ready;if(first==count)first=i;}
        }
        if(all?ready==count:ready>0){ret(all?0:first,5);return;}
        if(timeout!=0xffffffffu&&std::chrono::steady_clock::now()>=deadline){ret(258,5);return;}   // WSA_WAIT_TIMEOUT
        wsaov::pause();
    }
}
void wsaGetOverlappedResult(){  // (socket, overlapped, bytes*, wait, flags*)
    if(!wsaov::lanOn()){socketError(5);return;}
    const uint32_t overlapped=arg(1);
    for(;;){
        wsaov::poll();
        if(!wsaov::isPending(overlapped))break;
        if(!arg(3)){lastError=996;ret(0,5);return;}   // WSA_IO_INCOMPLETE
        wsaov::pause();
    }
    if(arg(2))write32(arg(2),read32(overlapped+4));
    if(arg(4))write32(arg(4),0);
    if(read32(overlapped)){lastError=995;ret(0,5);return;}   // WSA_OPERATION_ABORTED
    ret(1,5);
}
void wsaStringToAddress(){  // (text, family, protocol info, sockaddr*, length*)
    if(!wsaov::lanOn()){socketError(5);return;}
    unsigned a,b,c,d,port=0;const std::string text=guestString(arg(0));
    const int fields=std::sscanf(text.c_str(),"%u.%u.%u.%u:%u",&a,&b,&c,&d,&port);
    if(fields<4||arg(1)!=2||!arg(3)||!arg(4)||read32(arg(4))<16){ret(net::fail(net::wsaEinval),5);return;}
    uint8_t out[16]={2,0,uint8_t(port>>8),uint8_t(port),uint8_t(a),uint8_t(b),uint8_t(c),uint8_t(d)};
    std::memcpy(ptr(arg(3),16),out,16);write32(arg(4),16);ret(0,5);
}
void inetAddr(){
    unsigned a,b,c,d;const std::string s=guestString(arg(0));
    if(std::sscanf(s.c_str(),"%u.%u.%u.%u",&a,&b,&c,&d)!=4){ret(0xffffffffu,1);return;}
    ret(a|(b<<8)|(c<<16)|(d<<24),1);
}
void inetNtoa(){
    static uint32_t buffer=0;if(!buffer)buffer=allocate(16);
    const uint32_t v=arg(0);char text[16];std::snprintf(text,sizeof text,"%u.%u.%u.%u",v&255,(v>>8)&255,(v>>16)&255,v>>24);
    std::memcpy(ptr(buffer,16),text,16);ret(buffer,1);
}
void wsafdIsSet(){  // (socket, fd_set*)
    const uint32_t count=read32(arg(1));
    for(uint32_t i=0;i<count&&i<64;++i)if(read32(arg(1)+4+i*4)==arg(0)){ret(1,2);return;}
    ret(0,2);
}
// GetAdaptersInfo(IP_ADAPTER_INFO*, size*): the first network interface that is up, not loopback, with an IPv4 address.
void getAdaptersInfo(){
    constexpr uint32_t size=640;
    if(!wsaov::lanOn()){ret(232,2);return;}   // ERROR_NO_DATA
    if(!arg(1)){ret(87,2);return;}
    ifaddrs* list=nullptr;const ifaddrs* found=nullptr;
    if(::getifaddrs(&list)==0)for(const ifaddrs* i=list;i;i=i->ifa_next)
        if(i->ifa_addr&&i->ifa_addr->sa_family==AF_INET&&(i->ifa_flags&IFF_UP)&&!(i->ifa_flags&IFF_LOOPBACK)){found=i;break;}
    if(!found){if(list)::freeifaddrs(list);ret(232,2);return;}   // ERROR_NO_DATA
    if(!arg(0)||read32(arg(1))<size){write32(arg(1),size);::freeifaddrs(list);ret(111,2);return;}   // ERROR_BUFFER_OVERFLOW
    auto* out=static_cast<char*>(ptr(arg(0),size));std::memset(out,0,size);
    std::snprintf(out+8,260,"%s",found->ifa_name);std::snprintf(out+268,132,"%s",found->ifa_name);
    write32(arg(0)+400,6);
    int query=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);ifreq request{};std::snprintf(request.ifr_name,IFNAMSIZ,"%s",found->ifa_name);
    if(query>=0){if(::ioctl(query,SIOCGIFHWADDR,&request)==0)std::memcpy(out+404,request.ifr_hwaddr.sa_data,6);::close(query);}
    write32(arg(0)+412,1);write32(arg(0)+416,6);write32(arg(0)+424,arg(0)+428);
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(found->ifa_addr)->sin_addr,text,sizeof text);std::snprintf(out+432,16,"%s",text);
    if(found->ifa_netmask){inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(found->ifa_netmask)->sin_addr,text,sizeof text);std::snprintf(out+448,16,"%s",text);}
    std::printf("[FO2 NET] adapter %s\n",found->ifa_name);
    ::freeifaddrs(list);
    ret(0,2);
}

// --- msacm32: no codecs (only PCM sounds play) ----------------------------------------------------------
void acmFail(uint32_t argc){logOnce("acm (no codec)");ret(512,argc);}   // ACMERR_NOTPOSSIBLE
void acmFail2(){acmFail(2);}void acmFail3(){acmFail(3);}void acmFail4(){acmFail(4);}void acmFail5(){acmFail(5);}void acmFail8(){acmFail(8);}

// --- dsound.dll: FMOD 3 counts its output drivers with DirectSoundEnumerateA before DirectSoundCreate;
// with none it had no driver name and copied one from NULL. One driver: the default device (NULL GUID).
void directSoundEnumerateA(){  // (LPDSENUMCALLBACKA callback, context)
    static uint32_t description=0,module=0;
    if(!description){description=guestDup("Primary Sound Driver");module=guestDup("");}
    callGuest(arg(0),{0,description,module,arg(1)});
    ret(0,2);
}

// --- fo2_zoom.dll: the Zoom/Steam shim (its DllMain started Steam; nothing calls ZOOM_Init in .text) ------
void zoomInit(){ret(1,0);}

const std::map<std::string,recomp_func_t> exeFunctions={
    {"kernel32.dll!GetSystemTime",getSystemTime},{"kernel32.dll!GetLocalTime",getLocalTime},
    {"kernel32.dll!SystemTimeToFileTime",systemTimeToFileTime},{"kernel32.dll!GetUserDefaultLangID",getUserDefaultLangID},
    {"kernel32.dll!PeekNamedPipe",peekNamedPipe},{"kernel32.dll!GetFileInformationByHandle",fileInformationByHandle},
    {"kernel32.dll!VirtualLock",virtualLock},{"kernel32.dll!OpenFile",openFile},
    {"kernel32.dll!GlobalAlloc",globalAlloc},{"kernel32.dll!GlobalFree",globalFree},
    {"kernel32.dll!GetSystemDirectoryA",getSystemDirectoryA},{"kernel32.dll!GetWindowsDirectoryA",getWindowsDirectoryA},
    {"kernel32.dll!GetTempPathA",getTempPathA},{"kernel32.dll!DeviceIoControl",deviceIoControl},
    {"kernel32.dll!IsBadCodePtr",isBadCodePtr},{"kernel32.dll!TerminateThread",terminateThread},
    {"kernel32.dll!GetOverlappedResult",getOverlappedResult},{"kernel32.dll!ExpandEnvironmentStringsA",expandEnvironmentStringsA},
    {"kernel32.dll!IsDBCSLeadByte",isDBCSLeadByte},{"kernel32.dll!DisableThreadLibraryCalls",disableThreadLibraryCalls},
    {"kernel32.dll!FormatMessageA",formatMessageA},
    {"user32.dll!LoadImageA",loadImageA},{"user32.dll!CharLowerBuffA",charLowerBuffA},{"user32.dll!ToUnicode",toUnicode},
    {"user32.dll!GetKeyState",getKeyState},{"user32.dll!SetWindowsHookExA",setWindowsHookExA},
    {"user32.dll!UnhookWindowsHookEx",unhookWindowsHookEx},{"user32.dll!CallNextHookEx",callNextHookEx},
    {"user32.dll!GetKeyboardLayout",getKeyboardLayout},{"user32.dll!GetClassNameA",getClassNameA},
    {"user32.dll!UnregisterClassA",unregisterClassA},{"user32.dll!GetTitleBarInfo",getTitleBarInfo},
    {"user32.dll!SendDlgItemMessageW",sendDlgItemMessageW},
    {"advapi32.dll!RegCreateKeyExA",regCreateKeyExA},{"advapi32.dll!RegEnumKeyA",regEnumKeyA},
    {"ole32.dll!CoInitializeEx",coInitializeEx},{"ole32.dll!CLSIDFromString",clsidFromString},
    {"winmm.dll!waveOutGetNumDevs",waveGetNumDevs},{"winmm.dll!waveInGetNumDevs",waveGetNumDevs},
    {"winmm.dll!waveOutGetDevCapsA",waveNoDriver3},{"winmm.dll!waveInGetDevCapsA",waveNoDriver3},
    {"winmm.dll!waveOutOpen",waveNoDriver6},{"winmm.dll!waveInOpen",waveNoDriver6},
    {"winmm.dll!waveOutClose",waveNoDriver1},{"winmm.dll!waveInClose",waveNoDriver1},
    {"winmm.dll!waveOutReset",waveNoDriver1},{"winmm.dll!waveInReset",waveNoDriver1},{"winmm.dll!waveInStart",waveNoDriver1},
    {"winmm.dll!waveOutPrepareHeader",waveNoDriver3},{"winmm.dll!waveOutUnprepareHeader",waveNoDriver3},
    {"winmm.dll!waveOutWrite",waveNoDriver3},{"winmm.dll!waveOutGetPosition",waveNoDriver3},
    {"winmm.dll!waveInAddBuffer",waveNoDriver3},{"winmm.dll!waveInPrepareHeader",waveNoDriver3},
    {"winmm.dll!waveInUnprepareHeader",waveNoDriver3},{"winmm.dll!mciSendCommandA",mciSendCommandA},
    {"ws2_32.dll!WSAResetEvent",wsaResetEvent},{"ws2_32.dll!WSACloseEvent",wsaCloseEvent},{"ws2_32.dll!WSACreateEvent",wsaCreateEvent},
    {"ws2_32.dll!WSAAsyncGetHostByName",wsaFail5},{"ws2_32.dll!WSACancelAsyncRequest",wsaFail1},
    {"ws2_32.dll!inet_ntoa",inetNtoa},{"ws2_32.dll!inet_addr",inetAddr},{"ws2_32.dll!__WSAFDIsSet",wsafdIsSet},
    // the same, as the exe imports them: by ordinal
    {"ws2_32.dll!#11",inetAddr},{"ws2_32.dll!#12",inetNtoa},{"ws2_32.dll!#151",wsafdIsSet},
    {"ws2_32.dll!#103",wsaFail5},{"ws2_32.dll!#108",wsaFail1},
    {"ws2_32.dll!WSAStringToAddressA",wsaStringToAddress},{"ws2_32.dll!WSAGetOverlappedResult",wsaGetOverlappedResult},
    {"ws2_32.dll!WSAWaitForMultipleEvents",wsaWaitForMultipleEvents},{"ws2_32.dll!WSARecvFrom",wsaRecvFrom},{"ws2_32.dll!WSASendTo",wsaSendTo},
    {"iphlpapi.dll!GetAdaptersInfo",getAdaptersInfo},
    {"msacm32.dll!acmStreamOpen",acmFail8},{"msacm32.dll!acmStreamClose",acmFail2},{"msacm32.dll!acmStreamSize",acmFail4},
    {"msacm32.dll!acmFormatSuggest",acmFail5},{"msacm32.dll!acmStreamConvert",acmFail3},
    {"msacm32.dll!acmStreamPrepareHeader",acmFail3},{"msacm32.dll!acmStreamUnprepareHeader",acmFail3},
    {"fo2_zoom.dll!ZOOM_Init",zoomInit},
    {"dsound.dll!DirectSoundEnumerateA",directSoundEnumerateA},{"dsound.dll!#2",directSoundEnumerateA},
};

}  // namespace fo2
