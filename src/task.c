/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "task.h"
#include "sched.h"
#include "event.h"
#include "main.h"
#include "lib/public/pf_string.h"
#include "lib/public/pqueue.h"
#include "lib/public/khash.h"

#include <SDL.h>
#include <assert.h>

struct delay_desc{
    uint32_t tid;
    uint32_t wake_tick;
};

struct ts_req{
    enum{
        TS_REQ_NOTIFY, 
        TS_REQ_DELAY,
    }type;
    uint32_t ticks;
};

struct ns_req{
    enum{
        NS_REQ_REGISTER,
        NS_REQ_WHOIS
    }type;
    const char *name;
};

PQUEUE_TYPE(delay, struct delay_desc)
PQUEUE_IMPL(static, delay, struct delay_desc)

KHASH_MAP_INIT_STR(tid, uint32_t)

/*****************************************************************************/
/* STATIC VARIAVBLES                                                         */
/*****************************************************************************/

static uint32_t s_ns_tid; /* write-once */
static uint32_t s_ts_tid; /* write-once */

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct result tick_notifier(void *arg)
{
    uint32_t ts_tid = Task_ParentTid();
    struct ts_req request = (struct ts_req){ .type = TS_REQ_NOTIFY };
    int resp;

    while(1) {
        Task_AwaitEvent(EVENT_60HZ_TICK);
        Task_Send(ts_tid, &request, sizeof(request), &resp, sizeof(resp));
    }
    return NULL_RESULT;
}

static void timeserver_exit(void *arg)
{
    pq_delay_t *pq = (pq_delay_t*)arg;
    pq_delay_destroy(pq);
}

static struct result timeserver_task(void *arg)
{
    pq_delay_t descs;
    pq_delay_init(&descs);
    Task_SetDestructor(timeserver_exit, &descs);

    struct future res;
    uint32_t notifier = Task_Create(0, tick_notifier, NULL, &res, 0);

    while(1) {
        struct ts_req request;
        uint32_t tid;
        int reply = 0;

        Task_Receive(&tid, &request, sizeof(request));
        uint32_t curr_tick = SDL_GetTicks();

        switch(request.type) {
        case TS_REQ_NOTIFY:
            Task_Reply(tid, &reply, sizeof(reply));
            break;
        case TS_REQ_DELAY: {
            struct delay_desc dd = (struct delay_desc){
                .tid = tid,
                .wake_tick = curr_tick + request.ticks
            };
            pq_delay_push(&descs, dd.wake_tick, dd);
            break;
        }
        default: assert(0);
        }

        /* Check if any tasks need waking */
        struct delay_desc curr;
        do{
            if(!pq_delay_pop(&descs, &curr))
                break;
            if(curr.wake_tick > curr_tick) {
                pq_delay_push(&descs, curr.wake_tick, curr);
            }else{
                Task_Reply(curr.tid, &reply, sizeof(reply));
            }
        }while(curr.wake_tick <= curr_tick);
    }

    return NULL_RESULT;
}

static void nameserver_exit(void *arg)
{
    khash_t(tid) *names = (khash_t(tid)*)arg;
    const char *key;
    uint32_t tid;
    (void)tid;

    kh_foreach(names, key, tid, {
        free((void*)key);
    });
    kh_destroy(tid, names);
}

static struct result nameserver_task(void *arg)
{
    khash_t(tid) *names = kh_init(tid);
    Task_SetDestructor(nameserver_exit, names);

    while(1) {
        struct ns_req request;
        uint32_t tid;

        Task_Receive(&tid, &request, sizeof(request));

        switch(request.type) {
        case NS_REQ_REGISTER: {
            int status;
            khiter_t k = kh_get(tid, names, request.name);
            if(k == kh_end(names)) {
                k = kh_put(tid, names, pf_strdup(request.name), &status);
                assert(status != -1 && status != 0);
            }
            kh_value(names, k) = tid;

            int reply = 0;
            Task_Reply(tid, &reply, sizeof(reply));
            break;
        }
        case NS_REQ_WHOIS: {
            uint32_t resp;
            khiter_t k = kh_get(tid, names, request.name);
            if(k == kh_end(names)) {
                resp = NULL_TID;
            }else{
                resp = kh_value(names, k);
            }
            Task_Reply(tid, &resp, sizeof(resp));
            break;
        }
        default: assert(0);
        }
    }

