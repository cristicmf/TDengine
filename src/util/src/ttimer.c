/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "tlog.h"
#include "tsched.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"


#define TIMER_STATE_WAITING 0
#define TIMER_STATE_EXPIRED 1
#define TIMER_STATE_STOPPED 2
#define TIMER_STATE_CANCELED 3

typedef union _tmr_ctrl_t {
  char label[16];
  struct {
    // pad to ensure 'next' is the end of this union
    char               padding[16 - sizeof(union _tmr_ctrl_t*)];
    union _tmr_ctrl_t* next;
  };
} tmr_ctrl_t;

typedef struct tmr_obj_t {
  uintptr_t         id;
  tmr_ctrl_t*       ctrl;
  struct tmr_obj_t* mnext;
  struct tmr_obj_t* prev;
  struct tmr_obj_t* next;
  uint16_t          slot;
  uint8_t           wheel;
  uint8_t           state;
  uint8_t           refCount;
  uint8_t           reserved1;
  uint16_t          reserved2;
  union {
    int64_t expireAt;
    int64_t executedBy;
  };
  TAOS_TMR_CALLBACK fp;
  void*             param;
} tmr_obj_t;

typedef struct timer_list_t {
  int64_t    lockedBy;
  tmr_obj_t* timers;
} timer_list_t;

typedef struct timer_map_t {
  uint32_t      size;
  uint32_t      count;
  timer_list_t* slots;
} timer_map_t;

typedef struct time_wheel_t {
  pthread_mutex_t mutex;
  int64_t         nextScanAt;
  uint32_t        resolution;
  uint16_t        size;
  uint16_t        index;
  tmr_obj_t**     slots;
} time_wheel_t;

uint32_t tmrDebugFlag = DEBUG_ERROR | DEBUG_WARN | DEBUG_FILE;
uint32_t taosMaxTmrCtrl = 512;

static pthread_once_t  tmrModuleInit = PTHREAD_ONCE_INIT;
static pthread_mutex_t tmrCtrlMutex;
static tmr_ctrl_t*     tmrCtrls;
static tmr_ctrl_t*     unusedTmrCtrl = NULL;
static void*           tmrQhandle;
static int             numOfTmrCtrl = 0;

int taosTmrThreads = 1;

static uintptr_t nextTimerId = 0;

static time_wheel_t wheels[] = {
    {.resolution = MSECONDS_PER_TICK, .size = 4096},
    {.resolution = 1000, .size = 1024},
    {.resolution = 60000, .size = 1024},
};
static timer_map_t timerMap;

static uintptr_t getNextTimerId() {
  uintptr_t id;
  do {
    id = atomic_add_fetch_ptr(&nextTimerId, 1);
  } while (id == 0);
  return id;
}

static void timerAddRef(tmr_obj_t* timer) { atomic_add_fetch_8(&timer->refCount, 1); }

static void timerDecRef(tmr_obj_t* timer) {
  if (atomic_sub_fetch_8(&timer->refCount, 1) == 0) {
    free(timer);
  }
}

static void lockTimerList(timer_list_t* list) {
  int64_t tid = taosGetPthreadId();
  int       i = 0;
  while (atomic_val_compare_exchange_64(&(list->lockedBy), 0, tid) != 0) {
    if (++i % 1000 == 0) {
      sched_yield();
    }
  }
}

static void unlockTimerList(timer_list_t* list) {
  int64_t tid = taosGetPthreadId();
  if (atomic_val_compare_exchange_64(&(list->lockedBy), tid, 0) != tid) {
    assert(false);
    tmrError("%d trying to unlock a timer list not locked by current thread.", tid);
  }
}

static void addTimer(tmr_obj_t* timer) {
  timerAddRef(timer);
  timer->wheel = tListLen(wheels);

  uint32_t      idx = (uint32_t)(timer->id % timerMap.size);
  timer_list_t* list = timerMap.slots + idx;

  lockTimerList(list);
  timer->mnext = list->timers;
  list->timers = timer;
  unlockTimerList(list);
}

