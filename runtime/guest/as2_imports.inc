// Alien Shooter 2: Reloaded - the Win32 imports of AlienShooter.exe that the NFSU2 runtime does not have.
// Included by guest_runtime.cpp after the other guest_*.inc files; resolveFunction() looks names up in
// as2Functions ("dll!name", lower-case dll) before giving up with unsupported().
// Each handler is stdcall: ret(value, argument count). Calls that the game has not needed yet in a real
// way log once ("AS2 import") so the log shows where work is still missing.

#include "vkbd_font.h"   // runtime/d3d9_gles: DejaVu Sans Bold 12 px atlas, used by DrawText below

namespace as2 {

void logOnce(const char* what){
    static std::set<std::string> seen;
    if(seen.insert(what).second)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","AS2 import %s (from %08x)",what,g_cur_func);
}
uint16_t read16(uint32_t va){uint16_t v;std::memcpy(&v,ptr(va,2),2);return v;}
void write16(uint32_t va,uint16_t v){std::memcpy(ptr(va,2),&v,2);}
std::string guestStringN(uint32_t va,int32_t count){  // count -1: NUL-terminated
    if(count<0)return guestString(va);
    std::string s(static_cast<const char*>(ptr(va,count)),static_cast<size_t>(count));
    return s;
}
std::u16string guestWideN(uint32_t va,int32_t count){
    std::u16string s;
    for(uint32_t i=0;count<0||i<static_cast<uint32_t>(count);++i){uint16_t c=read16(va+i*2);if(count<0&&!c)break;s.push_back(char16_t(c));}
    return s;
}
// Writes text and a NUL into a guest buffer of 'capacity' characters; returns the Win32 count (with the
// NUL), or the size needed when capacity is 0.
uint32_t putString(uint32_t out,uint32_t capacity,const std::string& text){
    const uint32_t needed=static_cast<uint32_t>(text.size()+1);
    if(!capacity)return needed;
    if(capacity<needed){lastError=122;return 0;}
    std::memcpy(ptr(out,needed),text.c_str(),needed);return needed;
}
uint32_t putWide(uint32_t out,uint32_t capacity,const std::string& text){
    const uint32_t needed=static_cast<uint32_t>(text.size()+1);
    if(!capacity)return needed;
    if(capacity<needed){lastError=122;return 0;}
    for(uint32_t i=0;i<needed;++i)write16(out+i*2,i<text.size()?static_cast<unsigned char>(text[i]):0);
    return needed;
}
// Calls an existing handler with its first argument replaced by a guest copy of 'text' (the *W functions
// whose ANSI version exists: the game passes plain ASCII paths).
void callWithAnsiArgument(recomp_func_t handler,const std::u16string& text){
    std::string narrow;for(char16_t c:text)narrow.push_back(c<128?char(c):'?');
    const uint32_t copy=allocate(static_cast<uint32_t>(narrow.size()+1));
    std::memcpy(ptr(copy,narrow.size()+1),narrow.c_str(),narrow.size()+1);
    write32(g_esp+4,copy);
    handler();
}

// --- kernel32: locale, code pages, console --------------------------------------------------------------
void isValidCodePage(){ret(1,1);}
void isValidLocale(){ret(1,2);}
void getUserDefaultLCID(){ret(0x409,0);}
std::string localeValue(uint32_t type){
    switch(type&0xffff){
    case 0x1004:return "1252";                  // LOCALE_IDEFAULTANSICODEPAGE
    case 0x000b:return "437";                   // LOCALE_IDEFAULTCODEPAGE (OEM)
    case 0x0001:return "0409";                  // LOCALE_ILANGUAGE
    case 0x0003:return "ENU";                   // LOCALE_SABBREVLANGNAME
    case 0x0007:return "USA";                   // LOCALE_SABBREVCTRYNAME
    case 0x000e:return ".";                     // LOCALE_SDECIMAL
    case 0x000f:return ",";                     // LOCALE_STHOUSAND
    case 0x001d:return "/";                     // LOCALE_SDATE
    case 0x001e:return ":";                     // LOCALE_STIME
    case 0x0059:return "en";                    // LOCALE_SISO639LANGNAME
    case 0x005a:return "US";                    // LOCALE_SISO3166CTRYNAME
    default:logOnce("GetLocaleInfo (unknown type)");return "";
    }
}
void getLocaleInfoA(){ret(putString(arg(2),arg(3),localeValue(arg(1))),4);}
void getLocaleInfoW(){ret(putWide(arg(2),arg(3),localeValue(arg(1))),4);}
void enumSystemLocalesA(){ret(1,2);}
int compareNoCase(const std::u16string& a,const std::u16string& b,bool fold){
    for(size_t i=0;i<a.size()&&i<b.size();++i){
        char16_t x=a[i],y=b[i];
        if(fold){if(x<128)x=char16_t(std::tolower(x));if(y<128)y=char16_t(std::tolower(y));}
        if(x!=y)return x<y?1:3;
    }
    return a.size()==b.size()?2:(a.size()<b.size()?1:3);
}
std::u16string widen(const std::string& s){std::u16string w;for(unsigned char c:s)w.push_back(c);return w;}
void compareStringA(){  // (lcid, flags, s1, n1, s2, n2) -> CSTR_LESS_THAN 1 / EQUAL 2 / GREATER 3
    const int32_t n1=static_cast<int32_t>(arg(3)),n2=static_cast<int32_t>(arg(5));
    ret(compareNoCase(widen(guestStringN(arg(2),n1)),widen(guestStringN(arg(4),n2)),arg(1)&1),6);
}
void compareStringW(){
    const int32_t n1=static_cast<int32_t>(arg(3)),n2=static_cast<int32_t>(arg(5));
    ret(compareNoCase(guestWideN(arg(2),n1),guestWideN(arg(4),n2),arg(1)&1),6);
}
uint16_t ctype1(unsigned char c){
    uint16_t t=0;
    if(std::isupper(c))t|=0x1;if(std::islower(c))t|=0x2;if(std::isdigit(c))t|=0x4;if(std::isspace(c))t|=0x8;
    if(std::ispunct(c))t|=0x10;if(std::iscntrl(c))t|=0x20;if(c==' '||c=='\t')t|=0x40;if(std::isxdigit(c))t|=0x80;
    if(std::isalpha(c))t|=0x100;
    return t;
}
void getStringTypeA(){  // (lcid, infoType, src, count, out)
    const int32_t count=static_cast<int32_t>(arg(3));
    const std::string s=guestStringN(arg(2),count<0?-1:count);
    const size_t n=count<0?s.size()+1:s.size();
    for(size_t i=0;i<n;++i)write16(arg(4)+uint32_t(i)*2,(arg(1)==1)?ctype1(i<s.size()?static_cast<unsigned char>(s[i]):0):0);
    ret(1,5);
}
void lcMapStringA(){  // (lcid, flags, src, count, dst, dstCount)
    const int32_t count=static_cast<int32_t>(arg(3));
    std::string s=guestStringN(arg(2),count);
    if(arg(1)&0x100)for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));
    if(arg(1)&0x200)for(auto& c:s)c=char(std::toupper(static_cast<unsigned char>(c)));
    uint32_t length=static_cast<uint32_t>(s.size())+(count<0?1:0);
    if(!arg(5)){ret(length,6);return;}
    if(arg(5)<length){lastError=122;ret(0,6);return;}
    std::memcpy(ptr(arg(4),s.size()),s.data(),s.size());
    if(count<0)*static_cast<char*>(ptr(arg(4)+uint32_t(s.size()),1))=0;
    ret(length,6);
}
void consoleCP(){ret(437,0);}
void writeConsoleA(){
    const uint32_t n=arg(2);
    if(n)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","console: %s",guestStringN(arg(1),static_cast<int32_t>(std::min<uint32_t>(n,512))).c_str());
    if(arg(3))write32(arg(3),n);ret(1,5);
}
void writeConsoleW(){if(arg(3))write32(arg(3),arg(2));ret(1,5);}
void setStdHandle(){ret(1,2);}
void setEnvironmentVariableA(){ret(1,2);}
void lstrcmpiA(){ret(static_cast<uint32_t>(compareNoCase(widen(guestString(arg(0))),widen(guestString(arg(1))),true)-2),2);}
void mulDiv(){
    const int32_t a=static_cast<int32_t>(arg(0)),b=static_cast<int32_t>(arg(1)),c=static_cast<int32_t>(arg(2));
    if(!c){ret(UINT32_MAX,3);return;}
    const int64_t p=int64_t(a)*b;const int64_t r=(p+((p<0)!=(c<0)?-c/2:c/2))/c;
    ret(static_cast<uint32_t>(static_cast<int32_t>(r)),3);
}

