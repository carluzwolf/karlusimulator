#!/bin/bash
# Recompila o firmware (.ino) para WebAssembly e atualiza o starboy_sim.js
# Uso:  ./build.sh caminho/do/starboy_v13_ble.ino
# Precisa de: clang (com alvo wasm32), wasm-ld, python3.
set -e
INO="${1:-starboy_v13_ble.ino}"
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$HERE/.build"; rm -rf "$W"; mkdir -p "$W/shim"; cp "$INO" "$W/starboy.ino"; cp "$HERE/sim.cpp" "$W/"
cd "$W/shim"
cat > math.h <<'EOF_SHIM'
#pragma once
#define IMP(n) __attribute__((import_module("env"), import_name(n)))
extern "C" {
IMP("sinf") float sinf(float); IMP("cosf") float cosf(float); IMP("tanf") float tanf(float);
IMP("atan2f") float atan2f(float,float); IMP("atanf") float atanf(float);
IMP("expf") float expf(float); IMP("logf") float logf(float); IMP("powf") float powf(float,float);
}
static inline float sqrtf(float x){ return __builtin_sqrtf(x); }
static inline float fabsf(float x){ return __builtin_fabsf(x); }
static inline float floorf(float x){ return __builtin_floorf(x); }
static inline float ceilf(float x){ return __builtin_ceilf(x); }
static inline float truncf(float x){ return __builtin_truncf(x); }
static inline float fmaxf(float a,float b){ return a>b?a:b; }
static inline float fminf(float a,float b){ return a<b?a:b; }
static inline float fmodf(float x,float y){ return x - __builtin_truncf(x/y)*y; }
static inline float hypotf(float a,float b){ return __builtin_sqrtf(a*a+b*b); }
static inline float roundf(float x){ return x<0 ? -__builtin_floorf(-x+0.5f) : __builtin_floorf(x+0.5f); }
static inline long lroundf(float x){ return (long)roundf(x); }
#define M_PI 3.14159265358979323846
EOF_SHIM
cat > string.h <<'EOF_SHIM'
#pragma once
#include <stddef.h>
extern "C" { void *memcpy(void*,const void*,size_t); void *memset(void*,int,size_t); void *memmove(void*,const void*,size_t); size_t strlen(const char*); int memcmp(const void*,const void*,size_t); }
EOF_SHIM
cat > stdlib.h <<'EOF_SHIM'
#pragma once
#include <stddef.h>
extern "C" { void *malloc(size_t); void free(void*); }
EOF_SHIM
cat > initializer_list <<'EOF_SHIM'
#pragma once
#include <stddef.h>
namespace std { template<class T> class initializer_list { const T *_b; size_t _n;
 public: typedef T value_type; typedef const T* iterator; typedef const T* const_iterator;
 constexpr initializer_list(const T *b, size_t n): _b(b), _n(n) {}
 constexpr initializer_list(): _b(0), _n(0) {}
 constexpr size_t size() const { return _n; } constexpr const T *begin() const { return _b; } constexpr const T *end() const { return _b + _n; } }; }