    return NULL_RESULT;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Task_Yield(void)
{
    Sched_Request((struct request){ .type = SCHED_REQ_YIELD });
}

void Task_Send(uint32_t tid, void *msg, size_t msglen, void *reply, size_t replylen)
{
    Sched_Request((struct request){ 
        .type = SCHED_REQ_SEND,
        .argv[0] = (uint64_t)tid,
        .argv[1] = (uint64_t)msg,
        .argv[2] = (uint64_t)msglen,
        .argv[3] = (uint64_t)reply,
        .argv[4] = (uint64_t)replylen,
    });
}

void Task_Receive(uint32_t *tid, void *msg, size_t msglen)
{
    Sched_Request((struct request){
        .type = SCHED_REQ_RECEIVE,
        .argv[0] = (uint64_t)tid,
        .argv[1] = (uint64_t)msg,
        .argv[2] = (uint64_t)msglen,
    });
}

void Task_Reply(uint32_t tid, void *reply, size_t replylen)
{
    Sched_Request((struct request){
        .type = SCHED_REQ_REPLY,
        .argv[0] = (uint64_t)tid,
        .argv[1] = (uint64_t)reply,
        .argv[2] = (uint64_t)replylen,
    });
}

uint32_t Task_MyTid(void)
{
    return Sched_Request((struct request){ .type = SCHED_REQ_MY_TID });
}

uint32_t Task_ParentTid(void)
{
    return Sched_Request((struct request){ .type = SCHED_REQ_MY_PARENT_TID });
}

void *Task_AwaitEvent(int event)
{
    return (void*)Sched_Request((struct request){
        .type = SCHED_REQ_AWAIT_EVENT,
        .argv[0] = event,
    });
}

void Task_SetDestructor(void (*destructor)(void*), void *darg)
{
    Sched_Request((struct request){
        .type = SCHED_REQ_SET_DESTRUCTOR,
        .argv[0] = (uint64_t)destructor,
        .argv[1] = (uint64_t)darg
    });
}

uint32_t Task_Create(int prio, task_t code, void *arg, struct future *result, int flags)
{
    return Sched_Request((struct request){
        .type = SCHED_REQ_CREATE,
        .argv[0] = (uint64_t)prio,
        .argv[1] = (uint64_t)code,
        .argv[2] = (uint64_t)arg,
        .argv[3] = (uint64_t)result,
        .argv[4] = (uint64_t)flags,
    });
}

bool Task_Wait(uint32_t tid)
{
    return Sched_Request((struct request){
        .type = SCHED_REQ_WAIT,
        .argv[0] = (uint64_t)tid
    });
}

void Task_Sleep(int ms)
{
    struct ts_req tr = (struct ts_req){
        .type = TS_REQ_DELAY,
        .ticks = ms
    };
    int resp;
    Task_Send(s_ts_tid, &tr, sizeof(tr), &resp, sizeof(resp));
}

void Task_Register(const char *name)
{
    struct ns_req nr = (struct ns_req){
        .type = NS_REQ_REGISTER,
        .name = name
    };
    int resp;
    Task_Send(s_ns_tid, &nr, sizeof(nr), &resp, sizeof(resp));
}

uint32_t Task_WhoIs(const char *name)
{
    struct ns_req nr = (struct ns_req){
        .type = NS_REQ_WHOIS,
        .name = name
    };
    uint32_t resp;
    Task_Send(s_ns_tid, &nr, sizeof(nr), &resp, sizeof(resp));
    return resp;
}

void Task_CreateServices(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_ns_tid = Sched_Create(0, nameserver_task, NULL, NULL, 0);
    s_ts_tid = Sched_Create(0, timeserver_task, NULL, NULL, 0);
}