static tmr_obj_t* findTimer(uintptr_t id) {
  tmr_obj_t* timer = NULL;
  if (id > 0) {
    uint32_t      idx = (uint32_t)(id % timerMap.size);
    timer_list_t* list = timerMap.slots + idx;
    lockTimerList(list);
    for (timer = list->timers; timer != NULL; timer = timer->mnext) {
      if (timer->id == id) {
        timerAddRef(timer);
        break;
      }
    }
    unlockTimerList(list);
  }
  return timer;
}

static void removeTimer(uintptr_t id) {
  tmr_obj_t*    prev = NULL;
  uint32_t      idx = (uint32_t)(id % timerMap.size);
  timer_list_t* list = timerMap.slots + idx;
  lockTimerList(list);
  for (tmr_obj_t* p = list->timers; p != NULL; p = p->mnext) {
    if (p->id == id) {
      if (prev == NULL) {
        list->timers = p->mnext;
      } else {
        prev->mnext = p->mnext;
      }
      timerDecRef(p);
      break;
    }
    prev = p;
  }
  unlockTimerList(list);
}

static void addToWheel(tmr_obj_t* timer, uint32_t delay) {
  timerAddRef(timer);
  // select a wheel for the timer, we are not an accurate timer,
  // but the inaccuracy should not be too large.
  timer->wheel = tListLen(wheels) - 1;
  for (uint8_t i = 0; i < tListLen(wheels); i++) {
    time_wheel_t* wheel = wheels + i;
    if (delay < wheel->resolution * wheel->size) {
      timer->wheel = i;
      break;
    }
  }

  time_wheel_t* wheel = wheels + timer->wheel;
  timer->prev = NULL;
  timer->expireAt = taosGetTimestampMs() + delay;

  pthread_mutex_lock(&wheel->mutex);

  uint32_t idx = 0;
  if (timer->expireAt > wheel->nextScanAt) {
    // adjust delay according to next scan time of this wheel
    // so that the timer is not fired earlier than desired.
    delay = (uint32_t)(timer->expireAt - wheel->nextScanAt);
    idx = (delay + wheel->resolution - 1) / wheel->resolution;
  }

  timer->slot = (uint16_t)((wheel->index + idx + 1) % wheel->size);
  tmr_obj_t* p = wheel->slots[timer->slot];
  wheel->slots[timer->slot] = timer;
  timer->next = p;
  if (p != NULL) {
    p->prev = timer;
  }

  pthread_mutex_unlock(&wheel->mutex);
}

static bool removeFromWheel(tmr_obj_t* timer) {
  if (timer->wheel >= tListLen(wheels)) {
    return false;
  }
  time_wheel_t* wheel = wheels + timer->wheel;

  bool removed = false;
  pthread_mutex_lock(&wheel->mutex);
  // other thread may modify timer->wheel, check again.
  if (timer->wheel < tListLen(wheels)) {
    if (timer->prev != NULL) {
      timer->prev->next = timer->next;
    }
    if (timer->next != NULL) {
      timer->next->prev = timer->prev;
    }
    if (timer == wheel->slots[timer->slot]) {
      wheel->slots[timer->slot] = timer->next;
    }
    timer->wheel = tListLen(wheels);
    timer->next = NULL;
    timer->prev = NULL;
    timerDecRef(timer);
    removed = true;
  }
  pthread_mutex_unlock(&wheel->mutex);

  return removed;
}

