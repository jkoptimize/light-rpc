// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Tue Jul 10 17:40:58 CST 2012

#include "inc/fast_log.h"
#include <sys/syscall.h>
#include "butil/macros.h"                       // BAIDU_CASSERT
#include "butil/thread_local.h"
#include "task_group.h"                // TaskGroup
#include "task_control.h"              // TaskControl
#include "timer_thread.h"
#include "list_of_abafree_id.h"
#include "bthread.h"

namespace fast {
extern void print_task(std::ostream& os, bthread_t tid, bool enable_trace,
                       bool ignore_not_matched = false);

static bool never_set_bthread_concurrency = true;

BAIDU_CASSERT(sizeof(TaskControl*) == sizeof(std::atomic<TaskControl*>), atomic_size_match);

pthread_mutex_t g_task_control_mutex = PTHREAD_MUTEX_INITIALIZER;
// Referenced in rpc, needs to be extern.
// Notice that we can't declare the variable as atomic<TaskControl*> which
// are not constructed before main().
TaskControl* g_task_control = NULL;

extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;
EXTERN_BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group);
extern void (*g_worker_startfn)();
extern void (*g_tagged_worker_startfn)(bthread_tag_t);
extern void* (*g_create_span_func)();

inline TaskControl* get_task_control() {
    return g_task_control;
}

// Restore the startup checks formerly performed by brpc's gflags validators.
// FastBthreadConfig is startup-only; dynamic concurrency uses the public setters.
static bool validate_startup_config() {
    const auto& config = FastBthreadConfig::Get();
    if (config.bthread_concurrency < BTHREAD_MIN_CONCURRENCY ||
        config.bthread_concurrency > BTHREAD_MAX_CONCURRENCY) {
        LOG(ERROR) << "Invalid bthread_concurrency=" << config.bthread_concurrency;
        return false;
    }
    if (config.bthread_min_concurrency > 0 &&
        (config.bthread_min_concurrency < BTHREAD_MIN_CONCURRENCY ||
         config.bthread_min_concurrency > config.bthread_concurrency)) {
        LOG(ERROR) << "Invalid bthread_min_concurrency="
                   << config.bthread_min_concurrency;
        return false;
    }
    if (config.parking_lot_of_each_tag < BTHREAD_MIN_PARKINGLOT ||
        config.parking_lot_of_each_tag > BTHREAD_MAX_PARKINGLOT) {
        LOG(ERROR) << "Invalid parking_lot_of_each_tag="
                   << config.parking_lot_of_each_tag;
        return false;
    }
    return true;
}

inline TaskControl* get_or_new_task_control() {
    std::atomic<TaskControl*>* p = (std::atomic<TaskControl*>*)&g_task_control;
    TaskControl* c = p->load(std::memory_order_consume);
    if (c != NULL) {
        return c;
    }
    BAIDU_SCOPED_LOCK(g_task_control_mutex);
    c = p->load(std::memory_order_consume);
    if (c != NULL) {
        return c;
    }
    if (!validate_startup_config()) {
        return NULL;
    }
    c = new (std::nothrow) TaskControl;
    if (NULL == c) {
        return NULL;
    }
    int concurrency = FastBthreadConfig::Get().bthread_min_concurrency > 0 ?
        FastBthreadConfig::Get().bthread_min_concurrency :
        FastBthreadConfig::Get().bthread_concurrency;
    if (c->init(concurrency) != 0) {
        LOG(ERROR) << "Fail to init g_task_control";
        delete c;
        return NULL;
    }
    p->store(c, std::memory_order_release);
    return c;
}

#ifdef BRPC_BTHREAD_TRACER
BAIDU_THREAD_LOCAL TaskMeta* pthread_fake_meta = NULL;

