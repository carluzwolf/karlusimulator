// Simulador: roda o starboy_v13_ble.ino REAL (sem alteracoes) como WebAssembly.
// Faz o papel do ESP32 (Arduino, SPI, BLE, ADC) e do display GC9A01 (240x240 RGB565).
#include <Arduino.h>
#include <SPI.h>
#include <BLEDevice.h>
#define IMPORT(n) __attribute__((import_module("env"), import_name(n)))
#define EXPORT(n) __attribute__((export_name(n)))
IMPORT("js_frame")  void js_frame();
IMPORT("js_notify") void js_notify(const uint8_t *p, int n);

// ---------- memoria (malloc/free simples) ----------
extern "C" {
extern unsigned char __heap_base;
struct Blk { uint32_t size; uint32_t used; };
static uint8_t *heapEnd = nullptr; static uint8_t *heapTop = nullptr; static Blk *freeList[1] = {0};
void *memcpy(void *d, const void *s, size_t n){ uint8_t *a=(uint8_t*)d; const uint8_t *b=(const uint8_t*)s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, size_t n){ uint8_t *a=(uint8_t*)d; const uint8_t *b=(const uint8_t*)s; if(a<b){ while(n--) *a++=*b++; } else { a+=n; b+=n; while(n--) *--a=*--b; } return d; }
void *memset(void *d, int c, size_t n){ uint8_t *a=(uint8_t*)d; while(n--) *a++=(uint8_t)c; return d; }
int memcmp(const void *x, const void *y, size_t n){ const uint8_t *a=(const uint8_t*)x,*b=(const uint8_t*)y; for(;n;n--,a++,b++) if(*a!=*b) return *a-*b; return 0; }
size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
void *malloc(size_t n){
  n = (n+15)&~15u;
  // reaproveita bloco livre (first-fit)
  uint8_t *p = (uint8_t*)&__heap_base; if(!heapTop) heapTop = (uint8_t*)(((uintptr_t)&__heap_base+15)&~15u);
  p = (uint8_t*)(((uintptr_t)&__heap_base+15)&~15u);
  while (p < heapTop) { Blk *b=(Blk*)p; if(!b->used && b->size>=n){ b->used=1; return p+16; } p += 16 + b->size; }
  Blk *b=(Blk*)heapTop; b->size=n; b->used=1; heapTop += 16+n;
  size_t need = (size_t)heapTop; size_t have = __builtin_wasm_memory_size(0)*65536;
  if (need > have) { size_t pages = (need-have+65535)/65536; if (__builtin_wasm_memory_grow(0,pages)==(size_t)-1) return 0; }
  return (uint8_t*)b+16;
}
void free(void *q){ if(!q) return; Blk *b=(Blk*)((uint8_t*)q-16); b->used=0; }
}
void *operator new(size_t n){ return malloc(n); }
void operator delete(void *p){ free(p); }
void operator delete(void *p, size_t){ free(p); }

// ---------- relogio virtual ----------
static double vt_us = 0; static bool live = false;
static double spiBytes = 0;
void SIM_adv(double us){ if(!live) vt_us += us; }
uint32_t millis(){ SIM_adv(4); return (uint32_t)(vt_us/1000.0); }
uint32_t micros(){ SIM_adv(4); return (uint32_t)vt_us; }
void delay(uint32_t ms){ SIM_adv(ms*1000.0); }
void delayMicroseconds(uint32_t us){ SIM_adv(us); }
static uint32_t rs = 2463534242u;
static uint32_t rnd32(){ rs ^= rs<<13; rs ^= rs>>17; rs ^= rs<<5; return rs; }
long random(long m){ return m<=0 ? 0 : (long)(rnd32() % (uint32_t)m); }
long random(long a, long b){ return b<=a ? a : a + (long)(rnd32() % (uint32_t)(b-a)); }
void randomSeed(unsigned long s){ rs = s ? (uint32_t)s : 1u; }

// ---------- pinos / ADC (microfone) ----------
static int dcLevel = 1;
void pinMode(int,int){}
void digitalWrite(int pin, int v){ if(pin==1) dcLevel = v; }
int digitalRead(int){ return 1; }
void analogReadResolution(int){} void analogSetAttenuation(int){}
static uint16_t micBuf[512]; static uint32_t micIdx = 0;
int analogRead(int){ return micBuf[(micIdx++) & 511]; }

