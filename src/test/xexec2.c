#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#ifdef DLFCN
#include <dlfcn.h>
#endif
#ifdef MPI
#include <mpi.h>
#endif
#ifdef HIO
#include "hio.h"
#endif

//----------------------------------------------------------------------------
// To build:    cc -O3       xexec.c -o xexec
//       or: mpicc -O3 -DMPI xexec.c -o xexec
//  on Cray:    cc -O3 -DMPI xexec.c -o xexec -dynamic
//
// Optionally add: -DDBGLEV=3 (see discussion below)
//                 -DMPI      to enable MPI functions
//                 -DDLFCN    to enable dlopen and related calls
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// Features to add: mpi sr swap, file write & read, 
// flap seq validation, more flexible fr striding
//----------------------------------------------------------------------------

char * help = 
  "xexec - universal testing executable.  Processes command line arguments\n"
  "        in sequence to control actions.\n"
  "        Version 0.8.4 " __DATE__ " " __TIME__ "\n" 
  "\n"
  "  Syntax:  xexec -h | [ action [param ...] ] ...\n"
  "\n"
  "  Where valid actions and their parameters are:"
  "\n"
  "  v <level>     set verbosity level\n"
  "  d <level>     set debug message level\n"
  "  im <file>     imbed a file of actions at this point, - means stdin\n"
  "  lc <count>    loop start; repeat the following actions (up to the matching\n"
  "                loop end) <count> times\n"
  "  lt <seconds>  loop start; repeat the following actions (up to the matching\n"
  "                loop end) for at least <seconds>\n"
  #ifdef MPI
  "  ls <seconds>  like lt, but synchronized via MPI_Bcast from rank 0\n"
  #endif
  "  le            loop end; loops may be nested up to 16 deep\n"            
  "  o <count>     write <count> lines to stdout\n"
  "  e <count>     write <count> lines to stderr\n"
  "  s <seconds>   sleep for <seconds>\n"
  "  va <bytes>    malloc <bytes> of memory\n"
  "  vt <stride>   touch most recently allocated memory every <stride> bytes\n"
  "  vf            free most recently allocated memory\n"  
  #ifdef MPI
  "  mi            issue MPI_Init()\n" 
  "  msr <size> <stride>\n"
  "                issue MPI_Sendreceive with specified buffer <size> to and\n"
  "                from ranks <stride> above and below this rank\n"  
  "  mb            issue MPI_Barrier()\n" 
  "  mf            issue MPI_Finalize()\n" 
  #endif
  "  fi <size> <count>\n"
  "                Creates <count> blocks of <size> doubles each.  All\n"
  "                but one double in each block is populated with sequential\n"
  "                values starting with 1.0.\n"
  "  fr <rep> <stride>\n"
  "                The values in each block are added and written to the\n"
  "                remaining double in the block. The summing of the block is\n"
  "                repeated <rep> times.  All <count> blocks are processed in\n"
  "                sequence offset by <stride>. The sum of all blocks is\n"
  "                computed and compared with an expected value.\n"
  "                <size> must be 2 or greater, <count> must be 1 greater than\n"
  "                a multiple of <stride>.\n"
  "  ff            Free allocated blocks\n"
  "  hx <min> <max> <blocks> <limit> <count>\n"
  "                Perform <count> malloc/touch/free cycles on memory blocks ranging\n"
  "                in size from <min> to <max>.  Alocate no more than <limit> bytes\n"
  "                in <blocks> separate allocations.  Sequence and sizes of\n"
  "                allocations are randomized.\n"
  #ifdef DLFCN
  "  dlo <name>    Issue dlopen for specified file name\n"
  "  dls <symbol>  Issue dlsym for specified symbol in most recently opened library\n"
  "  dlc           Issue dlclose for most recently opened library\n"
  #endif
  #ifdef HIO
  "  hi  <name> <data_root>  Init hio context\n"
  "  hdo <name> <id> <flags> <mode> Dataset open\n"
  "  heo <name> <flags> Element open\n"
  "  hew <offset> <size> Element write\n"
  "  her <offset> <size> Element read\n"
  "  hec <name> <flags> Element close\n"
  "  hdc           Dataset close\n"
  "  hf            Fini\n"
  #endif
  "  k <signal>    raise <signal> (number)\n"
  "  x <status>    exit with <status>\n"
  "\n"
  "  Numbers can be specified with suffixes k, K, ki, M, Mi, G, Gi, etc.\n"
  "\n"
  "  Example action sequences:\n"
  "    v 1 d 1\n"
  "    lc 3 s 0 le\n"
  "    lt 3 s 1 le\n"
  "    o 3 e 2\n"
  "    va 1M vt 4K vf\n"
  #ifdef MPI
  "    mi mb mf\n"
  #endif
  "    fi 32 1M fr 8 1 ff\n"
  "    x 99\n"
  "\n"
  #ifndef MPI
  " MPI actions can be enabled by building with -DMPI.  See comments in source.\n"
  "\n"
  #endif
  #ifndef DLFCN
  " dlopen and related actions can be enabled by building with -DDLFCN.  See comments in source.\n"
  "\n"
  #endif
;

// Global variables
char id_string[256] = "";
int id_string_len = 0;
#ifdef MPI
int myrank, mpi_size = 0;
#endif
int verbose = 0;
int debug = 0;
int actc = 0;
char * * actv = NULL;
typedef unsigned long long int U64;

// Common subroutines and macros
#define ERRX(...) {                     \
  msg(stderr, "Error: " __VA_ARGS__);   \
  exit(12);                             \
}

//----------------------------------------------------------------------------
// DBGLEV controls which debug messages are compiled into the program.
// Set via compile option -DDBGLEV=<n>, where <n> is 0, 1, 2, or 3.
// Even when compiled in, debug messages are only issued if the current
// debug message level is set via the "d <n>" action to the level of the
// individual debug message.  Compiling in all debug messages via -DDBGLEV=3
// will noticeably impact the performance of high speed loops such as
// "vt" or "fr" due to the time required to repeatedly test the debug level.
// You have been warned !
//----------------------------------------------------------------------------