bthread_t init_for_pthread_stack_trace() {
    if (NULL != pthread_fake_meta) {
        return pthread_fake_meta->tid;
    }

    TaskControl* c = get_task_control();
    if (NULL == c) {
        LOG(ERROR) << "TaskControl has not been created, "
                      "please use bthread_start_xxx before call this function";
        return INVALID_BTHREAD;
    }

    fast::butil::ResourceId<TaskMeta> slot;
    pthread_fake_meta = fast::butil::get_resource(&slot);
    if (BAIDU_UNLIKELY(NULL == pthread_fake_meta)) {
        LOG(ERROR) << "Fail to get TaskMeta";
        return INVALID_BTHREAD;
    }

    pthread_fake_meta->attr = BTHREAD_ATTR_PTHREAD;
    pthread_fake_meta->tid = make_tid(*pthread_fake_meta->version_butex, slot);
    // Make TaskTracer use signal trace mode for pthread.
    c->_task_tracer.set_running_status(syscall(SYS_gettid), pthread_fake_meta);

    // Release the TaskMeta at exit of pthread.
    fast::butil::thread_atexit([]() {
        // Similar to TaskGroup::task_runner.
        bool tracing;
        {
            BAIDU_SCOPED_LOCK(pthread_fake_meta->version_lock);
            tracing = TaskTracer::set_end_status_unsafe(pthread_fake_meta);
            // If resulting version is 0,
            // change it to 1 to make bthread_t never be 0.
            if (0 == ++*pthread_fake_meta->version_butex) {
                ++*pthread_fake_meta->version_butex;
            }
        }

        if (tracing) {
            // Wait for tracing completion.
            get_task_control()->_task_tracer.WaitForTracing(pthread_fake_meta);
        }
        get_task_control()->_task_tracer.set_status(
            TASK_STATUS_UNKNOWN, pthread_fake_meta);

        fast::butil::return_resource(get_slot(pthread_fake_meta->tid));
        pthread_fake_meta = NULL;
    });

    return pthread_fake_meta->tid;
}

void stack_trace(std::ostream& os, bthread_t tid) {
    TaskControl* c = get_task_control();
    if (NULL == c) {
        os << "TaskControl has not been created";
        return;
    }
    c->stack_trace(os, tid);
}

std::string stack_trace(bthread_t tid) {
    TaskControl* c = get_task_control();
    if (NULL == c) {
        return "TaskControl has not been created";
    }
    return c->stack_trace(tid);
}

#endif // BRPC_BTHREAD_TRACER

// Print all living (started and not finished) bthreads
void print_living_tasks(std::ostream& os, bool enable_trace) {
    TaskControl* c = get_task_control();
    if (NULL == c) {
        os << "TaskControl has not been created";
        return;
    }
    auto tids = c->get_living_bthreads();
    if (tids.empty()) {
        os << "No living bthreads\n";
        return;
    }
    for (auto tid : tids) {
        print_task(os, tid, enable_trace, true);
    }
}

static int add_workers_for_each_tag(int num) {
    int added = 0;
    auto c = get_task_control();
    for (auto i = 0; i < num; ++i) {
        added += c->add_workers(1, i % FastBthreadConfig::Get().task_group_ntags);
    }
    return added;
}




__thread TaskGroup* tls_task_group_nosignal = NULL;

BUTIL_FORCE_INLINE int
start_from_non_worker(bthread_t* __restrict tid,
                      const bthread_attr_t* __restrict attr,
                      void* (*fn)(void*),
                      void* __restrict arg) {
    TaskControl* c = get_or_new_task_control();
    if (NULL == c) {
        return ENOMEM;
    }
    auto tag = BTHREAD_TAG_DEFAULT;
    if (attr != NULL && attr->tag != BTHREAD_TAG_INVALID) {
        tag = attr->tag;
    }
    if (attr != NULL && (attr->flags & BTHREAD_NOSIGNAL)) {
        // Remember the TaskGroup to insert NOSIGNAL tasks for 2 reasons:
        // 1. NOSIGNAL is often for creating many bthreads in batch,
        //    inserting into the same TaskGroup maximizes the batch.
        // 2. bthread_flush() needs to know which TaskGroup to flush.
        auto g = tls_task_group_nosignal;
        if (NULL == g) {
            g = c->choose_one_group(tag);
            tls_task_group_nosignal = g;
        }
        return g->start_background<true>(tid, attr, fn, arg);
    }
    return c->choose_one_group(tag)->start_background<true>(tid, attr, fn, arg);
}

// Meet one of the three conditions, can run in thread local
// attr is nullptr
// tag equal to thread local
// tag equal to BTHREAD_TAG_INVALID
BUTIL_FORCE_INLINE bool can_run_thread_local(const bthread_attr_t* __restrict attr) {
    return attr == nullptr || attr->tag == fast::tls_task_group->tag() ||
           attr->tag == BTHREAD_TAG_INVALID;
}

