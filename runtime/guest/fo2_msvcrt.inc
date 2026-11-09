// FlatOut 2: the msvcrt.dll imports of d3dx9_30.dll (lifted next to FlatOut2.exe, guest_modules.inc).
// FlatOut2.exe links its own CRT statically; D3DX uses the system msvcrt, so these run natively.
// All are cdecl: ret(value, 0) leaves the arguments to the caller. Results of double functions go on the
// x87 stack (ST0), and the _CI* intrinsics take their operands from it, as on Windows.
// Included by guest_runtime.cpp after as2_imports.inc.

namespace fo2 {

void logOnce(const char* what){
    static std::set<std::string> seen;
    if(seen.insert(what).second)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"FO2","FO2 msvcrt %s (from %08x)",what,g_cur_func);
}
double argDouble(uint32_t index){uint64_t bits=uint64_t(arg(index))|(uint64_t(arg(index+1))<<32);double v;std::memcpy(&v,&bits,8);return v;}
// x87 stack as the lifted code left it in g_cpu (registers are flushed around every import call).
double& st0(){return g_st_win[g_fp_sp];}
double& st1(){return g_st_win[g_fp_sp+1];}
void x87Push(double v){
    if(g_fp_sp<=8)fp_window_recentre();
    --g_fp_sp;g_st_win[g_fp_sp]=v;g_st_i64_win[g_fp_sp]=0;
}
void x87Pop(){++g_fp_sp;if(g_fp_sp>RECOMP_X87_SLOTS-9)fp_window_recentre();}
void retDouble(double v){x87Push(v);ret(0,0);}
void writeDouble(uint32_t va,double v){std::memcpy(ptr(va,8),&v,8);}
uint32_t guestDup(const std::string& s){uint32_t p=allocate(uint32_t(s.size()+1));std::memcpy(ptr(p,s.size()+1),s.c_str(),s.size()+1);return p;}

// --- startup, exit --------------------------------------------------------------------------------------
void onexit(){ret(arg(0),0);}         // the DLL is never unloaded: its atexit list never runs
void dllonexit(){ret(arg(0),0);}
void initterm(){
    for(uint32_t p=arg(0);p<arg(1);p+=4)if(uint32_t fn=read32(p))callGuest(fn,{});
    ret(0,0);
}
void exceptHandler3(){logOnce("_except_handler3");ret(1,0);}   // ExceptionContinueSearch
void cxxFrameHandler(){logOnce("__CxxFrameHandler");ret(1,0);}
void terminate(){throw GuestStop("d3dx9_30: terminate()");}
void abortCall(){throw GuestStop("d3dx9_30: abort()");}
void exitCall(){throw GuestStop("d3dx9_30: exit("+std::to_string(int32_t(arg(0)))+")");}
void purecall(){throw GuestStop("d3dx9_30: pure virtual call");}

// --- memory ---------------------------------------------------------------------------------------------
void mallocCall(){ret(heaps->alloc(GuestHeaps::process,0,std::max<uint32_t>(arg(0),1)),0);}
void callocCall(){uint64_t n=uint64_t(arg(0))*arg(1);ret(n>0x7fffffff?0:heaps->alloc(GuestHeaps::process,8,std::max<uint32_t>(uint32_t(n),1)),0);}
void freeCall(){if(arg(0))heaps->free(GuestHeaps::process,arg(0));ret(0,0);}
void reallocCall(){
    if(!arg(0)){ret(heaps->alloc(GuestHeaps::process,0,std::max<uint32_t>(arg(1),1)),0);return;}
    if(!arg(1)){heaps->free(GuestHeaps::process,arg(0));ret(0,0);return;}
    ret(heaps->realloc(GuestHeaps::process,0,arg(0),arg(1)),0);
}
void memmoveCall(){if(arg(2))std::memmove(ptr(arg(0),arg(2)),ptr(arg(1),arg(2)),arg(2));ret(arg(0),0);}
void qsortCall(){  // (base, count, width, compare): insertion into a host index order, compare called in the guest
    const uint32_t base=arg(0),count=arg(1),width=arg(2),compare=arg(3);
    if(count<2||!width){ret(0,0);return;}
    std::vector<uint32_t> order(count);for(uint32_t i=0;i<count;++i)order[i]=i;
    std::vector<unsigned char> copy(size_t(count)*width);std::memcpy(copy.data(),ptr(base,count*width),copy.size());
    // Elements are compared in place (the guest comparator reads guest memory): sort indices, then permute.
    // The comparator is cdecl: the caller pops its two arguments (cleanup 8).
    std::stable_sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b){
        return int32_t(callGuest(compare,{base+a*width,base+b*width},8))<0;});
    for(uint32_t i=0;i<count;++i)std::memcpy(ptr(base+i*width,width),copy.data()+size_t(order[i])*width,width);
    ret(0,0);
}