static void processExpiredTimer(void* handle, void* arg) {
  tmr_obj_t* timer = (tmr_obj_t*)handle;
  timer->executedBy = taosGetPthreadId();
  uint8_t state = atomic_val_compare_exchange_8(&timer->state, TIMER_STATE_WAITING, TIMER_STATE_EXPIRED);
  if (state == TIMER_STATE_WAITING) {
    const char* fmt = "%s timer[id=%lld, fp=%p, param=%p] execution start.";
    tmrTrace(fmt, timer->ctrl->label, timer->id, timer->fp, timer->param);

    (*timer->fp)(timer->param, (tmr_h)timer->id);
    atomic_store_8(&timer->state, TIMER_STATE_STOPPED);

    fmt = "%s timer[id=%lld, fp=%p, param=%p] execution end.";
    tmrTrace(fmt, timer->ctrl->label, timer->id, timer->fp, timer->param);
  }
  removeTimer(timer->id);
  timerDecRef(timer);
}

static void addToExpired(tmr_obj_t* head) {
  const char* fmt = "%s adding expired timer[id=%lld, fp=%p, param=%p] to queue.";

  while (head != NULL) {
    uintptr_t id = head->id;
    tmr_obj_t* next = head->next;
    tmrTrace(fmt, head->ctrl->label, id, head->fp, head->param);

    SSchedMsg  schedMsg;
    schedMsg.fp = NULL;
    schedMsg.tfp = processExpiredTimer;
    schedMsg.ahandle = head;
    schedMsg.thandle = NULL;
    taosScheduleTask(tmrQhandle, &schedMsg);

    tmrTrace("timer[id=%lld] has been added to queue.", id);
    head = next;
  }
}

static uintptr_t doStartTimer(tmr_obj_t* timer, TAOS_TMR_CALLBACK fp, int mseconds, void* param, tmr_ctrl_t* ctrl) {
  uintptr_t id = getNextTimerId();
  timer->id = id;
  timer->state = TIMER_STATE_WAITING;
  timer->fp = fp;
  timer->param = param;
  timer->ctrl = ctrl;
  addTimer(timer);

  const char* fmt = "%s timer[id=%lld, fp=%p, param=%p] started";
  tmrTrace(fmt, ctrl->label, timer->id, timer->fp, timer->param);

  if (mseconds == 0) {
    timer->wheel = tListLen(wheels);
    timerAddRef(timer);
    addToExpired(timer);
  } else {
    addToWheel(timer, mseconds);
  }

  // note: use `timer->id` here is unsafe as `timer` may already be freed
  return id;
}

tmr_h taosTmrStart(TAOS_TMR_CALLBACK fp, int mseconds, void* param, void* handle) {
  tmr_ctrl_t* ctrl = (tmr_ctrl_t*)handle;
  if (ctrl == NULL || ctrl->label[0] == 0) {
    return NULL;
  }

  tmr_obj_t* timer = (tmr_obj_t*)calloc(1, sizeof(tmr_obj_t));
  if (timer == NULL) {
    tmrError("%s failed to allocated memory for new timer object.", ctrl->label);
    return NULL;
  }

  return (tmr_h)doStartTimer(timer, fp, mseconds, param, ctrl);
}

static void taosTimerLoopFunc(int signo) {
  int64_t now = taosGetTimestampMs();

  for (int i = 0; i < tListLen(wheels); i++) {
    // `expried` is a temporary expire list.
    // expired timers are first add to this list, then move
    // to expired queue as a batch to improve performance.
    // note this list is used as a stack in this function.
    tmr_obj_t* expired = NULL;

    time_wheel_t* wheel = wheels + i;
    while (now >= wheel->nextScanAt) {
      pthread_mutex_lock(&wheel->mutex);
      wheel->index = (wheel->index + 1) % wheel->size;
      tmr_obj_t* timer = wheel->slots[wheel->index];
      while (timer != NULL) {
        tmr_obj_t* next = timer->next;
        if (now < timer->expireAt) {
          timer = next;
          continue;
        }

        // remove from the wheel
        if (timer->prev == NULL) {
          wheel->slots[wheel->index] = next;
          if (next != NULL) {
            next->prev = NULL;
          }
        } else {
          timer->prev->next = next;
          if (next != NULL) {
            next->prev = timer->prev;
          }
        }
        timer->wheel = tListLen(wheels);

        // add to temporary expire list
        timer->next = expired;
        timer->prev = NULL;
        if (expired != NULL) {
          expired->prev = timer;
        }
        expired = timer;

        timer = next;
      }
      pthread_mutex_unlock(&wheel->mutex);
      wheel->nextScanAt += wheel->resolution;
    }

    addToExpired(expired);
  }
}

