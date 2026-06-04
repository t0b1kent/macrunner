/* tso_litmus.c — x86-64 memory-ordering / atomics validation harness for MacRunner's TSO fix.
 * Source: ChatGPT recipe (2026-06-04). Run UNDER the translator to validate the LDAR/STLR + LSE
 * atomic + no-hoist implementation (CLAUDE-GATE-DIAGNOSIS UPDATE 5/6).
 *
 * Build with llvm-mingw:
 *   x86_64-w64-mingw32-clang -O2 -Wall reports/research/tso_litmus.c -o tso_litmus.exe
 * Run each under MacRunner:
 *   tso_litmus.exe mp | sb | spin | cas | xadd | split
 *
 * Expected / failure mapping:
 *   mp    : PASS. fail => store-release/load-acquire or no-hoist broken.
 *   spin  : PASS (terminates). fail => reader load hoisted/cached OR not ordered/visible.
 *   sb    : r1==0 && r2==0 is ALLOWED on x86 TSO. both0_seen==0 may mean over-fenced to SC (LDAR/STLR
 *           strict) — not a correctness fail, just a note to move to LDAPR for exact-ish TSO.
 *   cas   : PASS, final casword==1. fail => CMPXCHG/ZF/atomicity broken.
 *   xadd  : final counter == N*ITERS exactly. fail => LOCK XADD atomicity/old-result path broken.
 *   split : final == 40000. fail => unaligned/split-lock global helper missing or not respected.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

static volatile uint64_t x, y, flag, data, counter, casword;
static HANDLE start_evt;

static unsigned litmus_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *s) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, litmus_strlen(s), &written, NULL);
}

static void say_u64(uint64_t v) {
    char buf[32];
    unsigned i = sizeof(buf);
    buf[--i] = 0;
    if (!v) buf[--i] = '0';
    while (v) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    say(&buf[i]);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void wait_all(HANDLE *handles, int count) {
    for (int i = 0; i < count; i++) {
        if (!handles[i]) { say("FAIL thread create\n"); ExitProcess(7); }
        WaitForSingleObject(handles[i], INFINITE);
    }
}

static inline uint64_t load64(volatile uint64_t *p) {
    uint64_t v;
    __asm__ __volatile__("movq (%1), %0" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void store64(volatile uint64_t *p, uint64_t v) {
    __asm__ __volatile__("movq %1, (%0)" :: "r"(p), "r"(v) : "memory");
}
static inline void pause_x86(void) { __asm__ __volatile__("pause" ::: "memory"); }

static inline uint64_t lock_xadd64(volatile uint64_t *p, uint64_t v) {
    __asm__ __volatile__("lock xaddq %0, (%1)" : "+r"(v) : "r"(p) : "memory", "cc");
    return v;
}
static inline int lock_cmpxchg64(volatile uint64_t *p, uint64_t expected,
                                 uint64_t desired, uint64_t *old_out) {
    unsigned char z; uint64_t acc = expected;
    __asm__ __volatile__("lock cmpxchgq %3, (%2); setz %1"
                         : "+a"(acc), "=qm"(z) : "r"(p), "r"(desired) : "memory", "cc");
    *old_out = acc; return z != 0;
}

static DWORD WINAPI mp_writer(void *a){(void)a;WaitForSingleObject(start_evt,INFINITE);store64(&data,42);store64(&flag,1);return 0;}
static DWORD WINAPI mp_reader(void *a){(void)a;WaitForSingleObject(start_evt,INFINITE);
    while(!load64(&flag)) pause_x86();
    if(load64(&data)!=42){say("FAIL mp: flag visible but data stale\n");ExitProcess(2);} return 0;}
static int test_mp(void){ data=flag=0; start_evt=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE a=CreateThread(NULL,0,mp_writer,NULL,0,NULL), b=CreateThread(NULL,0,mp_reader,NULL,0,NULL);
    SetEvent(start_evt); WaitForSingleObject(a,INFINITE); WaitForSingleObject(b,INFINITE);
    say("PASS mp\n"); return 0; }

struct sb_arg{int role; volatile uint64_t *r; HANDLE go,done;};
static DWORD WINAPI sb_thread(void *p){ struct sb_arg *a=(struct sb_arg*)p;
    for(;;){ WaitForSingleObject(a->go,INFINITE); ResetEvent(a->go);
        if(a->role==0){store64(&x,1);*a->r=load64(&y);} else {store64(&y,1);*a->r=load64(&x);}
        SetEvent(a->done);} }
static int test_sb(void){ volatile uint64_t r1=99,r2=99;
    HANDLE go0=CreateEventA(NULL,TRUE,FALSE,NULL),go1=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE d0=CreateEventA(NULL,TRUE,FALSE,NULL),d1=CreateEventA(NULL,TRUE,FALSE,NULL);
    struct sb_arg a0={0,&r1,go0,d0},a1={1,&r2,go1,d1};
    CreateThread(NULL,0,sb_thread,&a0,0,NULL); CreateThread(NULL,0,sb_thread,&a1,0,NULL);
    int both0=0;
    for(int i=0;i<200000;i++){ x=y=0;r1=r2=99; ResetEvent(d0);ResetEvent(d1);
        SetEvent(go0);SetEvent(go1); WaitForSingleObject(d0,INFINITE);WaitForSingleObject(d1,INFINITE);
        if(r1==0&&r2==0){both0++;break;} }
    say(both0 ? "PASS sb: both0_seen=1\n" : "PASS sb: both0_seen=0\n"); return 0; }

static DWORD WINAPI spin_writer(void *a){(void)a;WaitForSingleObject(start_evt,INFINITE);
    for(volatile int i=0;i<100000;i++){} store64(&flag,1); return 0;}
static DWORD WINAPI spin_reader(void *a){(void)a;WaitForSingleObject(start_evt,INFINITE); uint64_t it=0;
    while(load64(&flag)==0){ pause_x86(); if(++it>100000000ULL){say("FAIL spin: reader never saw store\n");ExitProcess(3);} } return 0;}
static int test_spin(void){ flag=0; start_evt=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE a=CreateThread(NULL,0,spin_writer,NULL,0,NULL),b=CreateThread(NULL,0,spin_reader,NULL,0,NULL);
    SetEvent(start_evt); WaitForSingleObject(a,INFINITE);WaitForSingleObject(b,INFINITE); say("PASS spin\n"); return 0; }

static DWORD WINAPI cas_worker(void *a){(void)a;WaitForSingleObject(start_evt,INFINITE);
    for(;;){ uint64_t old; if(load64(&casword)>=1) return 0; if(lock_cmpxchg64(&casword,0,1,&old)) return 0; } }
static int test_cas(void){ casword=0; start_evt=CreateEventA(NULL,TRUE,FALSE,NULL); HANDLE th[8];
    for(int i=0;i<8;i++) th[i]=CreateThread(NULL,0,cas_worker,NULL,0,NULL);
    SetEvent(start_evt); wait_all(th,8);
    if(casword!=1){say("FAIL cas\n");return 4;} say("PASS cas\n"); return 0; }

struct xadd_arg{int iters;};
static DWORD WINAPI xadd_worker(void *p){ struct xadd_arg *a=(struct xadd_arg*)p; WaitForSingleObject(start_evt,INFINITE);
    for(int i=0;i<a->iters;i++) lock_xadd64(&counter,1); return 0; }
static int test_xadd(void){ const int N=8,IT=200000; counter=0; struct xadd_arg arg={IT}; HANDLE th[8];
    start_evt=CreateEventA(NULL,TRUE,FALSE,NULL);
    for(int i=0;i<N;i++) th[i]=CreateThread(NULL,0,xadd_worker,&arg,0,NULL);
    SetEvent(start_evt); wait_all(th,N);
    uint64_t exp=(uint64_t)N*IT;
    if(counter!=exp){say("FAIL xadd got="); say_u64(counter); say(" expected="); say_u64(exp); say("\n"); return 5;}
    say("PASS xadd\n"); return 0; }

static unsigned char split_buf[128] __attribute__((aligned(64)));
static DWORD WINAPI split_worker(void *a){(void)a; volatile uint64_t *ptr=(volatile uint64_t*)(void*)(split_buf+63);
    WaitForSingleObject(start_evt,INFINITE); for(int i=0;i<10000;i++) lock_xadd64(ptr,1); return 0; }
static int test_split(void){ for(int i=0;i<128;i++) split_buf[i]=0; volatile uint64_t *ptr=(volatile uint64_t*)(void*)(split_buf+63);
    HANDLE th[4]; start_evt=CreateEventA(NULL,TRUE,FALSE,NULL);
    for(int i=0;i<4;i++) th[i]=CreateThread(NULL,0,split_worker,NULL,0,NULL);
    SetEvent(start_evt); wait_all(th,4);
    uint64_t got = load64(ptr);
    if(got!=40000){say("FAIL split-lock xadd got="); say_u64(got); say(" expected=40000\n"); return 6;}
    say("PASS split-lock xadd\n"); return 0; }

int main(int argc,char**argv){ int rc; char mode[16]; const char *arg = NULL;
    if(argc >= 2) arg = argv[1];
    else if(GetEnvironmentVariableA("TSO_LITMUS_MODE", mode, sizeof(mode))) arg = mode;
    if(!arg){say("usage: tso_litmus.exe mp|sb|spin|cas|xadd|split\n");ExitProcess(1);}
    if(streq(arg,"mp"))rc = test_mp(); else if(streq(arg,"sb"))rc = test_sb();
    else if(streq(arg,"spin"))rc = test_spin(); else if(streq(arg,"cas"))rc = test_cas();
    else if(streq(arg,"xadd"))rc = test_xadd(); else if(streq(arg,"split"))rc = test_split();
    else {say("unknown test\n"); rc = 1;}
    ExitProcess((UINT)rc);
    return rc; }