// --- strings and characters -----------------------------------------------------------------------------
void strncpyCall(){
    const uint32_t d=arg(0),s=arg(1),n=arg(2);bool ended=false;
    for(uint32_t i=0;i<n;++i){char c=ended?0:*static_cast<char*>(ptr(s+i,1));if(!c)ended=true;*static_cast<char*>(ptr(d+i,1))=c;}
    ret(d,0);
}
void strchrCall(){
    const char wanted=char(arg(1));
    for(uint32_t p=arg(0);;++p){char c=*static_cast<char*>(ptr(p,1));if(c==wanted){ret(p,0);return;}if(!c){ret(0,0);return;}}
}
void stricmpCall(){
    const std::string a=lower(guestString(arg(0))),b=lower(guestString(arg(1)));
    ret(uint32_t(a<b?-1:a>b?1:0),0);
}
void strdupCall(){ret(arg(0)?guestDup(guestString(arg(0))):0,0);}
void wcslenCall(){uint32_t n=0;while(as2::read16(arg(0)+n*2))++n;ret(n,0);}
void wcstombsCall(){  // (dst, src, n): ASCII only, as everything D3DX converts
    const uint32_t d=arg(0),s=arg(1),n=arg(2);uint32_t i=0;
    for(;;++i){uint16_t c=as2::read16(s+i*2);if(d){if(i>=n)break;*static_cast<char*>(ptr(d+i,1))=char(c<128?c:'?');}if(!c)break;}
    ret(i,0);
}
void atoiCall(){ret(uint32_t(std::atoi(guestString(arg(0)).c_str())),0);}
void atofCall(){retDouble(std::atof(guestString(arg(0)).c_str()));}
void strtodCall(){
    const std::string s=guestString(arg(0));char* end=nullptr;double v=std::strtod(s.c_str(),&end);
    if(arg(1))write32(arg(1),arg(0)+uint32_t(end-s.c_str()));
    retDouble(v);
}
void ultoaCall(){  // (value, buffer, radix)
    uint32_t v=arg(0),radix=arg(2);std::string s;
    do{uint32_t d=v%radix;s.insert(s.begin(),char(d<10?'0'+d:'a'+d-10));v/=radix;}while(v);
    std::memcpy(ptr(arg(1),s.size()+1),s.c_str(),s.size()+1);ret(arg(1),0);
}
template<int (*F)(int)> void ctypeCall(){uint32_t c=arg(0);ret(c<256?uint32_t(F(int(c))?1:0):0,0);}
template<int (*F)(int)> void caseCall(){uint32_t c=arg(0);ret(c<256?uint32_t(F(int(c))):c,0);}
template<int (*F)(int)> void wideCtypeCall(){uint32_t c=arg(0)&0xffff;ret(c<128?uint32_t(F(int(c))?1:0):0,0);}
void setlocaleCall(){static uint32_t name=0;if(!name)name=guestDup("C");ret(name,0);}
void strtimeCall(){std::time_t t=std::time(nullptr);char b[16];std::strftime(b,sizeof b,"%H:%M:%S",std::localtime(&t));std::memcpy(ptr(arg(0),9),b,9);ret(arg(0),0);}
void strdateCall(){std::time_t t=std::time(nullptr);char b[16];std::strftime(b,sizeof b,"%m/%d/%y",std::localtime(&t));std::memcpy(ptr(arg(0),9),b,9);ret(arg(0),0);}
void tempnamCall(){logOnce("_tempnam");ret(0,0);}