// ---------- display GC9A01 ----------
static uint16_t fb[240*240];
static int curCmd=0, argN=0; static uint8_t args[8];
static int cx0=0,cx1=239,cy0=0,cy1=239, px=0, py=0, madctl=0, invOn=0, pxCount=0; static bool haveHi=false; static uint8_t hiB=0;
static void spiByte(uint8_t b){
  if (!dcLevel) { curCmd=b; argN=0; if(b==0x2C){ px=cx0; py=cy0; haveHi=false; pxCount=0; } else if(b==0x21) invOn=1; else if(b==0x20) invOn=0; return; }
  switch (curCmd) {
    case 0x2A: if(argN<4){ args[argN++]=b; if(argN==4){ cx0=(args[0]<<8)|args[1]; cx1=(args[2]<<8)|args[3]; } } break;
    case 0x2B: if(argN<4){ args[argN++]=b; if(argN==4){ cy0=(args[0]<<8)|args[1]; cy1=(args[2]<<8)|args[3]; } } break;
    case 0x36: madctl=b; break;
    case 0x2C:
      if(!haveHi){ hiB=b; haveHi=true; }
      else {
        haveHi=false; uint16_t v=(uint16_t)((hiB<<8)|b);
        if(px>=0&&px<240&&py>=0&&py<240) fb[py*240+px]=v;
        if(++px>cx1){ px=cx0; if(++py>cy1) py=cy0; }
        int area=(cx1-cx0+1)*(cy1-cy0+1);
        if(++pxCount>=area){ pxCount=0; if(area>=240*240){ SIM_adv(20000); js_frame(); } }
      }
      break;
    default: break;
  }
}
SPIClass SPI;
void SPIClass::begin(int,int,int,int){} void SPIClass::beginTransaction(SPISettings){}
uint8_t SPIClass::transfer(uint8_t b){ spiByte(b); SIM_adv(0.2); return 0; }
void SPIClass::writeBytes(const uint8_t *p, uint32_t n){ for(uint32_t i=0;i<n;i++) spiByte(p[i]); SIM_adv(n*0.2); }

// ---------- BLE ----------
static BLEServer gSrv; static BLEService gSvc; static BLECharacteristic gChr[4]; static int gChrN=0;
void BLEDevice::init(const char*){} BLEServer *BLEDevice::createServer(){ return &gSrv; }
BLEAdvertising *BLEDevice::getAdvertising(){ static BLEAdvertising a; return &a; }
BLEService *BLEServer::createService(const char*){ return &gSvc; }
BLECharacteristic *BLEService::createCharacteristic(const char*, int){ return &gChr[gChrN++ & 3]; }
void BLECharacteristic::notify(){ js_notify(val,(int)len); }

#include "starboy.ino"

// ---------- exportacoes para o JS ----------
extern "C" {
EXPORT("sim_fb")        uint16_t *sim_fb(){ return fb; }
EXPORT("sim_mic")       uint16_t *sim_mic(){ return micBuf; }
EXPORT("sim_flags")     int sim_flags(){ return (invOn?1:0) | ((madctl&0x08)?2:0); }
EXPORT("sim_seed")      void sim_seed(uint32_t s){ randomSeed(s); }
EXPORT("sim_setup")     double sim_setup(){ live=false; setup(); return vt_us/1000.0; }
EXPORT("sim_go_live")   void sim_go_live(){ live=true; }
EXPORT("sim_loop")      void sim_loop(double ms){ vt_us=ms*1000.0; loop(); }
EXPORT("sim_vt")        double sim_vt(){ return vt_us/1000.0; }
EXPORT("sim_connect")   void sim_connect(){ if(gSrv.cb) gSrv.cb->onConnect(&gSrv); }
EXPORT("sim_disconnect")void sim_disconnect(){ if(gSrv.cb) gSrv.cb->onDisconnect(&gSrv); }
EXPORT("sim_write")     void sim_write(const uint8_t *p, int n){ BLECharacteristic *c=&gChr[0]; if(n>64)n=64; memcpy(c->val,p,n); c->len=n; if(c->cb) c->cb->onWrite(c); }
EXPORT("sim_buf")       uint8_t *sim_buf(){ static uint8_t b[64]; return b; }
}