// --- kernel32: memory --------------------------------------------------------------------------------------
// VirtualAlloc hands out 64 KB-aligned blocks from a region no other guest allocator uses
// (heaps: 0x10000000-0x1f000000 and 0x30000000-0x70000000; stack 0x20000000; imports 0x7b000000+).
constexpr uint32_t virtualBase=0x72000000u,virtualEnd=0x7a000000u;
uint32_t virtualNext=virtualBase;
void virtualAlloc(){  // (address, size, type, protect)
    const uint32_t size=(arg(1)+0xfffu)&~0xfffu;
    if(arg(0)){
        const uint32_t start=arg(0)&~0xfffu;
        if(arg(2)&0x1000)memory->commit(start,((arg(0)+arg(1)+0xfffu)&~0xfffu)-start);
        ret(arg(0),4);return;
    }
    const uint32_t block=(size+0xffffu)&~0xffffu;
    if(!size||virtualNext+block>virtualEnd||virtualNext+block<virtualNext){lastError=8;ret(0,4);return;}
    const uint32_t address=virtualNext;virtualNext+=block;
    memory->commit(address,block);   // reserved space is committed as well: the guest touches it after MEM_COMMIT
    ret(address,4);
}
void virtualFree(){ret(1,3);}
void virtualQuery(){  // (address, MEMORY_BASIC_INFORMATION*, length) -> 28
    const uint32_t page=arg(0)&~0xfffu,out=arg(1);
    const bool used=memory->readable(page,0x1000);
    write32(out,page);write32(out+4,page);write32(out+8,used?4:0);write32(out+12,0x1000);
    write32(out+16,used?0x1000:0x10000);write32(out+20,used?4:1);write32(out+24,used?0x20000:0);
    ret(28,3);
}
void globalMemoryStatus(){  // MEMORYSTATUS: the handheld's memory as a 2009 PC would see it
    const uint32_t out=arg(0);
    write32(out,32);write32(out+4,40);write32(out+8,0x40000000u);write32(out+12,0x30000000u);
    write32(out+16,0x60000000u);write32(out+20,0x50000000u);write32(out+24,0x7ffe0000u);write32(out+28,0x70000000u);
    ret(0,1);
}

// --- kernel32: exceptions, process ----------------------------------------------------------------------
void raiseException(){
    throw GuestStop("RaiseException code "+hex(arg(0))+" flags "+hex(arg(1))+" from "+hex(g_cur_func)+
                    " (SEH dispatch is not implemented in the AS2 runtime yet)");
}
void rtlUnwind(){throw GuestStop("RtlUnwind from "+hex(g_cur_func)+" (SEH unwinding not implemented yet)");}
void unhandledExceptionFilter(){throw GuestStop("UnhandledExceptionFilter from "+hex(g_cur_func));}
void terminateProcess(){throw GuestStop("Guest requested TerminateProcess("+std::to_string(arg(1))+") from "+hex(g_cur_func));}