static bool doStopTimer(tmr_obj_t* timer, uint8_t state) {
  bool reusable = false;

  if (state == TIMER_STATE_WAITING) {
    if (removeFromWheel(timer)) {
      removeTimer(timer->id);
      // only safe to reuse the timer when timer is removed from the wheel.
      // we cannot guarantee the thread safety of the timr in all other cases.
      reusable = true;
    }
    const char* fmt = "%s timer[id=%lld, fp=%p, param=%p] is cancelled.";
    tmrTrace(fmt, timer->ctrl->label, timer->id, timer->fp, timer->param);
  } else if (state != TIMER_STATE_EXPIRED) {
    // timer already stopped or cancelled, has nothing to do in this case
  } else if (timer->executedBy == taosGetPthreadId()) {
    // taosTmrReset is called in the timer callback, should do nothing in this
    // case to avoid dead lock. note taosTmrReset must be the last statement
    // of the callback funtion, will be a bug otherwise.
  } else {
    assert(timer->executedBy != taosGetPthreadId());

    const char* fmt = "%s timer[id=%lld, fp=%p, param=%p] fired, waiting...";
    tmrTrace(fmt, timer->ctrl->label, timer->id, timer->fp, timer->param);

    for (int i = 1; atomic_load_8(&timer->state) != TIMER_STATE_STOPPED; i++) {
      if (i % 1000 == 0) {
        sched_yield();
      }
    }

    fmt = "%s timer[id=%lld, fp=%p, param=%p] stopped.";
    tmrTrace(fmt, timer->ctrl->label, timer->id, timer->fp, timer->param);
  }

  return reusable;
}

bool taosTmrStop(tmr_h timerId) {
  uintptr_t id = (uintptr_t)timerId;

  tmr_obj_t* timer = findTimer(id);
  if (timer == NULL) {
    tmrTrace("timer[id=%lld] does not exist", id);
    return false;
  }

  uint8_t state = atomic_val_compare_exchange_8(&timer->state, TIMER_STATE_WAITING, TIMER_STATE_CANCELED);
  doStopTimer(timer, state);
  timerDecRef(timer);

  return state == TIMER_STATE_WAITING;
}

bool taosTmrStopA(tmr_h* timerId) {
  bool ret = taosTmrStop(*timerId);
  *timerId = NULL;
  return ret;
}

bool taosTmrReset(TAOS_TMR_CALLBACK fp, int mseconds, void* param, void* handle, tmr_h* pTmrId) {
  tmr_ctrl_t* ctrl = (tmr_ctrl_t*)handle;
  if (ctrl == NULL || ctrl->label[0] == 0) {
    return NULL;
  }

  uintptr_t  id = (uintptr_t)*pTmrId;
  bool       stopped = false;
  tmr_obj_t* timer = findTimer(id);
  if (timer == NULL) {
    tmrTrace("%s timer[id=%lld] does not exist", ctrl->label, id);
  } else {
    uint8_t state = atomic_val_compare_exchange_8(&timer->state, TIMER_STATE_WAITING, TIMER_STATE_CANCELED);
    if (!doStopTimer(timer, state)) {
      timerDecRef(timer);
      timer = NULL;
    }
    stopped = state == TIMER_STATE_WAITING;
  }

  if (timer == NULL) {
    *pTmrId = taosTmrStart(fp, mseconds, param, handle);
    return stopped;
  }

  tmrTrace("%s timer[id=%lld] is reused", ctrl->label, timer->id);

  // wait until there's no other reference to this timer,
  // so that we can reuse this timer safely.
  for (int i = 1; atomic_load_8(&timer->refCount) > 1; ++i) {
    if (i % 1000 == 0) {
      sched_yield();
    }
  }

  assert(timer->refCount == 1);
  memset(timer, 0, sizeof(*timer));
  *pTmrId = (tmr_h)doStartTimer(timer, fp, mseconds, param, ctrl);

  return stopped;
}