#ifndef DBGLEV
  #define DBGLEV 2
#endif

#if DBGLEV >= 1
  #define DBG1(...) if (debug >= 1) msg(stderr, "Debug: " __VA_ARGS__);
#else
  #define DBG1(...)
#endif  

#if DBGLEV >= 2
  #define DBG2(...) if (debug >= 2) msg(stderr, "Debug: " __VA_ARGS__);
#else
  #define DBG2(...)
#endif  

#if DBGLEV >= 3
  #define DBG3(...) if (debug >= 3) msg(stderr, "Debug: " __VA_ARGS__);
#else
  #define DBG3(...)
#endif  

#if DBGLEV >= 4
  #define DBG4(...) if (debug >= 4) msg(stderr, "Debug: " __VA_ARGS__);
#else
  #define DBG4(...)
#endif  

#define MAX_VERBOSE 2
#define VERB0(...) msg(stdout, __VA_ARGS__);
#define VERB1(...) if (verbose >= 1) msg(stdout, __VA_ARGS__);
#define VERB2(...) if (verbose >= 2) msg(stdout, __VA_ARGS__);
#define DIM1(array) ( sizeof(array) / sizeof(array[0]) )

#ifdef MPI
  #define MPI_CK(API) {                              \
    int mpi_rc;                                      \
    VERB1("Calling " #API);                          \
    mpi_rc = (API);                                  \
    VERB2(#API " rc: %d", mpi_rc);                   \
    if (mpi_rc != MPI_SUCCESS) {                     \
      char str[256];                                 \
      int len;                                       \
      MPI_Error_string(mpi_rc, str, &len);           \
      ERRX("%s rc:%d [%s]", (#API), mpi_rc, str);    \
    }                                                \
  }
#endif

// Serialize execution of all MPI ranks              
#ifdef MPI
  #define RANK_SERIALIZE_START                           \
    if (mpi_size > 0 && myrank != 0) {                   \
      char buf;                                          \
      MPI_Status status;                                 \
      MPI_CK(MPI_Recv(&buf, 1, MPI_CHAR, myrank - 1,     \
             MPI_ANY_TAG, MPI_COMM_WORLD, &status));     \
    }
  #define RANK_SERIALIZE_END                             \
    if (mpi_size > 0 && myrank != mpi_size - 1) {        \
      char buf;                                          \
      MPI_CK(MPI_Send(&buf, 1, MPI_CHAR, myrank + 1,     \
             0, MPI_COMM_WORLD));                        \
    }
#else
  #define RANK_SERIALIZE_START
  #define RANK_SERIALIZE_END
#endif

void msg(FILE * file, const char * format, ...) {
  va_list args;
  char msg_buf[1024];
  char time_str[64];
  time_t now;
  size_t time_len, msg_len;

  now = time(0);
  time_len = strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

  va_start(args, format);
  msg_len = vsnprintf(msg_buf, sizeof(msg_buf), format, args);
  va_end(args); 
  fprintf(file, "%s %s%s\n", time_str, id_string, msg_buf);
}

// Simple non-threadsafe timer start/stop routines
#ifdef CLOCK_REALTIME
  static struct timespec timer_start_time, timer_end_time;
  void timer_start(void) {
    if (clock_gettime(CLOCK_REALTIME, &timer_start_time)) ERRX("clock_gettime() failed: %s", strerror(errno));
  }
  double timer_end(void) { 
    if (clock_gettime(CLOCK_REALTIME, &timer_end_time)) ERRX("clock_gettime() failed: %s", strerror(errno));
    return (double)(timer_end_time.tv_sec - timer_start_time.tv_sec) + 
             (1E-9 * (double) (timer_end_time.tv_nsec - timer_start_time.tv_nsec));
  }
#else 
  // Silly Mac OS doesn't support clock_gettime :-( 
  static struct timeval timer_start_time, timer_end_time;
  void timer_start(void) {
    if (gettimeofday(&timer_start_time, NULL)) ERRX("gettimeofday() failed: %s", strerror(errno));
  }
  double timer_end(void) { 
    if (gettimeofday(&timer_end_time, NULL)) ERRX("gettimeofday() failed: %s", strerror(errno));
    return (double)(timer_end_time.tv_sec - timer_start_time.tv_sec) + 
             (1E-6 * (double) (timer_end_time.tv_usec - timer_start_time.tv_usec));
  }
#endif
  
void get_id() {
  char * p;
  char tmp_id[sizeof(id_string)];
  int rc;

  rc = gethostname(tmp_id, sizeof(tmp_id));
  if (rc != 0) ERRX("gethostname rc: %d %s", rc, strerror(errno));
  p = strchr(tmp_id, '.');
  if (p) *p = '\0';

  # ifdef MPI
  mpi_size = 0;
  { int mpi_init_flag, mpi_final_flag;
    MPI_Initialized(&mpi_init_flag);
    if (mpi_init_flag) {
      MPI_Finalized(&mpi_final_flag);
      if (! mpi_final_flag) {
        MPI_CK(MPI_Comm_rank(MPI_COMM_WORLD, &myrank));
        sprintf(tmp_id+strlen(tmp_id), ".%d", myrank);
        MPI_CK(MPI_Comm_size(MPI_COMM_WORLD, &mpi_size));
        sprintf(tmp_id+strlen(tmp_id), "/%d", mpi_size);
      }
    }
  }
  #endif
  strcat(tmp_id, " ");
  strcpy(id_string, tmp_id);
  id_string_len = strlen(id_string);
}

U64 getU64(char * num, int actn) {
  long long int n;
  char * endptr;
  DBG3("getU64 num: %s", num);
  errno = 0;
  n = strtoll(num, &endptr, 0);
  if (errno != 0) ERRX("action %d, invalid integer \"%s\"", actn, num);
  if (n < 0) ERRX("action %d, negative integer \"%s\"", actn, num);
  if (*endptr == '\0');
  else if (!strcmp("k",  endptr)) n *= 1000;
  else if (!strcmp("K",  endptr)) n *= 1024;
  else if (!strcmp("ki", endptr)) n *= 1024;
  else if (!strcmp("M",  endptr)) n *= (1000 * 1000);
  else if (!strcmp("Mi", endptr)) n *= (1024 * 1024);
  else if (!strcmp("G",  endptr)) n *= (1000 * 1000 * 1000);
  else if (!strcmp("Gi", endptr)) n *= (1024 * 1024 * 1024);
  else if (!strcmp("T",  endptr)) n *= (1ll * 1000 * 1000 * 1000 * 1000);
  else if (!strcmp("Ti", endptr)) n *= (1ll * 1024 * 1024 * 1024 * 1024);
  else if (!strcmp("P",  endptr)) n *= (1ll * 1000 * 1000 * 1000 * 1000 * 1000);
  else if (!strcmp("Pi", endptr)) n *= (1ll * 1024 * 1024 * 1024 * 1024 * 1024);
  else ERRX("action %d, invalid integer \"%s\"", actn, num);
  return n;
}


//----------------------------------------------------------------------------
// Action handler definitions
//----------------------------------------------------------------------------
typedef union pval {
  U64 u;
  char * s;
} pval;

// Many action handlers, use macro to keep function defs in sync
#define MAX_PARAM 5
#define ACTION_HAND(name) void (name)(int run, char * action, int * pactn, pval v0, pval v1, pval v2, pval v3, pval v4)
typedef ACTION_HAND(action_hand);

//----------------------------------------------------------------------------
// Action handler functions
//----------------------------------------------------------------------------
ACTION_HAND(verbose_hand) {
  verbose = v0.u;
  if (verbose > MAX_VERBOSE) ERRX("Verbosity level %d > maximum %d", verbose, MAX_VERBOSE);
  if (run) msg(stdout, "Verbosity level is now %d", verbose);
}

ACTION_HAND(debug_hand) {
  debug = v0.u;
  if (debug > DBGLEV) ERRX("debug level %d > maximum %d."
                            " Rebuild with -DDBGLEV=<n> to increase"
                            " (see comments in source.)", debug, DBGLEV);
  msg(stdout, "Debug message level is now %d; run: %d", debug, run);
}

void add2actv(int n, char * * newact) {
  if (n == 0) return;
  actv = realloc(actv, (actc + n) * sizeof(char *));
  if (!actv) ERRX("actv realloc error: %s", strerror(errno));
  memcpy(actv + actc, newact, n * sizeof(char *));
  actc += n;
}

ACTION_HAND(imbed_hand) {
  DBG1("imbed_hand fn: \"%s\"", v0.s);
  if (!run) {
    // Open file and read into buffer
    FILE * file;
    if (!strcmp(v0.s, "-")) file = stdin;
    else file = fopen(v0.s, "r");
    if (!file) ERRX("unable to open file %s: %s", v0.s, strerror(errno));
    #define BUFSZ 1024*1024
    void * p = malloc(BUFSZ);
    if (!p) ERRX("malloc error: %s", strerror(errno));
    size_t size;
    size = fread(p, 1, BUFSZ, file);
    DBG1("fread %s returns %d", v0.s, size);
    if (ferror(file)) ERRX("error reading file %s %d %s", v0.s, ferror(file), strerror(ferror(file)));
    if (!feof(file)) ERRX("imbed file %s larger than buffer (%d bytes)", v0.s, BUFSZ);
    fclose(file);
    p = realloc(p, size);
    if (!p) ERRX("realloc error: %s", strerror(errno));

    // Save old actc / actv, copy up through current action into new actc / actv
    int old_actc = actc;
    char * * old_actv = actv;
    actc = 0;
    actv = NULL;
    add2actv(*pactn+1, old_actv);

    // tokenize buffer, append to actc / actv
    char * sep = " \t\n\f\r";
    char * a = strtok(p, sep);
    while (a) {
      DBG2("add tok: \"%s\" actc: %d", a, actc);
      add2actv(1, &a);
      a = strtok(NULL, sep);
    }
   
    // append remainder of old actc / actv to new 
    add2actv(old_actc - *pactn - 1, &old_actv[*pactn + 1]);
    free(old_actv);
  } else {
    VERB1("Imbed %s", v0.s);
  }
}

#define MAX_LOOP 16
enum looptype {COUNT, TIME, SYNC};
struct loop_ctl {
  enum looptype type;
  int count;
  int top;
  struct timeval start;
  struct timeval end;
} lctl[MAX_LOOP+1];
struct loop_ctl * lcur = &lctl[0]; 

ACTION_HAND(loop_hand) {
  if (!strcmp(action, "lc")) {
    if (++lcur - lctl >= MAX_LOOP) ERRX("Maximum nested loop depth of %d exceeded", MAX_LOOP);
    if (run) {
      VERB1("loop count start; depth: %d top actn: %d count: %d", lcur-lctl, *pactn, v0.u);
      lcur->type = COUNT;
      lcur->count = v0.u;
      lcur->top = *pactn;
    } 
  } else if (!strcmp(action, "lt")) {
    if (++lcur - lctl >= MAX_LOOP) ERRX("Maximum nested loop depth of %d exceeded", MAX_LOOP);
    if (run) {
      VERB1("loop time start; depth: %d top actn: %d time: %d", lcur - lctl, *pactn, v0.u);
      lcur->type = TIME;
      lcur->top = *pactn;
      if (gettimeofday(&lcur->end, NULL)) ERRX("loop time start: gettimeofday() failed: %s", strerror(errno));
      lcur->end.tv_sec += v0.u;  // Save future time of loop end
    } 
  #ifdef MPI
  } else if (!strcmp(action, "ls")) {
    if (++lcur - lctl >= MAX_LOOP) ERRX("Maximum nested loop depth of %d exceeded", MAX_LOOP);
    if (run) {

      VERB1("loop sync start; depth: %d top actn: %d time: %d", lcur - lctl, *pactn, v0.u);
      lcur->type = SYNC;
      lcur->top = *pactn;
      if (myrank == 0) {
        if (gettimeofday(&lcur->end, NULL)) ERRX("loop time start: gettimeofday() failed: %s", strerror(errno));
        lcur->end.tv_sec += v0.u;  // Save future time of loop end
      } 
    } 
  #endif
  } else if (!strcmp(action, "le")) {
    if (lcur <= lctl) ERRX("loop end when no loop active - more loop ends than loop starts");
    if (!run) {
      lcur--;
    } else {
      if (lcur->type == COUNT) { // Count loop
        if (--lcur->count > 0) {
          *pactn = lcur->top;
          VERB2("loop count end, not done; depth: %d top actn: %d count: %d", lcur-lctl, lcur->top, lcur->count);
        } else {
          VERB1("loop count end, done; depth: %d top actn: %d count: %d", lcur-lctl, lcur->top, lcur->count);
          lcur--;
        }
      } else if (lcur->type == TIME) { // timed loop
        struct timeval now;
        if (gettimeofday(&now, NULL)) ERRX("loop end: gettimeofday() failed: %s", strerror(errno));
        if (now.tv_sec > lcur->end.tv_sec ||
             (now.tv_sec == lcur->end.tv_sec && now.tv_usec >= lcur->end.tv_usec) ) {
          VERB1("loop time end, done; depth: %d top actn: %d", lcur-lctl, lcur->top);
          lcur--;
        } else {
          *pactn = lcur->top;
          VERB2("loop time end, not done; depth: %d top actn: %d", lcur-lctl, lcur->top);
        }
      #ifdef MPI
      } else { // Sync loop
        int time2stop = 0;
        struct timeval now;
        if (myrank == 0) {
          if (gettimeofday(&now, NULL)) ERRX("loop end: gettimeofday() failed: %s", strerror(errno));
          if (now.tv_sec > lcur->end.tv_sec ||
               (now.tv_sec == lcur->end.tv_sec && now.tv_usec >= lcur->end.tv_usec) ) {
            VERB1("loop sync rank 0 end, done; depth: %d top actn: %d", lcur-lctl, lcur->top);
            time2stop = 1;
          } else {
            VERB1("loop sync rank 0 end, not done; depth: %d top actn: %d", lcur-lctl, lcur->top);
          }  
        }
        MPI_CK(MPI_Bcast(&time2stop, 1, MPI_INT, 0, MPI_COMM_WORLD));
        if (time2stop) {
          VERB1("loop sync end, done; depth: %d top actn: %d", lcur-lctl, lcur->top);
          lcur--;
        } else {
          *pactn = lcur->top;
          VERB2("loop sync end, not done; depth: %d top actn: %d", lcur-lctl, lcur->top);
        }
      #endif
      }
    }

  } else ERRX("internal error loop_hand invalid action: %s", action);
}

ACTION_HAND(stdout_hand) {
  if (run) {  
    U64 line;
    for (line = 1; line <= v0.u; line++) {
      // Message padded to exactly 100 bytes long.
      msg(stdout, "action %-4u stdout line %-8lu of %-8lu %*s", *pactn - 1, line, v0.u, 34 - id_string_len, "");
    }
  } 
}

ACTION_HAND(stderr_hand) {
  if (run) {  
    U64 line;
    for (line = 1; line <= v0.u; line++) {
      // Message padded to exactly 100 bytes long.
      msg(stderr, "action %-4u stderr line %-8lu of %-8lu %*s", *pactn - 1, line, v0.u, 34 - id_string_len, "");
    }
  } 
}

ACTION_HAND(sleep_hand) {
  if (run) {
    VERB1("Sleeping for %llu seconds", v0.u);
    sleep(v0.u);
  }
}

ACTION_HAND(mem_hand) {
  struct memblk {
    size_t size;
    struct memblk * prev;
  };
  static struct memblk * memptr;  
  static int memcount;

  DBG1("mem_hand start; action: %s memcount: %d", action, memcount);
  if (!strcmp(action, "va")) { // va - allocate memory
    memcount++;
    if (run) {
      size_t len = v0.u;
      struct memblk * p;
      VERB1("Calling malloc(%lld)", len);
      p = (struct memblk *)malloc(len);
      VERB2("malloc returns %p", p);
      if (p) {
        p->size = len;
        p->prev = memptr;
        memptr = p;
      } else {
        msg(stdout, "Warning, malloc returned NULL");
        memcount--;
      }
    }     
  } else if (!strcmp(action, "vt")) { // vt - touch memory
    U64 stride = v0.u;
    if (!run) {
      if (memcount <= 0) ERRX("Touch without cooresponding allocate");
    } else {
      char *p, *end_p1;
      if (memcount > 0) {
        p = (char*)memptr;
        end_p1 = p + memptr->size;
        VERB1("Touching memory at %p, length 0x%llx, stride: %lld", p, memptr->size, stride);
        while (p < end_p1) {
          if (p - (char *)memptr >= sizeof(struct memblk)) {
            DBG4("touch memptr: %p memlen: 0x%llx: end_p1: %p p: %p", memptr, memptr->size, end_p1, p);
            *p = 'x';
          }
          p += stride;
        }
      } else {
        msg(stdout, "Warning, no memory allocation to touch");
      }
    } 
  } else if (!strcmp(action, "vf")) { // vf - free memory
    if (!run) {
      if (memcount-- <= 0) ERRX("Free without cooresponding allocate");
    } else {
      if (memcount > 0) {
        struct memblk * p;
        p = memptr->prev;
        VERB1("Calling free(%p)", memptr);
        free(memptr);
        memptr = p;
        memcount--;
      } else {
        msg(stdout, "Warning, no memory allocation to free");
      }
    }
  } else ERRX("internal error mem_hand invalid action: %s", action);
}       

#ifdef MPI
ACTION_HAND(mpi_hand) {
static void *mpi_sbuf, *mpi_rbuf;
static size_t mpi_buf_len;

  if (run) {
    if (!strcmp(action, "mi")) {
      MPI_CK(MPI_Init(NULL, NULL));
      get_id();
    } else if (!strcmp(action, "msr")) {
      int len = v0.u;
      int stride = v1.u;
      MPI_Status status;
      if (mpi_buf_len != len) {
        mpi_sbuf = realloc(mpi_sbuf, len);     
        if (!mpi_sbuf) ERRX("msr realloc %d error: %s", len, strerror(errno));
        mpi_rbuf = realloc(mpi_rbuf, len);     
        if (!mpi_rbuf) ERRX("msr realloc %d error: %s", len, strerror(errno));
        mpi_buf_len = len;
      }
      int dest = (myrank + stride) % mpi_size;
      int source = (myrank - stride + mpi_size) % mpi_size;
      DBG2("msr len: %d dest: %d source: %d", len, dest, source);
      MPI_CK(MPI_Sendrecv(mpi_sbuf, len, MPI_BYTE, dest, 0,
                          mpi_rbuf, len, MPI_BYTE, source, 0,
                          MPI_COMM_WORLD, &status));
    } else if (!strcmp(action, "mb")) {
      MPI_CK(MPI_Barrier(MPI_COMM_WORLD));
    } else if (!strcmp(action, "mf")) {
      MPI_CK(MPI_Finalize());
      get_id();
    } else ERRX("internal error mpi_hand invalid action: %s", action);
  }
}
#endif


ACTION_HAND(flap_hand) {
  static double * nums;
  static U64 size, count;
  U64 i, iv;

  if (!strcmp(action, "fi")) {
    size = v0.u; 
    count = v1.u;
    DBG1("flapper init starting; size: %llu count: %llu", size, count);
    if (size<2) ERRX("flapper: size must be at least 2");

    if (run) {
      U64 N = size * count;
      int rc;

      rc = posix_memalign((void * *)&nums, 4096, N * sizeof(double));
      if (rc) ERRX("flapper: posix_memalign %d doubles failed: %s", N, strerror(rc));
  
      iv = 0;
      for (i=0; i<N; ++i) {
        if (i%size != 0) { 
          nums[i] = (double) ++iv;
          DBG4("nums[%d] = %d", i, iv);
        }
      }

    } 
  } else if (!strcmp(action, "fr")) {
    U64 rep = v0.u;
    U64 stride = v1.u;

    if (!size) ERRX("fr without prior fi");
    if ((count-1)%stride != 0) ERRX("flapper: count-1 must equal a multiple of stride");
    if (rep<1) ERRX("flapper: rep must be at least 1");
  
    if (run) {
      double sum, delta_t, predicted;
      U64 b, ba, r, d, fp_add_ct, max_val; 
      U64 N = size * count;
      DBG1("flapper run starting; rep: %llu stride: %llu", rep, stride);
      
      max_val = (size-1) * count;
      predicted = (pow((double) max_val, 2.0) + (double) max_val ) / 2 * (double)rep;
      DBG1("v: %d predicted: %f", max_val, predicted);
      fp_add_ct = (max_val * rep) + count;

      for (i=0; i<N; i+=size) {
        nums[i] = 0.0;
          DBG3("nums[%d] = %d", i, 0);
      }

      VERB1("flapper starting; size: %llu count: %llu rep: %llu stride: %llu", size, count, rep, stride);
      timer_start();

      for (b=0; b<count; ++b) {
        ba = b * stride % count;
        U64 d_sum = ba*size;
        U64 d_first = d_sum + 1;
        U64 d_lastp1 = (ba+1)*size;
        DBG3("b: %llu ba:%llu", b, ba);
        for (r=0; r<rep; ++r) {
          sum = nums[d_sum];
          for (d=d_first; d<d_lastp1; ++d) {
            sum += nums[d];
            DBG3("val: %f sum: %f", nums[d], sum)
          }  
          nums[d_sum] = sum;
        }
      }  

      sum = 0.0;
      for (d=0; d<count*size; d+=size) {
        sum += nums[d];
      } 

      delta_t = timer_end();
 
      VERB1("flapper done; predicted: %e sum: %e delta: %e", predicted, sum, sum - predicted);
      VERB1("FP Adds: %llu, time: %f Seconds, MFLAPS: %e", fp_add_ct, delta_t, (double)fp_add_ct / delta_t / 1000000.0);
    }
  } else if (!strcmp(action, "ff")) {
    if (!size) ERRX("ff without prior fi");
    size = 0;
    if (run) free(nums);
  } else ERRX("internal error flap_hand invalid action: %s", action);

}


ACTION_HAND(heap_hand) {
  U64 min = v0.u;
  U64 max = v1.u;
  U64 blocks = v2.u;
  U64 limit = v3.u;
  U64 count = v4.u;

  if (!run) {
    if (min < 1) ERRX("heapx: min < 1");
    if (min > max) ERRX("heapx: min > max");
    if (max > limit) ERRX("heapx: max > limit");
  } else {
    double min_l2 = log2(min), max_l2 = log2(max);
    double range_l2 = max_l2 - min_l2;
    U64 i, n, k, total = 0;
    int b;

    struct {
      void * ptr;
      size_t size;
    } blk [ blocks ];

    struct stat {
      U64 count;
      double atime;
      double ftime;
    } stat [ 1 + (int)log2(max) ];

    // Set up
    VERB1("heapx starting; min: %llu max: %llu blocks: %llu limit: %llu count: %llu", min, max, blocks, limit, count);

    for (n=0; n<blocks; ++n) {
      blk[n].ptr = NULL;
      blk[n].size = 0;
    }

    for (b=0; b<sizeof(stat)/sizeof(struct stat); ++b) {
      stat[b].count = 0;
      stat[b].atime = 0.0;
      stat[b].ftime = 0.0;
    }

    // Do allocations
    for (i=0; i<count; ++i) { 
      
      n = random()%blocks;
      if (blk[n].ptr) {
        VERB2("heapx: total: %llu; free %td bytes", total, blk[n].size);
        b = (int) log2(blk[n].size);
        timer_start();
        free(blk[n].ptr);
        stat[b].ftime += timer_end();
        total -= blk[n].size;
        blk[n].size = 0;
        blk[n].ptr = 0;
      }

      // blk[n].size = random()%(max - min + 1) + min;

      blk[n].size = (size_t)exp2( ((double)random() / (double)RAND_MAX * range_l2 ) + min_l2 );

      // Make sure limit will not be exceeded
      while (blk[n].size + total > limit) {
        k = random()%blocks;
        if (blk[k].ptr) {
          VERB2("heapx: total: %llu; free %td bytes", total, blk[k].size);
          b = (int) log2(blk[k].size);
          timer_start();
          free(blk[k].ptr);
          stat[b].ftime += timer_end();
          total -= blk[k].size;
          blk[k].size = 0;
          blk[k].ptr = 0;
        }
      }

      VERB2("heapx: total: %llu; malloc and touch %td bytes", total, blk[n].size);
      b = (int) log2(blk[n].size);
      timer_start();
      blk[n].ptr = malloc(blk[n].size);
      stat[b].atime += timer_end();
      if (!blk[n].ptr) ERRX("heapx: malloc %td bytes failed", blk[n].size);
      total += blk[n].size;
      stat[b].count++;
      memset(blk[n].ptr, 0xA5, blk[n].size);
    } 
  
    // Clean up remainder
    for (n=0; n<blocks; ++n) {
      if (blk[n].ptr) {
        VERB2("heapx: total: %llu; free %td bytes", total, blk[n].size);
        b = (int) log2(blk[n].size);
        timer_start();
        free(blk[n].ptr);
        stat[b].ftime += timer_end();
        total -= blk[n].size;
        blk[n].size = 0;
        blk[n].ptr = 0;
      }
    }

    // Reporting
    RANK_SERIALIZE_START
    for (b=0; b<sizeof(stat)/sizeof(struct stat); ++b) {
      if (stat[b].count > 0) {
        VERB1("heapx: bucket start: %lld count: %lld alloc_time: %.3f uS free_time %.3f uS", (long)exp2(b),
        stat[b].count, stat[b].atime*1e6/(double)stat[b].count, stat[b].ftime*1e6/(double)stat[b].count);
      }
    }
    RANK_SERIALIZE_END

  }
}


#ifdef DLFCN
static int dl_num = -1;
ACTION_HAND(dl_hand) {
  static void * handle[100];

  if (!strcmp(action, "dlo")) {
    char * name = v0.s;
    if (++dl_num >= DIM1(handle)) ERRX("Too many dlo commands, limit is %d", DIM1(handle));
    if (run) {
      DBG1("dlo name: %s dl_num: %d", name, dl_num);
      handle[dl_num] = dlopen(name, RTLD_NOW);
      VERB2("dlopen(%s) returns %p", name, handle[dl_num]);
      if (!handle[dl_num]) {
        VERB0("dlopen failed: %s", dlerror());
        dl_num--;
      }
    }  
  } else if (!strcmp(action, "dls")) {
    char * symbol = v0.s;
    if (dl_num < 0) ERRX("No currently open dynamic library");
    if (run) {
      char * error = dlerror();
      DBG1("dls symbol: %s dl_num: %d handle[dl_num]: %p", symbol, dl_num, handle[dl_num]);
      void * sym = dlsym(handle[dl_num], symbol);
      VERB2("dlsym(%s) returns %p", symbol, sym);
      error = dlerror();
      if (error) VERB0("dlsym error: %s", error);
    }
  } else if (!strcmp(action, "dlc")) {
    if (dl_num < 0) ERRX("No currently open dynamic library");
    if (run) {
      DBG1("dlc dl_num: %d handle[dl_num]: %p", dl_num, handle[dl_num]);
      int rc = dlclose(handle[dl_num]);
      VERB2("dlclose() returns %d", rc);
      if (rc) VERB0("dlclose error: %s", dlerror());
    } 
    dl_num--;
  } else ERRX("internal error dl_hand invalid action: %s", action);
}  
#endif


#ifdef HIO
static hio_context_t context = NULL;
static hio_dataset_t dataset = NULL;
static hio_element_t element = NULL;
static void * wbuf = NULL, *rbuf = NULL;
static U64 bufsz = 0;
ACTION_HAND(hio_hand) {
  int rc;
  if (!strcmp(action, "hi")) {
    char * context_name = v0.s;
    char * data_root = v1.s;
    char * tmp_str;
    char * root_var = "data_roots";
    if (run) {
      DBG2("HIO_SET_ELEMENT_UNIQUE: %lld", HIO_SET_ELEMENT_UNIQUE);
      DBG2("HIO_SET_ELEMENT_SHARED: %lld", HIO_SET_ELEMENT_SHARED);
      MPI_Comm myworld = MPI_COMM_WORLD;
      // workaround for read only context_data_root and no default context
      char * ename;
      asprintf(&ename, "HIO_context_%s_%s", context_name, root_var); 
      DBG2("setenv %s=%s", ename, data_root);
      rc = setenv(ename, data_root, 1);
      DBG2("setenv %s=%s rc:%d", ename, data_root, rc);
      DBG1("getenv(%s) returns \"%s\"", ename, getenv(ename));
      free(ename); 
      // end workaround
      DBG1("Invoking hio_init_mpi context:%s root:%s", context_name, data_root);
      rc = hio_init_mpi(&context, &myworld, NULL, NULL, context_name);
      VERB2("hio_init_mpi rc:%d context_name:%s", rc, context_name);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_init_mpi failed rc:%d", rc);
        hio_err_print_all(context, stderr, "hio_init_mpi error: ");
      } else {
        //DBG1("Invoking hio_config_set_value var:%s val:%s", root_var, data_root);
        //rc = hio_config_set_value((hio_object_t)context, root_var, data_root);
        //VERB2("hio_config_set_value rc:%d var:%s val:%s", rc, root_var, data_root);
        DBG1("Invoking hio_config_get_value var:%s", root_var);
        rc = hio_config_get_value((hio_object_t)context, root_var, &tmp_str);
        VERB2("hio_config_get_value rc:%d, var:%s value=\"%s\"", rc, root_var, tmp_str);
        if (HIO_SUCCESS == rc) {
          free(tmp_str);
        } else {  
          VERB0("hio_config_get_value failed, rc:%d var:%s", rc, root_var);
          hio_err_print_all(context, stderr, "hio_config_get_value error: ");
        }
      }
    }  
  } else if (!strcmp(action, "hdo")) {
    char * ds_name = v0.s;
    U64 ds_id = v1.u;
    U64 flags = v2.u;
    U64 mode = v3.u;
    if (run) {
      DBG1("Invoking hio_dataset_open name:%s id:%lld flags:%lld mode:%lld", ds_name, ds_id, flags, mode);
      rc = hio_dataset_open (context, &dataset, ds_name, ds_id, flags, mode);
      VERB2("hio_dataset_open rc:%d name:%s id:%lld flags:%lld mode:%lld", rc, ds_name, ds_id, flags, mode);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_dataset_open failed rc:%d name:%s id:%lld flags:%lld mode:%lld", rc, ds_name, ds_id, flags, mode);
        hio_err_print_all(context, stderr, "hio_dataset_open error: ");
      }
    }
  } else if (!strcmp(action, "heo")) {
    char * el_name = v0.s;
    U64 flags = v1.u;
    bufsz = v2.u;
    if (run) {
      DBG1("Invoking hio_element_open name:%s flags:%lld", el_name, flags);
      rc = hio_element_open (dataset, &element, el_name, flags);
      VERB2("hio_element_open rc:%d name:%s flags:%lld", rc, el_name, flags);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_elememt_open failed rc:%d name:%s flags:%lld", rc, el_name, flags);
        hio_err_print_all(context, stderr, "hio_element_open error: ");
      }

      DBG1("Invoking malloc(%d)", bufsz);
      wbuf = malloc(bufsz);
      VERB2("malloc(%d) returns %p", bufsz, wbuf); 
      if (!wbuf) VERB0("malloc(%d) failed", bufsz);

      DBG1("Invoking malloc(%d)", bufsz);
      rbuf = malloc(bufsz);
      VERB2("malloc(%d) returns %p", bufsz, rbuf); 
      if (!wbuf) VERB0("malloc(%d) failed", bufsz);
    }
  } else if (!strcmp(action, "hew")) {
    U64 offset = v0.u;
    U64 size = v1.u;
    if (!run) {
      if (size > bufsz) ERRX("hew: size > bufsz");
    } else {
      DBG1("Invoking hio_element_write ofs:%lld size:%lld", offset, size);
      rc = hio_element_write (element, offset, 0, wbuf, 1, size);
      VERB2("hio_element_write rc:%d ofs:%lld size:%lld", rc, offset, size);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_element_write failed rc:%d ofs:%lld size:%lld", rc, offset, size);
        hio_err_print_all(context, stderr, "hio_element_write error: ");
      }
    }
  } else if (!strcmp(action, "hec")) {
    if (run) {
      DBG1("Invoking hio_element_close");
      rc = hio_element_close(&element);
      VERB2("hio_element_close rc:%d", rc);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_close element_failed rc:%d", rc);
        hio_err_print_all(context, stderr, "hio_element_close error: ");
      }
      DBG1("Invoking free(%p)", wbuf);
      free(wbuf);
      wbuf = NULL;
      DBG1("Invoking free(%p)", rbuf);
      free(rbuf);
      rbuf = NULL;
      bufsz = 0;
    }
  } else if (!strcmp(action, "hdc")) {
    if (run) {
      DBG1("Invoking hio_dataset_close");
      rc = hio_dataset_close(&dataset);
      VERB2("hio_datset_close rc:%d", rc);
      if (HIO_SUCCESS != rc) {
        VERB0("hio_datset_close failed rc:%d", rc);
        hio_err_print_all(context, stderr, "hio_dataset_close error: ");
      }
    }
  } else if (!strcmp(action, "hf")) {
    if (run) {
      DBG1("Invoking hio_fini");
      rc = hio_fini(&context);
      VERB2("hio_fini rc:%d", rc);
      if (rc != HIO_SUCCESS) {
        VERB0("hio_fini failed, rc:%d", rc);
        hio_err_print_all(context, stderr, "hio_fini error: ");
      }
    }
  } else ERRX("internal error hio_hand invalid action: %s", action);
}  
#endif