// --- kernel32: files, modules, resources ------------------------------------------------------------------
void moduleFileNameW(){
    const uint32_t out=arg(1),capacity=arg(2);
    if(arg(0)&&arg(0)!=imageBase){lastError=126;ret(0,3);return;}
    const std::string path=std::string("C:\\nfsu2\\")+NFS_EXE_NAME;
    const uint32_t count=std::min<uint32_t>(capacity,static_cast<uint32_t>(path.size()));
    for(uint32_t i=0;i<count;++i)write16(out+i*2,static_cast<unsigned char>(path[i]));
    if(capacity>count)write16(out+count*2,0);else lastError=122;
    ret(count,3);
}
void findResourceW(){logOnce("FindResourceW");lastError=1813;ret(0,3);}
void createFileW(){callWithAnsiArgument(createFile,guestWideN(arg(0),-1));}
void loadLibraryW(){callWithAnsiArgument(loadLibrary,guestWideN(arg(0),-1));}
void setFileTime(){ret(1,4);}
void copyFileA(){  // (from, to, failIfExists)
    const auto from=gamePath(guestString(arg(0))),to=gamePath(guestString(arg(1)));
    std::error_code ec;
    const auto options=arg(2)?std::filesystem::copy_options::none:std::filesystem::copy_options::overwrite_existing;
    if(!std::filesystem::copy_file(from,to,options,ec)){lastError=ec?5:80;ret(0,3);return;}
    ret(1,3);
}
void moveFileExA(){  // (from, to, flags): MOVEFILE_REPLACE_EXISTING (1) replaces the target
    const auto from=gamePath(guestString(arg(0)));
    if(!arg(1)){std::error_code ec;std::filesystem::remove(from,ec);ret(ec?0:1,3);return;}
    const auto to=gamePath(guestString(arg(1)));
    std::error_code ec;
    if(!(arg(2)&1)&&std::filesystem::exists(to)){lastError=183;ret(0,3);return;}
    std::filesystem::rename(from,to,ec);
    if(ec){lastError=5;ret(0,3);return;}
    ret(1,3);
}
// GetPrivateProfileString/Int: the game's .ini, read from the game folder ("[section]" / "key=value").
std::string profileValue(const std::string& file,const std::string& section,const std::string& key,bool& found){
    found=false;
    std::ifstream in(gamePath(file));std::string line,current;
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.size()>1&&line[0]=='['){current=line.substr(1,line.find(']')-1);continue;}
        if(lower(current)!=lower(section))continue;
        const auto eq=line.find('=');if(eq==std::string::npos)continue;
        std::string name=line.substr(0,eq);while(!name.empty()&&std::isspace(static_cast<unsigned char>(name.back())))name.pop_back();
        if(lower(name)!=lower(key))continue;
        std::string value=line.substr(eq+1);
        const auto comment=value.find(';');if(comment!=std::string::npos)value=value.substr(0,comment);
        while(!value.empty()&&std::isspace(static_cast<unsigned char>(value.back())))value.pop_back();
        while(!value.empty()&&std::isspace(static_cast<unsigned char>(value.front())))value.erase(value.begin());
        found=true;return value;
    }
    return "";
}
void getPrivateProfileStringA(){  // (section, key, default, out, size, file)
    if(!arg(0)||!arg(1)){logOnce("GetPrivateProfileStringA (section/key list)");ret(putString(arg(3),arg(4),"")?0:0,6);return;}
    bool found;std::string value=profileValue(guestString(arg(5)),guestString(arg(0)),guestString(arg(1)),found);
    if(!found)value=arg(2)?guestString(arg(2)):"";
    const uint32_t size=arg(4);
    if(!size){ret(0,6);return;}
    if(value.size()>=size)value.resize(size-1);
    std::memcpy(ptr(arg(3),value.size()+1),value.c_str(),value.size()+1);
    ret(static_cast<uint32_t>(value.size()),6);
}
void getPrivateProfileIntA(){  // (section, key, default, file)
    bool found;const std::string value=profileValue(guestString(arg(3)),guestString(arg(0)),guestString(arg(1)),found);
    ret(found?static_cast<uint32_t>(std::strtol(value.c_str(),nullptr,10)):arg(2),4);
}
// File mappings, read-only: D3DX maps the game's .fx effect files. A view is a guest copy of the file's
// bytes (the game never writes through one); the mapping keeps its own descriptor, so the file handle
// can be closed while the mapping lives, as on Windows.
struct Mapping{int descriptor;uint64_t size;};
std::map<uint32_t,Mapping> mappings;uint32_t nextMapping=0xf0070000u;
std::map<uint32_t,uint32_t> views;   // view address -> size
void createFileMappingA(){  // (file, attributes, protect, maximumHigh, maximumLow, name)
    auto file=fileHandles.find(arg(0));
    if(file==fileHandles.end()){logOnce("CreateFileMappingA without a file (page-file mapping)");lastError=6;ret(0,6);return;}
    if(arg(2)!=2&&arg(2)!=8)logOnce("CreateFileMappingA for writing (mapped read-only)");
    struct stat status{};
    if(fstat(file->second,&status)){lastError=fileError();ret(0,6);return;}
    const int descriptor=::dup(file->second);
    if(descriptor<0){lastError=fileError();ret(0,6);return;}
    uint64_t size=static_cast<uint64_t>(status.st_size);
    const uint64_t maximum=(uint64_t(arg(3))<<32)|arg(4);
    if(maximum)size=std::min(size,maximum);
    const uint32_t handle=nextMapping++;
    mappings[handle]=Mapping{descriptor,size};
    ret(handle,6);
}
void mapViewOfFile(){  // (mapping, access, offsetHigh, offsetLow, bytes)
    auto mapping=mappings.find(arg(0));
    if(mapping==mappings.end()){lastError=6;ret(0,5);return;}
    const uint64_t offset=(uint64_t(arg(2))<<32)|arg(3);
    if(offset>mapping->second.size){lastError=87;ret(0,5);return;}
    const uint64_t available=mapping->second.size-offset;
    const uint32_t bytes=static_cast<uint32_t>(arg(4)?std::min<uint64_t>(arg(4),available):available);
    const uint32_t view=allocate(std::max<uint32_t>(bytes,1));
    uint32_t done=0;
    while(done<bytes){
        const ssize_t n=::pread(mapping->second.descriptor,ptr(view+done,bytes-done),bytes-done,static_cast<off_t>(offset+done));
        if(n<=0)break;
        done+=static_cast<uint32_t>(n);
    }
    if(done<bytes)std::memset(ptr(view+done,bytes-done),0,bytes-done);
    views[view]=bytes;
    ret(view,5);
}
void unmapViewOfFile(){
    auto view=views.find(arg(0));
    if(view==views.end()){lastError=487;ret(0,1);return;}
    heaps->free(GuestHeaps::process,view->first);views.erase(view);ret(1,1);
}
// CloseHandle of a mapping (the runtime's own CloseHandle does not know them)
bool closeMapping(uint32_t handle){
    auto mapping=mappings.find(handle);if(mapping==mappings.end())return false;
    ::close(mapping->second.descriptor);mappings.erase(mapping);return true;
}
void dateFormatA(){  // (lcid, flags, SYSTEMTIME*, format, out, size): MM/dd/yyyy
    uint32_t y,m,d;
    if(arg(2)){y=read16(arg(2));m=read16(arg(2)+2);d=read16(arg(2)+6);}
    else{std::time_t t=std::time(nullptr);std::tm tm{};localtime_r(&t,&tm);y=tm.tm_year+1900;m=tm.tm_mon+1;d=tm.tm_mday;}
    char text[32];std::snprintf(text,sizeof text,"%02u/%02u/%04u",m,d,y);
    ret(putString(arg(4),arg(5),text),6);
}
void timeFormatA(){  // (lcid, flags, SYSTEMTIME*, format, out, size): HH:mm:ss
    uint32_t h,mi,s;
    if(arg(2)){h=read16(arg(2)+8);mi=read16(arg(2)+10);s=read16(arg(2)+12);}
    else{std::time_t t=std::time(nullptr);std::tm tm{};localtime_r(&t,&tm);h=tm.tm_hour;mi=tm.tm_min;s=tm.tm_sec;}
    char text[32];std::snprintf(text,sizeof text,"%02u:%02u:%02u",h,mi,s);
    ret(putString(arg(4),arg(5),text),6);
}

