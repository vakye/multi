
// NOTE(vak): This was made to try out Ryan Fleury's "Multi-core by default"
// style of multithreading:
//      https://www.dgtlgrove.com/p/multi-core-by-default
//
// The essential idea is that multiple threads will be spawned into a single
// entry point function much like how a shader program would work. Then, each
// thread will retrieve its own thread ID, and then do some work.
//
// It's basically a compute shader, but on the CPU! I think this style of
// multi-core programming is very attractive since
//
//      + You don't have to implement a thread pool + job system with
//        complicated enqueue/dequeue + synchronization logic.
//
//      + Multi-core by default! Instead of your entry point being single
//        threaded, you have to face the reality that your processor has
//        multiple cores. This will guide your programming and design
//        choices so that everything works with multi-core processors (
//        which is basically the default these days). This also means
//        big performance gains since you're utilizing the entire
//        processor.
//
//      + Shader-like programming. A lot of programmers may be familiar
//        with shader progamming (check out shadertoy!), so this style of
//        "Multi-core by default" where each thread enters a shared entry
//        point is very much like a compute shader. A lot of experience from
//        writing shaders can be easily transferred to multi-core programming
//        when you have a shared entry point!

#define local static
#define persist static

#define Minimum(A, B) ((A) < (B) ? (A) : (B))
#define Maximum(A, B) ((A) > (B) ? (A) : (B))

#define KB(Amount) ((ssize)(Amount) << 10)
#define MB(Amount) ((ssize)(Amount) << 20)
#define GB(Amount) ((ssize)(Amount) << 30)
#define TB(Amount) ((ssize)(Amount) << 40)

#define AlignUp(Value, PowerOf2) (((Value) + (PowerOf2) - 1) & ~((PowerOf2) - 1))

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef s64 ssize;
typedef u64 usize;

typedef u8 b8;
typedef u32 b32;

#define true (1)
#define false (0)

#define _GNU_SOURCE
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <asm/unistd_64.h> // NOTE(vak): For syscall numbers

void LinuxBootstrap(s32 ArgCount, char* Args[], char* Envp[]);

__attribute__((naked))
void EntryPoint(void)
{
    __asm__ volatile
    (
        "mov 0 (%rsp),          %edi\n"
        "lea 8 (%rsp),          %rsi\n"
        "lea 16(%rsp, %rdi, 8), %rdx\n"
        "call LinuxBootstrap"
    );
}

local usize LinuxSyscall(
    usize NR,
    usize Arg0, usize Arg1, usize Arg2,
    usize Arg3, usize Arg4, usize Arg5
);

#define mmap(Address, Length, Protection, Flags, FileDescriptor, Offset) \
    (void*)LinuxSyscall(__NR_mmap, (usize)(Address), Length, Protection, Flags, FileDescriptor, Offset)

#define sched_getaffinity(PID, CPUSetSize, Masks) \
    (s32)LinuxSyscall(__NR_sched_getaffinity, PID, CPUSetSize, (usize)(Masks), 0, 0, 0)

#define exit(Code) \
    (void)LinuxSyscall(__NR_exit, Code, 0, 0, 0, 0, 0)

#define exit_group(Code) \
    (void)LinuxSyscall(__NR_exit_group, Code, 0, 0, 0, 0, 0)

#define nanosleep(Duration, Remainder) \
    LinuxSyscall(__NR_nanosleep, (usize)(Duration), (usize)(Remainder), 0, 0, 0, 0)

#define futex_wait(FutexPtr, Expect) \
    LinuxSyscall(__NR_futex, (usize)(FutexPtr), 0, Expect, 0, 0, 0)

#define futex_wake(FutexPtr) \
    LinuxSyscall(__NR_futex, (usize)(FutexPtr), 1, 0x7fffffff, 0, 0, 0)

local u8 LinuxEntry(void* Parameters);

// NOTE(vak): Big thanks to Chris Wellons for showing how to do threading
// on Linux!
//
// Chris Wellons' "Practical libc-free threading on Linux"
//      https://nullprogram.com/blog/2023/03/23/

typedef struct 
{
    void*   JumpToEntry;
    void*   Function;
    void*   Parameters;
    s32     JoinFutex;
} linux_thread_stack_header;

__attribute__((naked))
local void LinuxPerformCloneSyscall(void* StackPointerAt)
{
    __asm__ volatile
    (
        "mov %%rdi, %%rsi\n"
        "mov $0x50f00, %%edi\n"
        "mov $56, %%eax\n"
        "syscall\n"
        "mov %%rsp, %%rdi\n"
        "ret\n"
        :::
        "rax", "rcx", "rsi", "rdi", "r11", "memory"
    );
}

typedef struct
{
    int* JoinFutex;
} linux_thread;

local void LinuxThreadEntry(linux_thread_stack_header* Header);