// --- printf / scanf -------------------------------------------------------------------------------------
// C99 conversions with MSVC's I64 and h/l/L sizes; arguments read from the guest's va_list.
std::string formatGuest(const std::string& format,uint32_t args){
    std::string out;
    for(size_t i=0;i<format.size();){
        if(format[i]!='%'){out+=format[i++];continue;}
        size_t start=i++;
        if(i<format.size()&&format[i]=='%'){out+='%';++i;continue;}
        std::string spec="%";
        while(i<format.size()&&std::strchr("-+ #0",format[i]))spec+=format[i++];
        auto number=[&]{
            if(i<format.size()&&format[i]=='*'){spec+=std::to_string(int32_t(read32(args)));args+=4;++i;return;}
            while(i<format.size()&&std::isdigit(static_cast<unsigned char>(format[i])))spec+=format[i++];
        };
        number();
        if(i<format.size()&&format[i]=='.'){spec+=format[i++];number();}
        bool wide64=false,longArg=false;
        if(format.compare(i,3,"I64")==0){wide64=true;i+=3;}
        else if(format.compare(i,2,"ll")==0){wide64=true;i+=2;}
        else if(i<format.size()&&(format[i]=='h'||format[i]=='l'||format[i]=='L')){longArg=format[i]=='l';++i;}
        if(i>=format.size()){out+=format.substr(start);break;}
        const char c=format[i++];char buffer[512];int n=0;
        switch(c){
        case 'd':case 'i':case 'u':case 'x':case 'X':case 'o':
            if(wide64){uint64_t v=uint64_t(read32(args))|(uint64_t(read32(args+4))<<32);args+=8;
                n=std::snprintf(buffer,sizeof buffer,(spec+"ll"+c).c_str(),(long long)v);}
            else{uint32_t v=read32(args);args+=4;
                n=(c=='d'||c=='i')?std::snprintf(buffer,sizeof buffer,(spec+c).c_str(),int32_t(v)):std::snprintf(buffer,sizeof buffer,(spec+c).c_str(),v);}
            break;
        case 'c':n=std::snprintf(buffer,sizeof buffer,(spec+'c').c_str(),int(read32(args)&0xff));args+=4;break;
        case 'p':n=std::snprintf(buffer,sizeof buffer,"%08X",read32(args));args+=4;break;
        case 'e':case 'E':case 'f':case 'g':case 'G':case 'a':{
            uint64_t bits=uint64_t(read32(args))|(uint64_t(read32(args+4))<<32);args+=8;double v;std::memcpy(&v,&bits,8);
            n=std::snprintf(buffer,sizeof buffer,(spec+c).c_str(),v);break;}
        case 's':case 'S':{
            uint32_t p=read32(args);args+=4;std::string s;
            if(!p)s="(null)";else if(c=='S'||longArg){for(uint32_t k=0;;++k){uint16_t w=as2::read16(p+k*2);if(!w)break;s+=char(w<128?w:'?');}}
            else s=guestString(p);
            std::string piece(4096,'\0');n=std::snprintf(piece.data(),piece.size(),(spec+'s').c_str(),s.c_str());
            if(n>0)out.append(piece.data(),std::min<size_t>(size_t(n),piece.size()-1));
            continue;}
        case 'n':write32(read32(args),uint32_t(out.size()));args+=4;continue;
        default:out+=format.substr(start,i-start);continue;
        }
        if(n>0)out.append(buffer,std::min<size_t>(size_t(n),sizeof buffer-1));
    }
    return out;
}
void sprintfCall(){
    const std::string s=formatGuest(guestString(arg(1)),g_esp+12);
    std::memcpy(ptr(arg(0),s.size()+1),s.c_str(),s.size()+1);ret(uint32_t(s.size()),0);
}
void vsnprintfCall(){  // (buffer, count, format, va_list): -1 when it does not fit, no NUL then (MSVC)
    const std::string s=formatGuest(guestString(arg(2)),arg(3));const uint32_t count=arg(1);
    if(s.size()<count){std::memcpy(ptr(arg(0),s.size()+1),s.c_str(),s.size()+1);ret(uint32_t(s.size()),0);return;}
    if(s.size()==count){std::memcpy(ptr(arg(0),count),s.data(),count);ret(count,0);return;}
    if(count)std::memcpy(ptr(arg(0),count),s.data(),count);
    ret(uint32_t(-1),0);
}
void sscanfCall(){  // (input, format, pointers...): each conversion scanned on the host with %n to advance
    const std::string input=guestString(arg(0)),format=guestString(arg(1));
    uint32_t pointers=g_esp+12;size_t at=0;uint32_t assigned=0;bool any=false;
    for(size_t i=0;i<format.size();){
        char f=format[i];
        if(std::isspace(static_cast<unsigned char>(f))){while(at<input.size()&&std::isspace(static_cast<unsigned char>(input[at])))++at;++i;continue;}
        if(f!='%'||(i+1<format.size()&&format[i+1]=='%')){
            if(f=='%')++i;
            if(at>=input.size()||input[at]!=format[i])break;
            ++at;++i;continue;
        }
        size_t start=i++;bool suppress=false;
        if(i<format.size()&&format[i]=='*'){suppress=true;++i;}
        while(i<format.size()&&std::isdigit(static_cast<unsigned char>(format[i])))++i;
        char size=0;if(i<format.size()&&std::strchr("hlL",format[i]))size=format[i++];
        if(i>=format.size())break;
        std::string conv;
        if(format[i]=='['){size_t close=format.find(']',i+2);if(close==std::string::npos)break;conv=format.substr(i,close-i+1);i=close+1;}
        else conv=std::string(1,format[i++]);
        std::string spec=format.substr(start,i-start);
        if(at>input.size())break;
        const char* text=input.c_str()+at;int used=-1;char kind=conv[0];
        if(kind=='n'){if(!suppress){write32(read32(pointers),uint32_t(at));pointers+=4;}continue;}
        if(kind=='d'||kind=='i'||kind=='u'||kind=='x'||kind=='X'||kind=='o'){
            long long v=0;std::string host=spec;host.erase(std::remove(host.begin(),host.end(),'*'),host.end());
            host.erase(std::remove_if(host.begin(),host.end(),[](char c){return c=='h'||c=='l'||c=='L';}),host.end());
            host.insert(host.size()-1,"ll");host+="%n";
            if(std::sscanf(text,host.c_str(),&v,&used)<1||used<0)break;
            if(!suppress){uint32_t p=read32(pointers);pointers+=4;if(size=='h')as2::write16(p,uint16_t(v));else write32(p,uint32_t(v));++assigned;}
        }else if(kind=='e'||kind=='f'||kind=='g'||kind=='E'||kind=='G'){
            double v=0;std::string host=spec;host.erase(std::remove(host.begin(),host.end(),'*'),host.end());
            host.erase(std::remove_if(host.begin(),host.end(),[](char c){return c=='h'||c=='l'||c=='L';}),host.end());
            host.insert(host.size()-1,"l");host+="%n";
            if(std::sscanf(text,host.c_str(),&v,&used)<1||used<0)break;
            if(!suppress){uint32_t p=read32(pointers);pointers+=4;if(size=='l'||size=='L')writeDouble(p,v);else{float x=float(v);std::memcpy(ptr(p,4),&x,4);}++assigned;}
        }else if(kind=='s'||kind=='c'||kind=='['){
            std::string host=spec;host.erase(std::remove(host.begin(),host.end(),'*'),host.end());
            host.erase(std::remove_if(host.begin(),host.end(),[](char c){return c=='h'||c=='l'||c=='L';}),host.end());
            std::string buffer(input.size()+2,'\0');host+="%n";
            if(std::sscanf(text,host.c_str(),buffer.data(),&used)<1||used<0)break;
            if(!suppress){uint32_t p=read32(pointers);pointers+=4;
                size_t length=kind=='c'?size_t(std::max(1,std::atoi(spec.c_str()+1))):std::strlen(buffer.c_str());
                std::memcpy(ptr(p,length+(kind=='c'?0:1)),buffer.data(),length+(kind=='c'?0:1));++assigned;}
        }else break;
        at+=size_t(used);any=true;
    }
    ret(any||input.size()?assigned:uint32_t(-1),0);
}
void freadCall(){logOnce("fread");ret(0,0);}
void fwriteCall(){logOnce("fwrite");ret(0,0);}
void fflushCall(){ret(0,0);}