EOF_SHIM
cat > Arduino.h <<'EOF_SHIM'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define ADC_11db 3
#define MSBFIRST 1
#define SPI_MODE0 0
typedef uint8_t byte;
void pinMode(int,int); void digitalWrite(int,int); int digitalRead(int); int analogRead(int);
void analogReadResolution(int); void analogSetAttenuation(int);
uint32_t millis(); uint32_t micros(); void delay(uint32_t); void delayMicroseconds(uint32_t);
long random(long); long random(long,long); void randomSeed(unsigned long);
EOF_SHIM
cat > SPI.h <<'EOF_SHIM'
#pragma once
#include <Arduino.h>
struct SPISettings { SPISettings(uint32_t=0,int=0,int=0){} };
struct SPIClass { void begin(int,int,int,int); void beginTransaction(SPISettings); uint8_t transfer(uint8_t); void writeBytes(const uint8_t*,uint32_t); };
extern SPIClass SPI;
EOF_SHIM
cat > BLEDevice.h <<'EOF_SHIM'
#pragma once
#include <Arduino.h>
#include <string.h>
class BLEServer; class BLEService; class BLECharacteristic;
class BLEDescriptor { public: virtual ~BLEDescriptor(){} };
class BLE2902 : public BLEDescriptor {};
class BLEServerCallbacks { public: virtual ~BLEServerCallbacks(){} virtual void onConnect(BLEServer*){} virtual void onDisconnect(BLEServer*){} };
class BLECharacteristicCallbacks { public: virtual ~BLECharacteristicCallbacks(){} virtual void onWrite(BLECharacteristic*){} };
class BLECharacteristic {
 public:
  static const int PROPERTY_READ=1, PROPERTY_WRITE=2, PROPERTY_NOTIFY=4, PROPERTY_WRITE_NR=8;
  uint8_t val[64]; size_t len; BLECharacteristicCallbacks *cb;
  void setCallbacks(BLECharacteristicCallbacks *c){ cb=c; }
  void addDescriptor(BLEDescriptor*){}
  void setValue(uint8_t *d, size_t n){ if(n>64)n=64; memcpy(val,d,n); len=n; }
  void notify();
  uint8_t *getData(){ return val; } size_t getLength(){ return len; }
};
class BLEService { public: BLECharacteristic *createCharacteristic(const char*, int); void start(){} };
class BLEAdvertising { public: void addServiceUUID(const char*){} void setScanResponse(bool){} };
class BLEServer { public: BLEServerCallbacks *cb; void setCallbacks(BLEServerCallbacks *c){ cb=c; } BLEService *createService(const char*); };
class BLEDevice { public: static void init(const char*); static BLEServer *createServer(); static BLEAdvertising *getAdvertising(); static void startAdvertising(){} };
EOF_SHIM
for f in BLEServer BLEUtils BLE2902; do printf '#pragma once\n#include <BLEDevice.h>\n' > $f.h; done
cd "$W"
INC=$(clang -print-resource-dir)/include
clang++ --target=wasm32 -std=c++17 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffreestanding -nostdinc -nostdlib++ -isystem shim -isystem "$INC" -mbulk-memory -I. -c sim.cpp -o sim.o
wasm-ld --no-entry --allow-undefined --export-memory -z stack-size=262144 --initial-memory=4194304 --gc-sections -o starboy.wasm sim.o
python3 -I - "$W" "$HERE" <<'EOF_PY'
import sys,re,json,base64
W,H=sys.argv[1],sys.argv[2]
old=open(H+'/starboy_sim.js',encoding='utf-8').read()
d=json.loads(old[old.index('=')+1:].rstrip().rstrip(';'))
fixed=["Sorriso","Sorrisinho","Pensando","Bocejo","Risada","Triste","Surpresa","Cambalhota","Não","Sim","Curioso","Piscadela","Vesgo","Resmungando","Sonolento","Troca de rosto"]
names=list(fixed);k=0
for l in open(W+'/starboy.ino',encoding='utf-8'):
    if 'RXT[n++]' in l:
        m=re.search(r'//\s*\d+\s+(.*?)\s*$',l); n=m.group(1).strip() if m else 'R-%d'%k
        n=re.sub(r'\s*\(.*$','',n).strip(); n=re.sub(r'^(R-\d+|S-\d+)\s+','',n)
        names.append(n[:1].upper()+n[1:]); k+=1
d['names']=names
d['wasm']=base64.b64encode(open(W+'/starboy.wasm','rb').read()).decode()
open(H+'/starboy_sim.js','w',encoding='utf-8').write('window.STARBOY='+json.dumps(d,ensure_ascii=False)+';\n')
print('ok:',len(names),'reacoes,',len(d['wasm'])*3//4,'bytes de wasm')
EOF_PY
rm -rf "$W"