ACTION_HAND(raise_hand) {
  if (run) {
    VERB1("Raising signal %d", v0.u);
    raise(v0.u);
  }
}

ACTION_HAND(exit_hand) {
  if (run) {
    VERB1("Exiting with status %d", v0.u);
    exit(v0.u);
  }
}

//----------------------------------------------------------------------------
// Argument string parsing table
//----------------------------------------------------------------------------
enum ptype { UINT, PINT, STR, NONE };

struct parse {
  char * action;
  enum ptype param[MAX_PARAM];
  action_hand * handler;  
} parse[] = { 
  {"v",   {UINT, NONE, NONE, NONE, NONE}, verbose_hand},
  {"d",   {UINT, NONE, NONE, NONE, NONE}, debug_hand},
  {"im",  {STR,  NONE, NONE, NONE, NONE}, imbed_hand},
  {"lc",  {UINT, NONE, NONE, NONE, NONE}, loop_hand},
  {"lt",  {UINT, NONE, NONE, NONE, NONE}, loop_hand},
  #ifdef MPI
  {"ls",  {UINT, NONE, NONE, NONE, NONE}, loop_hand},
  #endif
  {"le",  {NONE, NONE, NONE, NONE, NONE}, loop_hand},
  {"o",   {UINT, NONE, NONE, NONE, NONE}, stdout_hand},
  {"e",   {UINT, NONE, NONE, NONE, NONE}, stderr_hand},
  {"s",   {UINT, NONE, NONE, NONE, NONE}, sleep_hand},
  {"va",  {UINT, NONE, NONE, NONE, NONE}, mem_hand},
  {"vt",  {PINT, NONE, NONE, NONE, NONE}, mem_hand},
  {"vf",  {NONE, NONE, NONE, NONE, NONE}, mem_hand},
  #ifdef MPI
  {"mi",  {NONE, NONE, NONE, NONE, NONE}, mpi_hand},
  {"msr", {PINT, PINT, NONE, NONE, NONE}, mpi_hand},
  {"mb",  {NONE, NONE, NONE, NONE, NONE}, mpi_hand},
  {"mb",  {NONE, NONE, NONE, NONE, NONE}, mpi_hand},
  {"mf",  {NONE, NONE, NONE, NONE, NONE}, mpi_hand},
  #endif
  {"fi",  {UINT, PINT, NONE, NONE, NONE}, flap_hand},
  {"fr",  {PINT, PINT, NONE, NONE, NONE}, flap_hand},
  {"ff",  {NONE, NONE, NONE, NONE, NONE}, flap_hand},
  {"hx",  {UINT, UINT, UINT, UINT, UINT}, heap_hand},
  #ifdef DLFCN
  {"dlo", {STR,  NONE, NONE, NONE, NONE}, dl_hand},
  {"dls", {STR,  NONE, NONE, NONE, NONE}, dl_hand},
  {"dlc", {NONE, NONE, NONE, NONE, NONE}, dl_hand},
  #endif
  #ifdef HIO
  {"hi",  {STR,  STR,  NONE, NONE, NONE}, hio_hand},
  {"hdo", {STR,  UINT, UINT, UINT, NONE}, hio_hand},
  {"heo", {STR,  UINT, UINT, NONE, NONE}, hio_hand},
  {"hew", {UINT, UINT, NONE, NONE, NONE}, hio_hand},
  {"her", {UINT, UINT, NONE, NONE, NONE}, hio_hand},
  {"hec", {NONE, NONE, NONE, NONE, NONE}, hio_hand},
  {"hdc", {NONE, NONE, NONE, NONE, NONE}, hio_hand},
  {"hf",  {NONE, NONE, NONE, NONE, NONE}, hio_hand},
  #endif
  {"k",   {UINT, NONE, NONE, NONE, NONE}, raise_hand},
  {"x",   {UINT, NONE, NONE, NONE, NONE}, exit_hand}
};