// --- math -----------------------------------------------------------------------------------------------
uint32_t randState=1;
void srandCall(){randState=arg(0);ret(0,0);}
void randCall(){randState=randState*214013u+2531011u;ret((randState>>16)&0x7fff,0);}
void atolCall(){ret(uint32_t(std::atol(guestString(arg(0)).c_str())),0);}
void floorCall(){retDouble(std::floor(argDouble(0)));}
void ceilCall(){retDouble(std::ceil(argDouble(0)));}
void ldexpCall(){retDouble(std::ldexp(argDouble(0),int32_t(arg(2))));}
void frexpCall(){int e=0;double v=std::frexp(argDouble(0),&e);write32(arg(2),uint32_t(e));retDouble(v);}
void modfCall(){double whole=0;double v=std::modf(argDouble(0),&whole);writeDouble(arg(2),whole);retDouble(v);}
void isnanCall(){ret(std::isnan(argDouble(0))?1:0,0);}
void finiteCall(){ret(std::isfinite(argDouble(0))?1:0,0);}
void fpclassCall(){
    const double v=argDouble(0);uint32_t c;
    switch(std::fpclassify(v)){
    case FP_NAN:c=0x0002;break;                                   // _FPCLASS_QNAN
    case FP_INFINITE:c=std::signbit(v)?0x0004:0x0200;break;       // NINF / PINF
    case FP_ZERO:c=std::signbit(v)?0x0020:0x0040;break;           // NZ / PZ
    case FP_SUBNORMAL:c=std::signbit(v)?0x0010:0x0080;break;      // ND / PD
    default:c=std::signbit(v)?0x0008:0x0100;                      // NN / PN
    }
    ret(c,0);
}
uint32_t controlWord=0x0009001fu;   // _PC_53 | _RC_NEAR | every exception masked: msvcrt's default
void controlfpCall(){controlWord=(controlWord&~arg(1))|(arg(0)&arg(1));ret(controlWord,0);}
void clearfpCall(){ret(0,0);}
// _CI* take ST0 (and ST1 for the binary ones) and leave the result in ST0.
template<double (*F)(double)> void ciUnary(){st0()=F(st0());ret(0,0);}
template<double (*F)(double,double)> void ciBinary(){const double x=st1(),y=st0();x87Pop();st0()=F(x,y);ret(0,0);}
double hostSqrt(double v){return std::sqrt(v);}
double hostExp(double v){return std::exp(v);}
double hostAsin(double v){return std::asin(v);}
double hostAcos(double v){return std::acos(v);}
double hostSinh(double v){return std::sinh(v);}
double hostCosh(double v){return std::cosh(v);}
double hostTanh(double v){return std::tanh(v);}
double hostPow(double x,double y){return std::pow(x,y);}
double hostFmod(double x,double y){return std::fmod(x,y);}