// --- user32 ---------------------------------------------------------------------------------------------
constexpr uint32_t fakeDC=0x00d00001u,fakeCursor=0x00c00011u,fakeAccelerators=0x00a00001u;
void getDC(){ret(fakeDC,1);}
void releaseDC(){ret(1,2);}
void clientToScreen(){ret(1,2);}
void setCursorPos(){  // screen coordinates -> the pointer the right stick moves
    if(auto window=windows.find(activeWindow);window!=windows.end()){
        const float width=float(std::max(window->second.width,1)),height=float(std::max(window->second.height,1));
        guestPointerX=std::clamp(float(int32_t(arg(0))-window->second.x)/width,0.f,1.f);
        guestPointerY=std::clamp(float(int32_t(arg(1))-window->second.y)/height,0.f,1.f);
    }
    ret(1,2);
}
void loadStringA(){logOnce("LoadStringA");if(arg(2)&&arg(3))*static_cast<char*>(ptr(arg(2),1))=0;ret(0,4);}
void isCharAlphaA(){ret(std::isalpha(static_cast<unsigned char>(arg(0)&0xff))?1:0,1);}
void charCase(bool upper){
    const uint32_t p=arg(0);
    if(p<=0xffff){const unsigned char c=static_cast<unsigned char>(p);ret(upper?std::toupper(c):std::tolower(c),1);return;}
    for(uint32_t i=0;;++i){char* c=static_cast<char*>(ptr(p+i,1));if(!*c)break;*c=char(upper?std::toupper(static_cast<unsigned char>(*c)):std::tolower(static_cast<unsigned char>(*c)));}
    ret(p,1);
}
void charUpperA(){charCase(true);}
void charLowerA(){charCase(false);}
void waitMessage(){std::this_thread::sleep_for(std::chrono::milliseconds(1));ret(1,0);}
void translateAcceleratorA(){ret(0,3);}
void activateKeyboardLayout(){ret(0x04090409u,2);}
void keyboardLayoutNameA(){std::memcpy(ptr(arg(0),9),"00000409",9);ret(1,1);}
void loadAcceleratorsA(){ret(fakeAccelerators,2);}
void registerClassA(){  // WNDCLASSA (40 bytes) -> WNDCLASSEXA (cbSize, the same 40 bytes, hIconSm)
    const uint32_t ex=allocate(48);
    write32(ex,48);std::memcpy(ptr(ex+4,40),ptr(arg(0),40),40);write32(ex+44,0);
    write32(g_esp+4,ex);
    registerClassEx();
}
void findWindowA(){ret(0,2);}
void getCursor(){ret(fakeCursor,0);}
void toAscii(){  // (vk, scan, keyState[256], out WORD*, flags)
    const uint32_t vk=arg(0);
    const bool shift=arg(2)&&(*static_cast<const uint8_t*>(ptr(arg(2)+0x10,1))&0x80);
    int c=0;
    if(vk>='A'&&vk<='Z')c=shift?int(vk):int(vk)+32;
    else if(vk>='0'&&vk<='9')c=int(vk);
    else if(vk==0x20||vk==0x0d||vk==0x08||vk==0x1b)c=int(vk);
    else if(vk>=0x60&&vk<=0x69)c='0'+int(vk-0x60);
    if(!c){ret(0,5);return;}
    write16(arg(3),static_cast<uint16_t>(c));ret(1,5);
}
// DrawTextA/W are defined with the GDI objects below.
void drawTextA();
void drawTextW();
void dialogBoxParamA(){logOnce("DialogBoxParamA");ret(1,5);}
void endDialog(){ret(1,2);}
void setWindowTextA(){ret(1,2);}
// Cursors: the game loads cursores\arrow.ani (a plain .cur) and attack.ANI (RIFF ACON, one frame) and lets
// Windows draw them. Here they are decoded to RGBA; the D3D9 layer draws the current one at the pointer
// (guestCursorImage in guest_runtime.cpp).
struct CursorImage{int width=0,height=0,hotX=0,hotY=0;bool crosshair=false;std::vector<uint32_t> rgba;};  // RGBA bytes, top row first
std::mutex cursorMutex;
std::map<uint32_t,std::shared_ptr<const CursorImage>> cursorImages;
std::shared_ptr<const CursorImage> firstCursor;   // stands in for the system cursors (LoadCursor)
uint32_t nextCursorHandle=0x00c00100u;
std::shared_ptr<const CursorImage> decodeCursor(const uint8_t* d,size_t n){
    auto u16=[&](size_t o){return o+2<=n?uint32_t(d[o]|d[o+1]<<8):0u;};
    auto u32=[&](size_t o){return o+4<=n?uint32_t(d[o]|d[o+1]<<8|d[o+2]<<16|uint32_t(d[o+3])<<24):0u;};
    if(n>=12&&!std::memcmp(d,"RIFF",4)&&!std::memcmp(d+8,"ACON",4)){  // the first "icon" chunk of LIST fram
        for(size_t o=12;o+8<=n;){
            const uint32_t size=u32(o+4);
            if(!std::memcmp(d+o,"LIST",4)&&o+12<=n&&!std::memcmp(d+o+8,"fram",4)){o+=12;continue;}
            if(!std::memcmp(d+o,"icon",4))return o+8+size<=n?decodeCursor(d+o+8,size):nullptr;
            o+=8+size+(size&1);
        }
        return nullptr;
    }
    if(n<22||u16(0)!=0||(u16(2)!=1&&u16(2)!=2)||!u16(4))return nullptr;
    auto image=std::make_shared<CursorImage>();
    image->hotX=int(u16(10));image->hotY=int(u16(12));
    const size_t bmp=u32(18);
    if(bmp+40>n||u32(bmp)<40)return nullptr;
    const int w=int(u32(bmp+4)),h=int(u32(bmp+8))/2,bpp=int(u16(bmp+14));
    if(w<=0||w>256||h<=0||h>256||(bpp!=1&&bpp!=4&&bpp!=8&&bpp!=24&&bpp!=32))return nullptr;
    const size_t colors=bpp<=8?(u32(bmp+32)?u32(bmp+32):1u<<bpp):0,palette=bmp+u32(bmp),pixels=palette+colors*4;
    const size_t xorStride=((size_t(w)*bpp+31)/32)*4,andStride=((size_t(w)+31)/32)*4,mask=pixels+xorStride*h;
    if(mask+andStride*h>n)return nullptr;
    image->width=w;image->height=h;image->rgba.resize(size_t(w)*h);
    bool alpha=false;
    if(bpp==32)for(int y=0;y<h&&!alpha;++y)for(int x=0;x<w;++x)if(d[pixels+y*xorStride+x*4+3]){alpha=true;break;}
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const uint8_t* row=d+pixels+size_t(h-1-y)*xorStride;
        uint32_t b=0,g=0,r=0,a=255;
        if(bpp==32){b=row[x*4];g=row[x*4+1];r=row[x*4+2];if(alpha)a=row[x*4+3];}
        else if(bpp==24){b=row[x*3];g=row[x*3+1];r=row[x*3+2];}
        else{
            const uint32_t index=bpp==8?row[x]:bpp==4?(row[x/2]>>(x&1?0:4))&15:(row[x/8]>>(7-x%8))&1;
            if(index<colors){b=d[palette+index*4];g=d[palette+index*4+1];r=d[palette+index*4+2];}
        }
        const bool transparent=(d[mask+size_t(h-1-y)*andStride+x/8]>>(7-x%8))&1;
        if(!alpha&&transparent){a=(r|g|b)?255:0;r=g=b=0;}   // screen-inverting pixels: drawn black
        image->rgba[size_t(y)*w+x]=r|g<<8|b<<16|a<<24;
    }
    return image;
}
void loadCursorFromFileA(){
    const auto path=gamePath(guestString(arg(0)));
    std::ifstream in(path,std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    std::shared_ptr<const CursorImage> decoded=decodeCursor(data.data(),data.size());
    std::shared_ptr<CursorImage> image;
    if(decoded){   // attack.ANI: the crosshair shown while playing (as2_controls.h aims with the right stick then)
        image=std::make_shared<CursorImage>(*decoded);
        std::string lower(path.c_str());for(char& ch:lower)ch=char(std::tolower(static_cast<unsigned char>(ch)));
        image->crosshair=lower.find("attack")!=std::string::npos;
    }
    if(!image){NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","[AS2] cursor %s not decoded (%zu bytes)",path.c_str(),data.size());ret(fakeCursor,1);return;}
    std::lock_guard<std::mutex> lock(cursorMutex);
    const uint32_t handle=nextCursorHandle++;
    cursorImages[handle]=image;if(!firstCursor)firstCursor=image;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","[AS2] cursor %s: %dx%d hotspot %d,%d -> %08x",path.c_str(),image->width,image->height,image->hotX,image->hotY,handle);
    ret(handle,1);
}
void destroyCursor(){ret(1,1);}
void sendDlgItemMessageA(){ret(0,5);}
void enableWindow(){ret(0,2);}
void redrawWindow(){ret(1,4);}
void getDlgItem(){ret(0,2);}
void drawMenuBar(){ret(1,1);}

// --- gdi32: memory DCs, DIB sections, fonts and DrawText --------------------------------------------
// The game writes its text (menus, "Loading") with DrawTextA into a DIB section selected in a memory DC
// and uploads the pixels as a texture. Glyphs come from the DejaVu Sans Bold 12 px atlas of the D3D9 layer
// (vkbd_font.h), scaled to the font's height.
uint32_t nextGdiHandle=0x00e00001u;
struct Dib{uint32_t bits;int32_t width,height;uint32_t bpp,stride;};
struct Font{int height;bool bold;bool mono;};
struct DC{uint32_t bitmap=0,font=0;uint32_t textColor=0,bkColor=0xffffff;uint32_t bkMode=2;};  // OPAQUE
std::map<uint32_t,Dib> dibs;std::map<uint32_t,Font> fonts;std::map<uint32_t,DC> dcs;
void createCompatibleDC(){const uint32_t h=nextGdiHandle++;dcs[h];ret(h,1);}
void deleteDC(){dcs.erase(arg(0));ret(1,1);}
void getStockObject(){ret(0x00e80000u|(arg(0)&0xff),1);}
void selectObject(){  // (hdc, object) -> the previous object of that kind
    auto dc=dcs.find(arg(0));const uint32_t object=arg(1);
    if(dc==dcs.end()){ret(0x00e80000u,2);return;}
    uint32_t previous=0x00e80000u;
    if(dibs.count(object)){previous=dc->second.bitmap?dc->second.bitmap:previous;dc->second.bitmap=object;}
    else if(fonts.count(object)){previous=dc->second.font?dc->second.font:previous;dc->second.font=object;}
    ret(previous,2);
}
void setBkColor(){auto dc=dcs.find(arg(0));uint32_t old=0;if(dc!=dcs.end()){old=dc->second.bkColor;dc->second.bkColor=arg(1);}ret(old,2);}
void setTextColor(){auto dc=dcs.find(arg(0));uint32_t old=0;if(dc!=dcs.end()){old=dc->second.textColor;dc->second.textColor=arg(1);}ret(old,2);}
void setBkMode(){auto dc=dcs.find(arg(0));uint32_t old=2;if(dc!=dcs.end()){old=dc->second.bkMode;dc->second.bkMode=arg(1);}ret(old,2);}

// Glyph advance in atlas pixels (12 px font): the glyph's inked width plus one column.
const std::array<int,96>& glyphAdvances(){
    static const std::array<int,96> advances=[]{
        std::array<int,96> a{};
        for(int c=0;c<96;++c){
            const int cx=(c%16)*kVkbdCell,cy=(c/16)*kVkbdCell;int right=-1;
            for(int y=0;y<kVkbdCell;++y)for(int x=0;x<kVkbdCell;++x)if(kVkbdFont[(cy+y)*kVkbdFontW+cx+x]>32)right=std::max(right,x);
            a[c]=right<0?4:right+2;
        }
        return a;
    }();
    return advances;
}
int glyphIndex(unsigned char c){return (c>=32&&c<128)?c-32:('?'-32);}

// TrueType glyphs (stb_truetype) at the size the game asks for: its fonts are Arial 9 and 11 px and Courier 11,
// which the 12 px atlas scaled down by dropping whole rows and columns, before the display shrinks the picture
// by another 0.8. Arial: DejaVu Sans Condensed Bold (about Arial's width); Courier: DejaVu Sans Mono Bold.
// AS2_FONT / AS2_FONT_MONO name other .ttf files; without any, the atlas below is used.
struct TtFace{std::vector<unsigned char> data;stbtt_fontinfo info{};bool ok=false;};
const TtFace* ttFace(bool mono){
    static TtFace faces[2];static bool tried[2]={false,false};
    const int i=mono?1:0;
    if(!tried[i]){
        tried[i]=true;
        std::vector<std::string> candidates;
        if(const char* e=std::getenv(mono?"AS2_FONT_MONO":"AS2_FONT"))candidates.push_back(e);
        candidates.push_back(mono?"/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf":"/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf");
        candidates.push_back("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf");
        for(const std::string& path:candidates){
            std::ifstream in(path,std::ios::binary);if(!in)continue;
            faces[i].data.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
            if(!faces[i].data.empty()&&stbtt_InitFont(&faces[i].info,faces[i].data.data(),stbtt_GetFontOffsetForIndex(faces[i].data.data(),0))){
                faces[i].ok=true;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","GDI text font: %s",path.c_str());break;
            }
        }
        if(!faces[i].ok)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","GDI text: no TrueType font found, using the atlas");
    }
    return faces[i].ok?&faces[i]:nullptr;
}
struct TtGlyph{int w=0,h=0,xoff=0,yoff=0;float advance=0;std::vector<unsigned char> alpha;};
const TtGlyph& ttGlyph(const TtFace* face,float scale,unsigned char c){
    static std::map<std::tuple<const TtFace*,float,unsigned char>,TtGlyph> cache;
    auto key=std::make_tuple(face,scale,c);auto it=cache.find(key);if(it!=cache.end())return it->second;
    TtGlyph g;int adv=0,lsb=0;const int cp=(c>=32&&c<127)?c:'?';
    stbtt_GetCodepointHMetrics(&face->info,cp,&adv,&lsb);g.advance=adv*scale;
    int x0=0,y0=0,x1=0,y1=0;stbtt_GetCodepointBitmapBox(&face->info,cp,scale,scale,&x0,&y0,&x1,&y1);
    g.w=std::max(0,x1-x0);g.h=std::max(0,y1-y0);g.xoff=x0;g.yoff=y0;
    if(g.w&&g.h){g.alpha.resize(size_t(g.w)*g.h);stbtt_MakeCodepointBitmap(&face->info,g.alpha.data(),g.w,g.h,g.w,scale,scale,cp);}
    return cache.emplace(key,std::move(g)).first->second;
}
// Text metrics at a font height: the atlas cell (16 px) holds a 12 px font; a TrueType face uses its own.
struct TextScale{float s;int lineHeight;const TtFace* face=nullptr;float ttScale=0;int ascent=0;};
TextScale textScale(const Font* font){
    const int h=font&&font->height?std::abs(font->height):12;
    TextScale ts{h/12.f,std::max(1,int(std::lround(h*16/12.f)))};
    if(const TtFace* face=ttFace(font&&font->mono)){
        // LOGFONT: a negative height is the character (em) height, a positive one the cell height
        ts.face=face;ts.ttScale=font&&font->height>0?stbtt_ScaleForPixelHeight(&face->info,float(h)):stbtt_ScaleForMappingEmToPixels(&face->info,float(h));
        int ascent=0,descent=0,gap=0;stbtt_GetFontVMetrics(&face->info,&ascent,&descent,&gap);
        ts.ascent=int(std::lround(ascent*ts.ttScale));ts.lineHeight=std::max(1,int(std::lround((ascent-descent)*ts.ttScale)));
    }
    return ts;
}
int textWidth(const std::string& line,const TextScale& ts){
    if(ts.face){float w=0;for(unsigned char c:line)w+=std::round(ttGlyph(ts.face,ts.ttScale,c).advance);return int(w);}
    int w=0;for(unsigned char c:line)w+=glyphAdvances()[glyphIndex(c)];return int(std::lround(w*ts.s));
}
// Lines of a DrawText call: hard breaks, and word wrapping at 'width' for DT_WORDBREAK.
std::vector<std::string> textLines(const std::string& text,uint32_t format,int width,const TextScale& s){
    std::vector<std::string> lines;std::string current;
    auto flush=[&]{lines.push_back(current);current.clear();};
    if(format&0x20){std::string one;for(char c:text)if(c!='\r'&&c!='\n')one.push_back(c);return {one};}   // DT_SINGLELINE
    for(size_t i=0;i<text.size();++i){
        const char c=text[i];
        if(c=='\r')continue;
        if(c=='\n'){flush();continue;}
        current.push_back(c);
        if((format&0x10)&&width>0&&textWidth(current,s)>width){   // DT_WORDBREAK
            const size_t space=current.find_last_of(' ');
            if(space!=std::string::npos&&space>0){std::string rest=current.substr(space+1);current.resize(space);flush();current=rest;}
        }
    }
    flush();
    return lines;
}
void putTextPixel(const Dib& dib,int x,int y,uint32_t color,float alpha){
    if(x<0||y<0||x>=dib.width||y>=std::abs(dib.height)||alpha<=0)return;
    const int row=dib.height<0?y:std::abs(dib.height)-1-y;   // positive height: bottom-up rows
    const uint32_t address=dib.bits+uint32_t(row)*dib.stride;
    const float r=(color&0xff),g=((color>>8)&0xff),b=((color>>16)&0xff);
    if(dib.bpp==32){
        uint8_t* p=static_cast<uint8_t*>(ptr(address+uint32_t(x)*4,4));   // BGRA
        p[0]=uint8_t(p[0]+(b-p[0])*alpha);p[1]=uint8_t(p[1]+(g-p[1])*alpha);p[2]=uint8_t(p[2]+(r-p[2])*alpha);
        p[3]=uint8_t(std::max<float>(p[3],255*alpha));
    }else if(dib.bpp==24){
        uint8_t* p=static_cast<uint8_t*>(ptr(address+uint32_t(x)*3,3));
        p[0]=uint8_t(p[0]+(b-p[0])*alpha);p[1]=uint8_t(p[1]+(g-p[1])*alpha);p[2]=uint8_t(p[2]+(r-p[2])*alpha);
    }else if(dib.bpp==16){   // 5-5-5
        uint16_t v=read16(address+uint32_t(x)*2);
        float pr=((v>>10)&31)*255/31.f,pg=((v>>5)&31)*255/31.f,pb=(v&31)*255/31.f;
        pr+= (r-pr)*alpha;pg+=(g-pg)*alpha;pb+=(b-pb)*alpha;
        write16(address+uint32_t(x)*2,uint16_t((int(pr)*31/255)<<10|(int(pg)*31/255)<<5|(int(pb)*31/255)));
    }
}
// DrawText(hdc, text, rect, format) on the DC's DIB; returns the text height. DT_CALCRECT (0x400) only measures.
uint32_t drawText(uint32_t hdc,const std::string& text,uint32_t rect,uint32_t format){
    auto dcIt=dcs.find(hdc);
    const DC* dc=dcIt==dcs.end()?nullptr:&dcIt->second;
    const Font* font=nullptr;if(dc){auto f=fonts.find(dc->font);if(f!=fonts.end())font=&f->second;}
    const TextScale ts=textScale(font);
    int left=int32_t(read32(rect)),top=int32_t(read32(rect+4)),right=int32_t(read32(rect+8)),bottom=int32_t(read32(rect+12));
    const auto lines=textLines(text,format,right-left,ts);
    int widest=0;for(auto& l:lines)widest=std::max(widest,textWidth(l,ts));
    const int height=int(lines.size())*ts.lineHeight;
    if(format&0x400){write32(rect+8,uint32_t(left+widest));write32(rect+12,uint32_t(top+height));return uint32_t(height);}
    if(!dc||!dibs.count(dc->bitmap))return uint32_t(height);   // a window DC: nothing to draw into
    const Dib& dib=dibs.at(dc->bitmap);
    if(dc->bkMode==2)for(int y=top;y<bottom;++y)for(int x=left;x<right;++x)putTextPixel(dib,x,y,dc->bkColor,1.f);
    int y0=top;
    if(format&0x20){if(format&0x4)y0=top+(bottom-top-ts.lineHeight)/2;else if(format&0x8)y0=bottom-ts.lineHeight;}   // DT_VCENTER, DT_BOTTOM
    for(size_t n=0;n<lines.size();++n){
        const std::string& line=lines[n];const int w=textWidth(line,ts);
        int x0=left;if(format&0x1)x0=left+(right-left-w)/2;else if(format&0x2)x0=right-w;   // DT_CENTER, DT_RIGHT
        const int ly=y0+int(n)*ts.lineHeight;float pen=float(x0);
        if(ts.face){
            // coverage pushed up a little (gamma 0.75): the display shrinks the text again, thin stems first
            for(unsigned char c:line){
                const TtGlyph& g=ttGlyph(ts.face,ts.ttScale,c);
                for(int y=0;y<g.h;++y)for(int x=0;x<g.w;++x){
                    const unsigned char a=g.alpha[size_t(y)*g.w+x];
                    if(a)putTextPixel(dib,int(pen)+g.xoff+x,ly+ts.ascent+g.yoff+y,dc->textColor,std::pow(a/255.f,0.75f));
                }
                pen+=std::round(g.advance);
            }
            continue;
        }
        for(unsigned char c:line){
            const int g=glyphIndex(c),cx=(g%16)*kVkbdCell,cy=(g/16)*kVkbdCell;
            const int gw=int(std::ceil(kVkbdCell*ts.s)),gh=ts.lineHeight;
            for(int y=0;y<gh;++y)for(int x=0;x<gw;++x){
                const int sx=std::min(kVkbdCell-1,int(x/ts.s)),sy=std::min(kVkbdCell-1,int(y/ts.s));
                const float a=kVkbdFont[(cy+sy)*kVkbdFontW+cx+sx]/255.f;
                if(a>0)putTextPixel(dib,int(pen)+x,ly+y,dc->textColor,a);
            }
            pen+=glyphAdvances()[g]*ts.s;
        }
    }
    return uint32_t(height);
}
void drawTextA(){  // (hdc, text, count, rect, format)
    const int32_t count=static_cast<int32_t>(arg(2));
    ret(drawText(arg(0),guestStringN(arg(1),count),arg(3),arg(4)),5);
}
void drawTextW(){
    const int32_t count=static_cast<int32_t>(arg(2));std::string narrow;
    for(char16_t c:guestWideN(arg(1),count))narrow.push_back(c<128?char(c):'?');
    ret(drawText(arg(0),narrow,arg(3),arg(4)),5);
}
void getDeviceCaps(){
    uint32_t value=0;
    switch(arg(1)){case 8:value=800;break;case 10:value=600;break;case 12:value=32;break;case 14:value=1;break;
                   case 88:case 90:value=96;break;default:value=0;}
    ret(value,2);
}
void createFontIndirectA(){  // LOGFONTA: lfHeight at 0 (negative: character height), lfWeight at 16
    const uint32_t h=nextGdiHandle++;
    std::string face=guestString(arg(0)+28);for(char& ch:face)ch=char(std::tolower(static_cast<unsigned char>(ch)));
    fonts[h]=Font{static_cast<int32_t>(read32(arg(0))),read32(arg(0)+16)>=600,face.find("courier")!=std::string::npos};
    static int logged=0;
    if(logged++<4)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","CreateFontIndirectA height %d weight %u face %s",static_cast<int32_t>(read32(arg(0))),read32(arg(0)+16),guestString(arg(0)+28).c_str());
    ret(h,1);
}
void getPixel(){ret(0,3);}
void setPixel(){ret(0,4);}
void createDIBSection(){  // (hdc, BITMAPINFO*, usage, void** bits, section, offset): zeroed pixels in guest memory
    const uint32_t info=arg(1);
    const int32_t width=static_cast<int32_t>(read32(info+4)),height=static_cast<int32_t>(read32(info+8));
    const uint32_t bpp=read16(info+14);
    const uint32_t stride=((static_cast<uint32_t>(std::abs(width))*bpp+31)/32)*4;
    const uint32_t bytes=stride*static_cast<uint32_t>(std::abs(height));
    if(!bytes){ret(0,6);return;}
    const uint32_t bits=allocate(bytes);std::memset(ptr(bits,bytes),0,bytes);
    if(arg(3))write32(arg(3),bits);
    const uint32_t handle=nextGdiHandle++;
    dibs[handle]=Dib{bits,width,height,bpp,stride};
    static int logged=0;
    if(logged++<4)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","CreateDIBSection %dx%d %u bpp",width,height,bpp);
    ret(handle,6);
}

// --- shell32, ole32, comdlg32, winmm ------------------------------------------------------------------
void shellExecuteA(){logOnce("ShellExecuteA");ret(33,6);}
void coInitialize(){ret(0,1);}
void coUninitialize(){ret(0,0);}
void coCreateInstance(){  // (clsid, outer, context, iid, out): no COM server here
    const uint32_t c=arg(0);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","CoCreateInstance clsid=%08x-%04x-%04x from %08x: not available",read32(c),read16(c+4),read16(c+6),g_cur_func);
    if(arg(4))write32(arg(4),0);
    ret(0x80040154u,5);   // REGDB_E_CLASSNOTREG
}
void getOpenFileNameA(){ret(0,1);}
void mciSendStringA(){  // (command, returnString, returnLength, callback): music comes later
    static int logged=0;
    if(logged++<20)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"AS2","mciSendStringA \"%s\" (music not played yet)",guestString(arg(0)).c_str());
    if(arg(1)&&arg(2))*static_cast<char*>(ptr(arg(1),1))=0;
    ret(0,4);
}
void waveOutSetVolume(){ret(0,2);}
void auxSetVolume(){ret(0,2);}

const std::map<std::string,recomp_func_t> functions={
    {"kernel32.dll!IsValidCodePage",isValidCodePage},{"kernel32.dll!IsValidLocale",isValidLocale},
    {"kernel32.dll!GetUserDefaultLCID",getUserDefaultLCID},{"kernel32.dll!GetLocaleInfoA",getLocaleInfoA},
    {"kernel32.dll!GetLocaleInfoW",getLocaleInfoW},{"kernel32.dll!EnumSystemLocalesA",enumSystemLocalesA},
    {"kernel32.dll!CompareStringA",compareStringA},{"kernel32.dll!CompareStringW",compareStringW},
    {"kernel32.dll!GetStringTypeA",getStringTypeA},{"kernel32.dll!LCMapStringA",lcMapStringA},
    {"kernel32.dll!GetConsoleCP",consoleCP},{"kernel32.dll!GetConsoleOutputCP",consoleCP},
    {"kernel32.dll!WriteConsoleA",writeConsoleA},{"kernel32.dll!WriteConsoleW",writeConsoleW},
    {"kernel32.dll!SetStdHandle",setStdHandle},{"kernel32.dll!SetEnvironmentVariableA",setEnvironmentVariableA},
    {"kernel32.dll!lstrcmpiA",lstrcmpiA},{"kernel32.dll!MulDiv",mulDiv},
    {"kernel32.dll!VirtualAlloc",virtualAlloc},{"kernel32.dll!VirtualFree",virtualFree},{"kernel32.dll!VirtualQuery",virtualQuery},
    {"kernel32.dll!GlobalMemoryStatus",globalMemoryStatus},
    {"kernel32.dll!RaiseException",raiseException},{"kernel32.dll!RtlUnwind",rtlUnwind},
    {"kernel32.dll!UnhandledExceptionFilter",unhandledExceptionFilter},{"kernel32.dll!TerminateProcess",terminateProcess},
    {"kernel32.dll!GetModuleFileNameW",moduleFileNameW},{"kernel32.dll!FindResourceW",findResourceW},
    {"kernel32.dll!CreateFileW",createFileW},{"kernel32.dll!LoadLibraryW",loadLibraryW},
    {"kernel32.dll!SetFileTime",setFileTime},{"kernel32.dll!CopyFileA",copyFileA},{"kernel32.dll!MoveFileExA",moveFileExA},
    {"kernel32.dll!GetPrivateProfileStringA",getPrivateProfileStringA},{"kernel32.dll!GetPrivateProfileIntA",getPrivateProfileIntA},
    {"kernel32.dll!CreateFileMappingA",createFileMappingA},{"kernel32.dll!MapViewOfFile",mapViewOfFile},
    {"kernel32.dll!UnmapViewOfFile",unmapViewOfFile},
    {"kernel32.dll!GetDateFormatA",dateFormatA},{"kernel32.dll!GetTimeFormatA",timeFormatA},
    {"user32.dll!GetDC",getDC},{"user32.dll!ReleaseDC",releaseDC},{"user32.dll!ClientToScreen",clientToScreen},
    {"user32.dll!SetCursorPos",setCursorPos},{"user32.dll!LoadStringA",loadStringA},{"user32.dll!IsCharAlphaA",isCharAlphaA},
    {"user32.dll!CharUpperA",charUpperA},{"user32.dll!CharLowerA",charLowerA},{"user32.dll!WaitMessage",waitMessage},
    {"user32.dll!TranslateAcceleratorA",translateAcceleratorA},{"user32.dll!ActivateKeyboardLayout",activateKeyboardLayout},
    {"user32.dll!GetKeyboardLayoutNameA",keyboardLayoutNameA},{"user32.dll!LoadAcceleratorsA",loadAcceleratorsA},
    {"user32.dll!RegisterClassA",registerClassA},{"user32.dll!FindWindowA",findWindowA},{"user32.dll!GetCursor",getCursor},
    {"user32.dll!ToAscii",toAscii},{"user32.dll!DrawTextA",drawTextA},{"user32.dll!DrawTextW",drawTextW},
    {"user32.dll!DialogBoxParamA",dialogBoxParamA},{"user32.dll!EndDialog",endDialog},{"user32.dll!SetWindowTextA",setWindowTextA},
    {"user32.dll!LoadCursorFromFileA",loadCursorFromFileA},{"user32.dll!DestroyCursor",destroyCursor},
    {"user32.dll!SendDlgItemMessageA",sendDlgItemMessageA},{"user32.dll!EnableWindow",enableWindow},
    {"user32.dll!RedrawWindow",redrawWindow},{"user32.dll!GetDlgItem",getDlgItem},{"user32.dll!DrawMenuBar",drawMenuBar},
    {"gdi32.dll!CreateCompatibleDC",createCompatibleDC},{"gdi32.dll!DeleteDC",deleteDC},{"gdi32.dll!GetStockObject",getStockObject},
    {"gdi32.dll!SelectObject",selectObject},{"gdi32.dll!SetBkColor",setBkColor},{"gdi32.dll!SetTextColor",setTextColor},
    {"gdi32.dll!SetBkMode",setBkMode},{"gdi32.dll!GetDeviceCaps",getDeviceCaps},{"gdi32.dll!CreateFontIndirectA",createFontIndirectA},
    {"gdi32.dll!GetPixel",getPixel},{"gdi32.dll!SetPixel",setPixel},{"gdi32.dll!CreateDIBSection",createDIBSection},
    {"shell32.dll!ShellExecuteA",shellExecuteA},
    {"ole32.dll!CoInitialize",coInitialize},{"ole32.dll!CoUninitialize",coUninitialize},{"ole32.dll!CoCreateInstance",coCreateInstance},
    {"comdlg32.dll!GetOpenFileNameA",getOpenFileNameA},
    {"winmm.dll!mciSendStringA",mciSendStringA},{"winmm.dll!waveOutSetVolume",waveOutSetVolume},{"winmm.dll!auxSetVolume",auxSetVolume},
    // DirectSoundCreate8 (ordinal 11) takes DirectSoundCreate's arguments; the device object serves both.
    {"dsound.dll!#11",directSoundCreate},
};

}  // namespace as2