struct TidTraits {
    static const size_t BLOCK_SIZE = 63;
    static const size_t MAX_ENTRIES = 65536;
    static const size_t INIT_GC_SIZE = 65536;
    static const bthread_t ID_INIT;
    static bool exists(bthread_t id) { return fast::TaskGroup::exists(id); }
};
const bthread_t TidTraits::ID_INIT = INVALID_BTHREAD;

typedef ListOfABAFreeId<bthread_t, TidTraits> TidList;

struct TidStopper {
    void operator()(bthread_t id) const { bthread_stop(id); }
};
struct TidJoiner {
    void operator()(bthread_t & id) const {
        bthread_join(id, NULL);
        id = INVALID_BTHREAD;
    }
};

}  // namespace fast

extern "C" {

int bthread_start_urgent(bthread_t* __restrict tid,
                         const bthread_attr_t* __restrict attr,
                         void * (*fn)(void*),
                         void* __restrict arg) {
    fast::TaskGroup* g = fast::tls_task_group;
    if (g) {
        // if attribute is null use thread local task group
        if (fast::can_run_thread_local(attr)) {
            return fast::TaskGroup::start_foreground(&g, tid, attr, fn, arg);
        }
    }
    return fast::start_from_non_worker(tid, attr, fn, arg);
}

int bthread_start_background(bthread_t* __restrict tid,
                             const bthread_attr_t* __restrict attr,
                             void * (*fn)(void*),
                             void* __restrict arg) {
    fast::TaskGroup* g = fast::tls_task_group;
    if (g) {
        // if attribute is null use thread local task group
        if (fast::can_run_thread_local(attr)) {
            return g->start_background<false>(tid, attr, fn, arg);
        }
    }
    return fast::start_from_non_worker(tid, attr, fn, arg);
}

void bthread_flush() {
    fast::TaskGroup* g = fast::tls_task_group;
    if (g) {
        return g->flush_nosignal_tasks();
    }
    g = fast::tls_task_group_nosignal;
    if (g) {
        // NOSIGNAL tasks were created in this non-worker.
        fast::tls_task_group_nosignal = NULL;
        return g->flush_nosignal_tasks_remote();
    }
}

int bthread_interrupt(bthread_t tid, bthread_tag_t tag) {
    return fast::TaskGroup::interrupt(tid, fast::get_task_control(), tag);
}

int bthread_stop(bthread_t tid) {
    fast::TaskGroup::set_stopped(tid);
    return bthread_interrupt(tid);
}

int bthread_stopped(bthread_t tid) {
    return (int)fast::TaskGroup::is_stopped(tid);
}

bthread_t bthread_self(void) {
    fast::TaskGroup* g = fast::tls_task_group;
    // note: return 0 for main tasks now, which include main thread and
    // all work threads. So that we can identify main tasks from logs
    // more easily. This is probably questionable in the future.
    if (g != NULL && !g->is_current_main_task()/*note*/) {
        return g->current_tid();
    }
    return INVALID_BTHREAD;
}

int bthread_equal(bthread_t t1, bthread_t t2) {
    return t1 == t2;
}

#ifdef BUTIL_USE_ASAN
// Fixme!!!
// The noreturn `bthread_exit' may cause a warning of ASan, but does not abort the program.
//
// ==94463==WARNING: ASan is ignoring requested __asan_handle_no_return: stack type: default top: 0x00016dd7f000; bottom 0x00010b1a4000; size: 0x000062bdb000 (1656598528)
// False positive error reports may follow
#endif // BUTIL_USE_ASAN
void bthread_exit(void* retval) {
    fast::TaskGroup* g = fast::tls_task_group;
    if (g != NULL && !g->is_current_main_task()) {
        throw fast::ExitException(retval);
    } else {
        pthread_exit(retval);
    }
}

int bthread_join(bthread_t tid, void** thread_return) {
    return fast::TaskGroup::join(tid, thread_return);
}

int bthread_attr_init(bthread_attr_t* a) {
    *a = BTHREAD_ATTR_NORMAL;
    return 0;
}

int bthread_attr_destroy(bthread_attr_t*) {
    return 0;
}

int bthread_getattr(bthread_t tid, bthread_attr_t* attr) {
    return fast::TaskGroup::get_attr(tid, attr);
}

int bthread_getconcurrency(void) {
    return fast::FastBthreadConfig::Get().bthread_concurrency;
}

int bthread_setconcurrency(int num) {
    if (num < BTHREAD_MIN_CONCURRENCY || num > BTHREAD_MAX_CONCURRENCY) {
        LOG(ERROR) << "Invalid concurrency=" << num;
        return EINVAL;
    }
    if (fast::FastBthreadConfig::Get().bthread_min_concurrency > 0) {
        if (num < fast::FastBthreadConfig::Get().bthread_min_concurrency) {
            return EINVAL;
        }
        if (fast::never_set_bthread_concurrency) {
            fast::never_set_bthread_concurrency = false;
        }
        fast::FastBthreadConfig::Get().bthread_concurrency = num;
        return 0;
    }
    fast::TaskControl* c = fast::get_task_control();
    if (c != NULL) {
        if (num < c->concurrency()) {
            return EPERM;
        } else if (num == c->concurrency()) {
            return 0;
        }
    }
    BAIDU_SCOPED_LOCK(fast::g_task_control_mutex);
    c = fast::get_task_control();
    if (c == NULL) {
        if (fast::never_set_bthread_concurrency) {
            fast::never_set_bthread_concurrency = false;
            fast::FastBthreadConfig::Get().bthread_concurrency = num;
        } else if (num > fast::FastBthreadConfig::Get().bthread_concurrency) {
            fast::FastBthreadConfig::Get().bthread_concurrency = num;
        }
        return 0;
    }
    if (fast::FastBthreadConfig::Get().bthread_concurrency != c->concurrency()) {
        LOG(ERROR) << "CHECK failed: bthread_concurrency="
                   << fast::FastBthreadConfig::Get().bthread_concurrency
                   << " != tc_concurrency=" << c->concurrency();
        fast::FastBthreadConfig::Get().bthread_concurrency = c->concurrency();
    }
    if (num > fast::FastBthreadConfig::Get().bthread_concurrency) {
        // Create more workers if needed.
        auto added = fast::add_workers_for_each_tag(num - fast::FastBthreadConfig::Get().bthread_concurrency);
        fast::FastBthreadConfig::Get().bthread_concurrency += added;
    }
    return (num == fast::FastBthreadConfig::Get().bthread_concurrency ? 0 : EPERM);
}

int bthread_getconcurrency_by_tag(bthread_tag_t tag) {
    BAIDU_SCOPED_LOCK(fast::g_task_control_mutex);
    auto c = fast::get_task_control();
    if (c == NULL) {
        return EPERM;
    }
    return c->concurrency(tag);
}

int bthread_setconcurrency_by_tag(int num, bthread_tag_t tag) {
    if (tag == BTHREAD_TAG_INVALID) {
        return 0;
    } else if (tag < BTHREAD_TAG_DEFAULT || tag >= fast::FastBthreadConfig::Get().task_group_ntags) {
        return EINVAL;
    }
    if (num < BTHREAD_MIN_CONCURRENCY || num > BTHREAD_MAX_CONCURRENCY) {
        LOG(ERROR) << "Invalid concurrency_by_tag=" << num;
        return EINVAL;
    }
    auto c = fast::get_or_new_task_control();
    if (c == NULL) {
        return ENOMEM;
    }
    BAIDU_SCOPED_LOCK(fast::g_task_control_mutex);
    auto tag_ngroup = c->concurrency(tag);
    auto add = num - tag_ngroup;

    if (add >= 0) {
        auto added = c->add_workers(add, tag);
        fast::FastBthreadConfig::Get().bthread_concurrency += added;
        return (add == added ? 0 : EPERM);
    } else {
        LOG(ERROR) << "Fail to set concurrency by tag: " << tag
                     << ", tag concurrency should be larger than old oncurrency. old concurrency: "
                     << tag_ngroup << ", new concurrency: " << num;
        return EPERM;
    }
}

int bthread_about_to_quit() {
    fast::TaskGroup* g = fast::tls_task_group;
    if (g != NULL) {
        fast::TaskMeta* current_task = g->current_task();
        if(!(current_task->attr.flags & BTHREAD_NEVER_QUIT)) {
            current_task->about_to_quit = true;
        }
        return 0;
    }
    return EPERM;
}

int bthread_timer_add(bthread_timer_t* id, timespec abstime,
                      void (*on_timer)(void*), void* arg) {
    fast::TaskControl* c = fast::get_or_new_task_control();
    if (c == NULL) {
        return ENOMEM;
    }
    fast::TimerThread* tt = fast::get_or_create_global_timer_thread();
    if (tt == NULL) {
        return ENOMEM;
    }
    bthread_timer_t tmp = tt->schedule(on_timer, arg, abstime);
    if (tmp != 0) {
        *id = tmp;
        return 0;
    }
    return ESTOP;
}

int bthread_timer_del(bthread_timer_t id) {
    fast::TaskControl* c = fast::get_task_control();
    if (c != NULL) {
        fast::TimerThread* tt = fast::get_global_timer_thread();
        if (tt == NULL) {
            return EINVAL;
        }
        const int state = tt->unschedule(id);
        if (state >= 0) {
            return state;
        }
    }
    return EINVAL;
}

int bthread_usleep(uint64_t microseconds) {
    fast::TaskGroup* g = fast::BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    if (NULL != g && !g->is_current_pthread_task()) {
        return fast::TaskGroup::usleep(&g, microseconds);
    }
    return ::usleep(microseconds);
}

int bthread_yield(void) {
    fast::TaskGroup* g = fast::BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    if (NULL != g && !g->is_current_pthread_task()) {
        fast::TaskGroup::yield(&g);
        return 0;
    }
    // pthread_yield is not available on MAC
    return sched_yield();
}

int bthread_set_worker_startfn(void (*start_fn)()) {
    if (start_fn == NULL) {
        return EINVAL;
    }
    fast::g_worker_startfn = start_fn;
    return 0;
}

int bthread_set_tagged_worker_startfn(void (*start_fn)(bthread_tag_t)) {
    if (start_fn == NULL) {
        return EINVAL;
    }
    fast::g_tagged_worker_startfn = start_fn;
    return 0;
}

int bthread_set_create_span_func(void* (*func)()) {
    if (func == NULL) {
        return EINVAL;
    }
    fast::g_create_span_func = func;
    return 0;
}

void bthread_stop_world() {
    fast::TaskControl* c = fast::get_task_control();
    if (c != NULL) {
        c->stop_and_join();
    }
}

int bthread_list_init(bthread_list_t* list,
                      unsigned /*size*/,
                      unsigned /*conflict_size*/) {
    list->impl = new (std::nothrow) fast::TidList;
    if (NULL == list->impl) {
        return ENOMEM;
    }
    // Set unused fields to zero as well.
    list->head = 0;
    list->size = 0;
    list->conflict_head = 0;
    list->conflict_size = 0;
    return 0;
}

void bthread_list_destroy(bthread_list_t* list) {
    delete static_cast<fast::TidList*>(list->impl);
    list->impl = NULL;
}

int bthread_list_add(bthread_list_t* list, bthread_t id) {
    if (list->impl == NULL) {
        return EINVAL;
    }
    return static_cast<fast::TidList*>(list->impl)->add(id);
}

int bthread_list_stop(bthread_list_t* list) {
    if (list->impl == NULL) {
        return EINVAL;
    }
    static_cast<fast::TidList*>(list->impl)->apply(fast::TidStopper());
    return 0;
}

int bthread_list_join(bthread_list_t* list) {
    if (list->impl == NULL) {
        return EINVAL;
    }
    static_cast<fast::TidList*>(list->impl)->apply(fast::TidJoiner());
    return 0;
}

bthread_tag_t bthread_self_tag(void) {
    return fast::tls_task_group != nullptr ? fast::tls_task_group->tag()
                                              : BTHREAD_TAG_DEFAULT;
}

uint64_t bthread_cpu_clock_ns(void) {
     fast::TaskGroup* g = fast::tls_task_group;
    if (g != NULL && !g->is_current_main_task()) {
        return g->current_task_cpu_clock_ns();
    }
    return 0;
}

}  // extern "C"

void bthread_attr_set_name(bthread_attr_t* attr, const char* name) {
    if (attr) {
        strncpy(attr->name, name, BTHREAD_NAME_MAX_LENGTH);
        attr->name[BTHREAD_NAME_MAX_LENGTH] = '\0';
    }
}