const std::map<std::string,recomp_func_t> functions={
    {"msvcrt.dll!_onexit",onexit},{"msvcrt.dll!__dllonexit",dllonexit},{"msvcrt.dll!_initterm",initterm},
    {"msvcrt.dll!_except_handler3",exceptHandler3},{"msvcrt.dll!__CxxFrameHandler",cxxFrameHandler},
    {"msvcrt.dll!?terminate@@YAXXZ",terminate},{"msvcrt.dll!abort",abortCall},{"msvcrt.dll!exit",exitCall},
    {"msvcrt.dll!_purecall",purecall},
    {"msvcrt.dll!malloc",mallocCall},{"msvcrt.dll!calloc",callocCall},{"msvcrt.dll!free",freeCall},
    {"msvcrt.dll!realloc",reallocCall},{"msvcrt.dll!??2@YAPAXI@Z",mallocCall},{"msvcrt.dll!??3@YAXPAX@Z",freeCall},
    {"msvcrt.dll!memmove",memmoveCall},{"msvcrt.dll!qsort",qsortCall},
    {"msvcrt.dll!strncpy",strncpyCall},{"msvcrt.dll!strchr",strchrCall},{"msvcrt.dll!_stricmp",stricmpCall},
    {"msvcrt.dll!_strdup",strdupCall},{"msvcrt.dll!wcslen",wcslenCall},{"msvcrt.dll!wcstombs",wcstombsCall},
    {"msvcrt.dll!atoi",atoiCall},{"msvcrt.dll!atol",atolCall},{"msvcrt.dll!atof",atofCall},{"msvcrt.dll!strtod",strtodCall},
    {"msvcrt.dll!_ultoa",ultoaCall},
    {"msvcrt.dll!isxdigit",ctypeCall<isxdigit>},{"msvcrt.dll!isalnum",ctypeCall<isalnum>},{"msvcrt.dll!isalpha",ctypeCall<isalpha>},
    {"msvcrt.dll!isspace",ctypeCall<isspace>},{"msvcrt.dll!isdigit",ctypeCall<isdigit>},
    {"msvcrt.dll!toupper",caseCall<toupper>},{"msvcrt.dll!tolower",caseCall<tolower>},
    {"msvcrt.dll!iswspace",wideCtypeCall<isspace>},{"msvcrt.dll!iswalpha",wideCtypeCall<isalpha>},
    {"msvcrt.dll!iswdigit",wideCtypeCall<isdigit>},{"msvcrt.dll!iswpunct",wideCtypeCall<ispunct>},
    {"msvcrt.dll!setlocale",setlocaleCall},{"msvcrt.dll!_strtime",strtimeCall},{"msvcrt.dll!_strdate",strdateCall},
    {"msvcrt.dll!_tempnam",tempnamCall},
    {"msvcrt.dll!sprintf",sprintfCall},{"msvcrt.dll!_vsnprintf",vsnprintfCall},{"msvcrt.dll!sscanf",sscanfCall},
    {"msvcrt.dll!fread",freadCall},{"msvcrt.dll!fwrite",fwriteCall},{"msvcrt.dll!fflush",fflushCall},
    {"msvcrt.dll!srand",srandCall},{"msvcrt.dll!rand",randCall},
    {"msvcrt.dll!floor",floorCall},{"msvcrt.dll!ceil",ceilCall},{"msvcrt.dll!ldexp",ldexpCall},{"msvcrt.dll!frexp",frexpCall},
    {"msvcrt.dll!modf",modfCall},{"msvcrt.dll!_isnan",isnanCall},{"msvcrt.dll!_finite",finiteCall},{"msvcrt.dll!_fpclass",fpclassCall},
    {"msvcrt.dll!_controlfp",controlfpCall},{"msvcrt.dll!_clearfp",clearfpCall},
    {"msvcrt.dll!_CIsqrt",ciUnary<hostSqrt>},{"msvcrt.dll!_CIexp",ciUnary<hostExp>},{"msvcrt.dll!_CIasin",ciUnary<hostAsin>},
    {"msvcrt.dll!_CIacos",ciUnary<hostAcos>},{"msvcrt.dll!_CIsinh",ciUnary<hostSinh>},{"msvcrt.dll!_CIcosh",ciUnary<hostCosh>},
    {"msvcrt.dll!_CItanh",ciUnary<hostTanh>},{"msvcrt.dll!_CIpow",ciBinary<hostPow>},{"msvcrt.dll!_CIfmod",ciBinary<hostFmod>},
};

}  // namespace fo2