static void taosTmrModuleInit(void) {
  tmrCtrls = malloc(sizeof(tmr_ctrl_t) * taosMaxTmrCtrl);
  if (tmrCtrls == NULL) {
    tmrError("failed to allocate memory for timer controllers.");
    return;
  }

  for (int i = 0; i < taosMaxTmrCtrl - 1; ++i) {
    tmr_ctrl_t* ctrl = tmrCtrls + i;
    ctrl->next = ctrl + 1;
  }
  unusedTmrCtrl = tmrCtrls;

  pthread_mutex_init(&tmrCtrlMutex, NULL);

  int64_t now = taosGetTimestampMs();
  for (int i = 0; i < tListLen(wheels); i++) {
    time_wheel_t* wheel = wheels + i;
    if (pthread_mutex_init(&wheel->mutex, NULL) != 0) {
      tmrError("failed to create the mutex for wheel, reason:%s", strerror(errno));
      return;
    }
    wheel->nextScanAt = now + wheel->resolution;
    wheel->index = 0;
    wheel->slots = (tmr_obj_t**)calloc(wheel->size, sizeof(tmr_obj_t*));
    if (wheel->slots == NULL) {
      tmrError("failed to allocate wheel slots");
      return;
    }
    timerMap.size += wheel->size;
  }

  timerMap.count = 0;
  timerMap.slots = (timer_list_t*)calloc(timerMap.size, sizeof(timer_list_t));
  if (timerMap.slots == NULL) {
    tmrError("failed to allocate hash map");
    return;
  }

  tmrQhandle = taosInitScheduler(10000, taosTmrThreads, "tmr");
  taosInitTimer(taosTimerLoopFunc, MSECONDS_PER_TICK);

  tmrTrace("timer module is initialized, number of threads: %d", taosTmrThreads);
}

void* taosTmrInit(int maxNumOfTmrs, int resolution, int longest, const char* label) {
  pthread_once(&tmrModuleInit, taosTmrModuleInit);

  pthread_mutex_lock(&tmrCtrlMutex);
  tmr_ctrl_t* ctrl = unusedTmrCtrl;
  if (ctrl != NULL) {
    unusedTmrCtrl = ctrl->next;
    numOfTmrCtrl++;
  }
  pthread_mutex_unlock(&tmrCtrlMutex);

  if (ctrl == NULL) {
    tmrError("%s too many timer controllers, failed to create timer controller.", label);
    return NULL;
  }

  strncpy(ctrl->label, label, sizeof(ctrl->label));
  ctrl->label[sizeof(ctrl->label) - 1] = 0;
  tmrTrace("%s timer controller is initialized, number of timer controllers: %d.", label, numOfTmrCtrl);
  return ctrl;
}

void taosTmrCleanUp(void* handle) {
  tmr_ctrl_t* ctrl = (tmr_ctrl_t*)handle;
  if (ctrl == NULL || ctrl->label[0] == 0) {
    return;
  }

  tmrTrace("%s timer controller is cleaned up.", ctrl->label);
  ctrl->label[0] = 0;

  pthread_mutex_lock(&tmrCtrlMutex);
  ctrl->next = unusedTmrCtrl;
  numOfTmrCtrl--;
  unusedTmrCtrl = ctrl;
  pthread_mutex_unlock(&tmrCtrlMutex);
}