local linux_thread LinuxLaunchThread(void* Function, usize StackSize, void* Parameters)
{
    linux_thread Thread = {0};

    u8* StackBottom = (u8*)mmap(0, StackSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    u8* StackTop    = StackBottom + StackSize;

    void* StackPointerAt = (StackTop - AlignUp(sizeof(linux_thread_stack_header), 16));

    linux_thread_stack_header* Header = (linux_thread_stack_header*)StackPointerAt;

    Header->JumpToEntry = (void*)LinuxThreadEntry;
    Header->Function    = Function;
    Header->Parameters  = Parameters;
    Header->JoinFutex   = 0;

    Thread.JoinFutex = &Header->JoinFutex;

    LinuxPerformCloneSyscall(StackPointerAt);

    return (Thread);
}

typedef u8 linux_thread_callback(void* Parameters);

local void LinuxThreadEntry(linux_thread_stack_header* Header)
{
    linux_thread_callback* Function = (linux_thread_callback*)Header->Function;

    u8 ExitCode = Function(Header->Parameters);

    __atomic_store_n(&Header->JoinFutex, 1, __ATOMIC_SEQ_CST);
    futex_wake(&Header->JoinFutex);

    exit(ExitCode);
}

local void LinuxJoinThread(linux_thread Thread)
{
    futex_wait(Thread.JoinFutex, 0);
}

typedef struct
{
    u32 ThreadCount;
    u32 Sum;
} linux_state;

local linux_state Linux = {0};

void LinuxBootstrap(s32 ArgCount, char* Args[], char* Envp[])
{
    Linux.ThreadCount = 1;

    {
        cpu_set_t Set = {0};

        sched_getaffinity(0, sizeof(Set), &Set);

        u32 MaskCount = sizeof(Set) / sizeof(__cpu_mask);
        u32 BitCount = sizeof(__cpu_mask) * 8;

        u32 Counted = 0;

        for (u32 MaskIndex = 0; MaskIndex < MaskCount; MaskIndex++)
        {
            u32 Mask = Set.__bits[MaskIndex];

            for (u32 BitIndex = 0; BitIndex < BitCount; BitIndex++)
                Counted += ((Mask & (1ull << BitIndex)) != 0);
        }

        Linux.ThreadCount = Maximum(1, Counted);
    }

    linux_thread* Threads = mmap(0, Linux.ThreadCount * sizeof(linux_thread), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);

    for (usize ThreadIndex = 1; ThreadIndex < Linux.ThreadCount; ThreadIndex++)
    {
        void* Function      = (void*)(LinuxEntry);
        usize StackSize     = KB(512);
        void* Parameters    = (void*)(ThreadIndex);

        Threads[ThreadIndex] = LinuxLaunchThread(Function, StackSize, Parameters);
    }

    u8 ExitCode = 0;

    {
        usize MainThreadIndex = 0;
        void* Parameters = (void*)(MainThreadIndex);
        ExitCode = LinuxEntry(Parameters);
    }

    for (usize ThreadIndex = 1; ThreadIndex < Linux.ThreadCount; ThreadIndex++)
    {
        LinuxJoinThread(Threads[ThreadIndex]);
    }

    exit_group(ExitCode);
}

#define SUM_RANGE (1090)

local void BarrierSync(void)
{
    persist u32 ArrivedCount = 0;
    persist u32 ExitedCount = 0;

    __atomic_fetch_add(&ArrivedCount, 1, __ATOMIC_SEQ_CST);
    while (ArrivedCount < Linux.ThreadCount);

    __atomic_fetch_add(&ExitedCount, 1, __ATOMIC_SEQ_CST);
    while (ExitedCount < Linux.ThreadCount);

    __atomic_fetch_sub(&ArrivedCount, 1, __ATOMIC_SEQ_CST);
    while (ArrivedCount > 0);

    __atomic_fetch_sub(&ExitedCount, 1, __ATOMIC_SEQ_CST);
    while (ExitedCount > 0);
}

local u8 LinuxEntry(void* Parameters)
{
    usize ThreadIndex = (usize)(Parameters);

    u8 ExitCode = ThreadIndex;

    b32 IsLastThread    = (ThreadIndex + 1 == Linux.ThreadCount);
    u32 RangePerBatch   = SUM_RANGE / Linux.ThreadCount;
    u32 Leftover        = SUM_RANGE - (RangePerBatch * Linux.ThreadCount);

    u32 Start   = ThreadIndex * RangePerBatch;
    u32 End     = Start + RangePerBatch + (Leftover * IsLastThread);
    u32 ThisSum = 0;

    for (u32 Value = Start; Value < End; Value++)
    {
        ThisSum += Value;
    }

    __atomic_fetch_add(&Linux.Sum, ThisSum, __ATOMIC_SEQ_CST);

    BarrierSync();
    BarrierSync();

    int BreakHere = 5;

    BarrierSync();
    BarrierSync();

    return (ExitCode);
}

local usize LinuxSyscall(
    usize NR,
    usize Arg0, usize Arg1, usize Arg2,
    usize Arg3, usize Arg4, usize Arg5
)
{
    usize Result = 0;

    register usize R10 __asm__ ("r10")  = Arg3;
    register usize R8  __asm__ ("r8")   = Arg4;
    register usize R9  __asm__ ("r9")   = Arg5;

    __asm__ volatile
    (
        "syscall" :
        "=a"(Result) :
        "a"(NR),
        "D"(Arg0),
        "S"(Arg1),
        "d"(Arg2),
        "r"(R10),
        "r"(R8),
        "r"(R9) :
        "memory", "rcx", "r11"
    );

    return (Result);
}

void* memset(void* DestInit, s32 Byte, usize Size)
{
    u8* Dest = (u8*)DestInit;

    while (Size--)
        *Dest++ = (u8)Byte;

    return (DestInit);
}