//----------------------------------------------------------------------------
// Argument string parser - calls action handlers
//----------------------------------------------------------------------------
void parse_and_dispatch(int run) {
  int a = 0, aa, i, j;
  pval vals[MAX_PARAM];

  verbose = debug = 0;
  
  #ifdef DLFCN
    dl_num = -1;
  #endif  

  while ( ++a < actc ) {
    for (i = 0; i < DIM1(parse); ++i) {
      if (0 == strcmp(actv[a], parse[i].action)) {
        DBG3("match: actv[%d]: %s parse[%d].action: %s run: %d", a, actv[a], i, parse[i].action, run);
        aa = a;
        for (j = 0; j < MAX_PARAM; ++j) {
          if (parse[i].param[j] == NONE) break;
          if (actc - a <= 1) ERRX("action %d \"%s\" missing param %d", aa, actv[aa], j+1);
          switch (parse[i].param[j]) {
            case UINT:
              a++;
              vals[j].u = getU64(actv[a], a);
              break;
            case PINT:
              a++;
              vals[j].u = getU64(actv[a], a);
              if (0 == vals[j].u) ERRX("action %d \"%s\" param %d \"%s\" must be > 0", aa, actv[aa], j+1, actv[a]);
              break;
            case STR:
              vals[j].s = actv[++a];
            case NONE:
              break;
            default:
              ERRX("internal parse error parse[%d].param[%d]: %d", i, j, parse[i].param[j]);
          }
        }
        parse[i].handler(run, actv[aa], &a, vals[0], vals[1], vals[2], vals[3], vals[4]);
        break;
      }
    }
    if (i >= DIM1(parse)) ERRX("action %d: \"%s\" not recognized.", a, actv[a]);
  }
  if (lcur-lctl > 0) ERRX("Unterminated loop - more loop starts than loop ends");
}

//----------------------------------------------------------------------------
// Main - write help, call parser / dispatcher
//----------------------------------------------------------------------------
int main(int argc, char * * argv) {

  if (argc <= 1 || 0 == strncmp("-h", argv[1], 2)) {
    fputs(help, stdout);
    return 1;
  }

  get_id();

  add2actv(argc, argv); // Make initial copy of argv so im works 

  // Make two passes through args, first to check, second to run.
  parse_and_dispatch(0);
  parse_and_dispatch(1);

  return 0;
}
// --- end of xexec.c ---
