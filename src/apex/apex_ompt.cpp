//  Copyright (c) 2014-2018 University of Oregon
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <ompt.h>
#include <unordered_map>
#include "string.h"
#include "stdio.h"
#include "apex_api.hpp"
#include "apex_types.h"
#include "thread_instance.hpp"
#include "apex_cxx_shared_lock.hpp"
#include <atomic>
#include <memory>
#include <string>
#include "apex_assert.h"

#ifdef DEBUG
#define DEBUG_PRINT(...) do{ \
fprintf( stderr, __VA_ARGS__ ); fflush(stderr); \
} while( false )
#else
#define DEBUG_PRINT(...) do{ } while ( false )
#endif

std::mutex apex_apex_threadid_mutex;
std::atomic<uint64_t> apex_numthreads(0);
APEX_NATIVE_TLS uint64_t apex_threadid(-1);

class linked_timer {
    public:
        void * prev;
        std::shared_ptr<apex::task_wrapper> tw;
        bool timing;
        inline void start(void) { apex::start(tw); timing = true;  }
        inline void yield(void) { apex::yield(tw); timing = false; }
        inline void stop(void)  { apex::stop(tw);  timing = false; }
        /* constructor */
        linked_timer(const char * name,
            uint64_t task_id,
            void *p,
            std::shared_ptr<apex::task_wrapper> &parent,
            bool auto_start) :
            prev(p), timing(auto_start) {
                // No GUIDs generated by the runtime? Generate our own.
                if (task_id == 0ULL) {
                    tw = apex::new_task(name, UINT64_MAX, parent);
                } else {
                    tw = apex::new_task(name, task_id, parent);
                }
                if (auto_start) { this->start();
            }
        }
        /* destructor */
        ~linked_timer() {
            if (timing) {
                apex::stop(tw);
            }
        }
};

/* Function pointers.  These are all queried from the runtime during
 *  * ompt_initialize() */
static ompt_enumerate_states_t ompt_enumerate_states;
static ompt_enumerate_mutex_impls_t ompt_enumerate_mutex_impls;
static ompt_set_callback_t ompt_set_callback;
static ompt_get_callback_t ompt_get_callback;
static ompt_get_thread_data_t ompt_get_thread_data;
static ompt_get_num_procs_t ompt_get_num_procs;
static ompt_get_num_places_t ompt_get_num_places;
static ompt_get_place_proc_ids_t ompt_get_place_proc_ids;
static ompt_get_place_num_t ompt_get_place_num;
static ompt_get_partition_place_nums_t ompt_get_partition_place_nums;
static ompt_get_proc_id_t ompt_get_proc_id;
static ompt_get_parallel_info_t ompt_get_parallel_info;
static ompt_get_task_info_t ompt_get_task_info;
static ompt_get_task_memory_t ompt_get_task_memory;
static ompt_get_target_info_t ompt_get_target_info;
static ompt_get_num_devices_t ompt_get_num_devices;
static ompt_get_unique_id_t ompt_get_unique_id;
static ompt_finalize_tool_t ompt_finalize_tool;
static ompt_function_lookup_t ompt_function_lookup;

namespace apex {

/* This function is used by APEX to tell the OpenMP runtime to stop sending
 * OMPT events.  This is when apex::finalize() happens before ompt_finalize().
 * It is called from apex::finalize().
 */
void ompt_force_shutdown(void) {
    DEBUG_PRINT("Forcing shutdown of OpenMP Tools API\n");
    /* OpenMP might not have been used... */
    if (ompt_finalize_tool) {
        ompt_finalize_tool();
    }
}

} // end apex namespace

/* These methods are some helper functions for starting/stopping timers */

void apex_ompt_start(const char * state, ompt_data_t * ompt_data,
        ompt_data_t * region_data, bool auto_start) {
    static std::shared_ptr<apex::task_wrapper> nothing(nullptr);
    linked_timer* tmp;
    /* First check if there's a parent "region" - could be a task */
    if (ompt_data->ptr == nullptr && region_data != nullptr) {
        /* Get the parent scoped timer */
        linked_timer* parent = (linked_timer*)(region_data->ptr);
        if (parent != nullptr) {
            tmp = new linked_timer(state, ompt_data->value, ompt_data->ptr,
            parent->tw, auto_start);
        } else {
            tmp = new linked_timer(state, ompt_data->value, ompt_data->ptr,
            nothing, auto_start);
        }
#if 0
    /* if the ompt_data->ptr pointer is not null, that means we have an implicit
     * parent and there's no need to specify a parent */
    } else if (ompt_data->ptr != nullptr) {
        /* Get the parent scoped timer */
        linked_timer* previous = (linked_timer*)(ompt_data->ptr);
        tmp = new linked_timer(state, ompt_data->value, ompt_data->ptr,
        parent->tw, true);
#endif
    } else {
        tmp = new linked_timer(state, ompt_data->value, ompt_data->ptr,
        nothing, auto_start);
    }

    /* Save the address of the scoped timer with the parallel region
     * or task, so we can stop the timer later */
    ompt_data->ptr = (void*)(tmp);
}

void apex_ompt_stop(ompt_data_t * ompt_data) {
    APEX_ASSERT(ompt_data->ptr);
    void* tmp = ((linked_timer*)(ompt_data->ptr))->prev;
    delete((linked_timer*)(ompt_data->ptr));
    ompt_data->ptr = tmp;
}

/*
 * Mandatory Events
 *
 * The following events are supported by all OMPT implementations.
 */

/* Event #1, thread begin */
extern "C" void apex_thread_begin(
    ompt_thread_t thread_type,   /* type of thread */
    ompt_data_t *thread_data     /* data of thread */)
{
    APEX_UNUSED(thread_data);
    {
        std::unique_lock<std::mutex> l(apex_apex_threadid_mutex);
        apex_threadid = apex_numthreads++;
    }
    switch (thread_type) {
        case ompt_thread_initial:
            apex::register_thread("OpenMP Initial Thread");
            apex::sample_value("OpenMP Initial Thread", 1);
            break;
        case ompt_thread_worker:
            apex::register_thread("OpenMP Worker Thread");
            apex::sample_value("OpenMP Worker Thread", 1);
            break;
        case ompt_thread_other:
            apex::register_thread("OpenMP Other Thread");
            apex::sample_value("OpenMP Other Thread", 1);
            break;
        case ompt_thread_unknown:
        default:
            apex::register_thread("OpenMP Unknown Thread");
            apex::sample_value("OpenMP Unknown Thread", 1);
    }
    DEBUG_PRINT("New %d thread\n", thread_type);
}

/* Event #2, thread end */
extern "C" void apex_thread_end(
    ompt_data_t *thread_data              /* data of thread                      */
) {
    APEX_UNUSED(thread_data);
    apex::exit_thread();
}

/* Event #3, parallel region begin */
static void apex_parallel_region_begin (
    ompt_data_t *encountering_task_data,         /* data of encountering task           */
    const ompt_frame_t *encountering_task_frame,  /* frame data of encountering task     */
    ompt_data_t *parallel_data,                  /* data of parallel region             */
    unsigned int requested_team_size,            /* requested number of threads in team */
    int flags,                                   /* flags */
    const void *codeptr_ra                       /* return address of runtime call      */
) {
    APEX_UNUSED(encountering_task_data);
    APEX_UNUSED(encountering_task_frame);
    APEX_UNUSED(requested_team_size);
    APEX_UNUSED(flags);
    char regionIDstr[128] = {0};
    sprintf(regionIDstr, "OpenMP Parallel Region: UNRESOLVED ADDR %p", codeptr_ra);
    apex_ompt_start(regionIDstr, parallel_data, encountering_task_data, true);
    DEBUG_PRINT("%lu: Parallel Region Begin parent: %p, apex_parent: %p, region: %p, apex_region: %p, %s\n", apex_threadid, (void*)encountering_task_data, encountering_task_data->ptr, (void*)parallel_data, parallel_data->ptr, regionIDstr);
}

/* Event #4, parallel region end */
static void apex_parallel_region_end (
    ompt_data_t *parallel_data,           /* data of parallel region             */
    ompt_data_t *encountering_task_data,  /* data of encountering task           */
    int flags,                            /* flags              */
    const void *codeptr_ra                /* return address of runtime call      */
) {
    APEX_UNUSED(encountering_task_data);
    APEX_UNUSED(flags);
    APEX_UNUSED(codeptr_ra);
    DEBUG_PRINT("%lu: Parallel Region End parent: %p, apex_parent: %p, region: %p, apex_region: %p\n", apex_threadid, (void*)encountering_task_data, encountering_task_data->ptr, (void*)parallel_data, parallel_data->ptr);
    apex_ompt_stop(parallel_data);
}

/* Event #5, task create */
extern "C" void apex_task_create (
    ompt_data_t *encountering_task_data,        /* data of parent task            */
    const ompt_frame_t *encountering_task_frame, /* frame data for parent task     */
    ompt_data_t *new_task_data,                 /* data of created task           */
    int type,                                   /* flags */
    int has_dependences,                        /* created task has dependences   */
    const void *codeptr_ra                      /* return address of runtime call */
) {
    APEX_UNUSED(encountering_task_frame);
    APEX_UNUSED(has_dependences);
    APEX_UNUSED(codeptr_ra);
    char * type_str;
    static const char * initial_str = "OpenMP Initial Task";
    static const char * implicit_str = "OpenMP Implicit Task";
    static const char * explicit_str = "OpenMP Explicit Task";
    static const char * target_str = "OpenMP Target Task";
    static const char * undeferred_str = "OpenMP Undeferred Task";
    static const char * untied_str = "OpenMP Untied Task";
    static const char * final_str = "OpenMP Final Task";
    static const char * mergable_str = "OpenMP Mergable Task";
    static const char * merged_str = "OpenMP Merged Task";
    switch ((ompt_task_flag_t)(type)) {
        case ompt_task_initial:
            type_str = const_cast<char*>(initial_str);
            break;
        case ompt_task_implicit:
            type_str = const_cast<char*>(implicit_str);
            break;
        case ompt_task_explicit:
            type_str = const_cast<char*>(explicit_str);
            break;
        case ompt_task_target:
            type_str = const_cast<char*>(target_str);
            break;
        case ompt_task_undeferred:
            type_str = const_cast<char*>(undeferred_str);
            break;
        case ompt_task_untied:
            type_str = const_cast<char*>(untied_str);
            break;
        case ompt_task_final:
            type_str = const_cast<char*>(final_str);
            break;
        case ompt_task_mergeable:
            type_str = const_cast<char*>(mergable_str);
            break;
        case ompt_task_merged:
        default:
            type_str = const_cast<char*>(merged_str);
    }
    DEBUG_PRINT("%lu: %s Task Create parent: %p, child: %p\n", apex_threadid, type_str, (void*)encountering_task_data, (void*)new_task_data);

    if (codeptr_ra != nullptr) {
        char regionIDstr[128] = {0};
        sprintf(regionIDstr, "%s: UNRESOLVED ADDR %p", type_str, codeptr_ra);
        apex_ompt_start(regionIDstr, new_task_data, encountering_task_data,
        false);
    } else {
        apex_ompt_start(type_str, new_task_data, encountering_task_data,
        false);
    }
}

/* Event #6, task schedule */
extern "C" void apex_task_schedule(
    ompt_data_t *prior_task_data,         /* data of prior task   */
    ompt_task_status_t prior_task_status, /* status of prior task */
    ompt_data_t *next_task_data           /* data of next task    */
    ) {
    DEBUG_PRINT("%lu: Task Schedule prior: %p, status: %d, next: %p\n", apex_threadid, (void*)prior_task_data, prior_task_status, (void*)next_task_data);
    if (prior_task_data != nullptr) {
        linked_timer* prior = (linked_timer*)(prior_task_data->ptr);
        if (prior != nullptr) {
            switch (prior_task_status) {
                case ompt_task_yield:
                case ompt_task_detach:
                case ompt_task_switch:
                    prior->yield();
                    break;
                case ompt_task_complete:
                case ompt_task_early_fulfill:
                case ompt_task_late_fulfill:
                case ompt_task_cancel:
                default:
                    void* tmp = prior->prev;
                    delete(prior);
                    prior_task_data->ptr = tmp;
            }
        }
    }
    //apex_ompt_start("OpenMP Task", next_task_data, nullptr, true);
    linked_timer* next = (linked_timer*)(next_task_data->ptr);
    //APEX_ASSERT(next);
    if (next != nullptr) {
        next->start();
    }
}

/* Event #7, implicit task */
extern "C" void apex_implicit_task(
    ompt_scope_endpoint_t endpoint, /* endpoint of implicit task       */
    ompt_data_t *parallel_data,     /* data of parallel region         */
    ompt_data_t *task_data,         /* data of implicit task           */
    unsigned int team_size,         /* team size                       */
    unsigned int thread_num,        /* thread number of calling thread */
    int flags
  ) {
    APEX_UNUSED(team_size);
    APEX_UNUSED(thread_num);
    APEX_UNUSED(flags);
    if (endpoint == ompt_scope_begin) {
        if (flags == ompt_task_initial) {
            apex_ompt_start("OpenMP Initial Task", task_data, parallel_data, true);
        } else {
            apex_ompt_start("OpenMP Implicit Task", task_data, parallel_data, true);
        }
    } else {
        apex_ompt_stop(task_data);
    }
    DEBUG_PRINT("%lu: Initial/Implicit Task task [%u:%u]: %p, apex: %p, region: %p, region ptr: %p, %d\n",
        apex_threadid, thread_num, team_size, (void*)task_data, task_data->ptr,
        (void*)parallel_data, parallel_data ? parallel_data->ptr : nullptr,
        endpoint);
}

/* These are placeholder functions */

#if 0

/* Event #8, target */
extern "C" void apex_target (
    ompt_target_t kind,
    ompt_scope_endpoint_t endpoint,
    uint64_t device_num,
    ompt_data_t *task_data,
    ompt_id_t target_id,
    const void *codeptr_ra
) {
}

/* Event #9, target data */
extern "C" void apex_target_data_op (
    ompt_id_t target_id,
    ompt_id_t host_op_id,
    ompt_target_data_op_t optype,
    void *host_addr,
    void *device_addr,
    size_t bytes
) {
}

/* Event #10, target submit */
extern "C" void apex_target_submit (
    ompt_id_t target_id,
    ompt_id_t host_op_id
) {
}

/* Event #11, tool control */
extern "C" void apex_control(
    uint64_t command,      /* command of control call             */
    uint64_t modifier,     /* modifier of control call            */
    void *arg,             /* argument of control call            */
    const void *codeptr_ra /* return address of runtime call      */
    ) {
}

/* Event #12, device initialize */
extern "C" void apex_device_initialize (
    uint64_t device_num,
    const char *type,
    ompt_device_t *device,
    ompt_function_lookup_t lookup,
    const char *documentation
) {
}

/* Event #13, device finalize */
extern "C" void apex_device_finalize (
    uint64_t device_num
) {
}

/* Event #14, device load */
extern "C" void apex_device_load_t (
    uint64_t device_num,
    const char * filename,
    int64_t offset_in_file,
    void * vma_in_file,
    size_t bytes,
    void * host_addr,
    void * device_addr,
    uint64_t module_id
) {
}

/* Event #15, device load */
extern "C" void apex_device_unload (
    uint64_t device_num,
    uint64_t module_id
) {
}

#endif // placeholder functions

/**********************************************************************/
/* End Mandatory Events */
/**********************************************************************/

/**********************************************************************/
/* Optional events */
/**********************************************************************/

/* Event #16, sync region wait       */
extern "C" void apex_sync_region_wait (
    ompt_sync_region_t kind,        /* kind of sync region            */
    ompt_scope_endpoint_t endpoint, /* endpoint of sync region        */
    ompt_data_t *parallel_data,     /* data of parallel region        */
    ompt_data_t *task_data,         /* data of task                   */
    const void *codeptr_ra          /* return address of runtime call */
) {
    char * tmp_str;
    static const char * barrier_str = "Barrier Wait";
    static const char * barrier_i_str = "Implicit Barrier Wait";
    static const char * barrier_e_str = "Explicit Barrier Wait";
    static const char * barrier_imp_str = "Barrier Implementation Wait";
    static const char * task_wait_str = "Task Wait";
    static const char * task_group_str = "Task Group Wait";
    static const char * reduction_str = "Reduction Wait";
    static const char * unknown_str = "Unknown Wait";
    switch (kind) {
        case ompt_sync_region_barrier:
            tmp_str = const_cast<char*>(barrier_str);
            break;
        case ompt_sync_region_barrier_implicit:
            tmp_str = const_cast<char*>(barrier_i_str);
            break;
        case ompt_sync_region_barrier_explicit:
            tmp_str = const_cast<char*>(barrier_e_str);
            break;
        case ompt_sync_region_barrier_implementation:
            tmp_str = const_cast<char*>(barrier_imp_str);
            break;
        case ompt_sync_region_taskwait:
            tmp_str = const_cast<char*>(task_wait_str);
            break;
        case ompt_sync_region_taskgroup:
            tmp_str = const_cast<char*>(task_group_str);
            break;
        case ompt_sync_region_reduction:
            tmp_str = const_cast<char*>(reduction_str);
            break;
        default:
            tmp_str = const_cast<char*>(unknown_str);
            break;
    }
    if (endpoint == ompt_scope_begin) {
        char regionIDstr[128] = {0};
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP %s: UNRESOLVED ADDR %p", tmp_str,
            codeptr_ra);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        } else {
            sprintf(regionIDstr, "OpenMP %s", tmp_str);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        }
    } else {
        apex_ompt_stop(task_data);
    }
    DEBUG_PRINT("%lu: %s: %p, apex: %p, region: %p, region ptr: %p, %d\n",
        apex_threadid, tmp_str, (void*)task_data, task_data->ptr,
        (void*)parallel_data, parallel_data ? parallel_data->ptr : nullptr,
        endpoint);
}

/* Event #20, task at work begin or end       */
extern "C" void apex_ompt_work (
    ompt_work_t wstype,             /* type of work region            */
    ompt_scope_endpoint_t endpoint, /* endpoint of work region        */
    ompt_data_t *parallel_data,     /* data of parallel region        */
    ompt_data_t *task_data,         /* data of task                   */
    uint64_t count,                 /* quantity of work               */
    const void *codeptr_ra          /* return address of runtime call */
    ) {
    APEX_UNUSED(count); // unused on end

    char * tmp_str;
    static const char * loop_str = "Loop";
    static const char * sections_str = "Sections";
    static const char * single_executor_str = "Single Executor";
    static const char * single_other_str = "Single Other";
    static const char * workshare_str = "Workshare";
    static const char * distribute_str = "Distribute";
    static const char * taskloop_str = "Taskloop";
    static const char * unknown_str = "Unknown";

    static const char * iterations_type = "Iterations";
    static const char * collapsed_type = "Iterations (collapsed)";
    static const char * sections_type = "Sections";
    static const char * units_type = "Units of Work";
    static const char * single_type = "Single";
    char * count_type = const_cast<char*>(iterations_type);

    switch(wstype) {
        case ompt_work_loop:
            tmp_str = const_cast<char*>(loop_str);
            break;
        case ompt_work_sections:
            tmp_str = const_cast<char*>(sections_str);
            count_type = const_cast<char*>(sections_type);
            break;
        case ompt_work_single_executor:
            tmp_str = const_cast<char*>(single_executor_str);
            count_type = const_cast<char*>(single_type);
            break;
        case ompt_work_single_other:
            tmp_str = const_cast<char*>(single_other_str);
            count_type = const_cast<char*>(single_type);
            break;
        case ompt_work_workshare:
            tmp_str = const_cast<char*>(workshare_str);
            count_type = const_cast<char*>(units_type);
            break;
        case ompt_work_distribute:
            tmp_str = const_cast<char*>(distribute_str);
            break;
        case ompt_work_taskloop:
            tmp_str = const_cast<char*>(taskloop_str);
            count_type = const_cast<char*>(collapsed_type);
            break;
        default:
            tmp_str = const_cast<char*>(unknown_str);
            break;
    }
    if (endpoint == ompt_scope_begin) {
        char regionIDstr[128] = {0};
        DEBUG_PRINT("%lu: %s Begin task: %p, region: %p\n", apex_threadid,
        tmp_str, (void*)task_data, (void*)parallel_data);
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Work %s: UNRESOLVED ADDR %p", tmp_str,
            codeptr_ra);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        } else {
            sprintf(regionIDstr, "OpenMP Work %s", tmp_str);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        }
        std::stringstream ss;
        ss << count_type << ": " << regionIDstr;
        std::string tmp{ss.str()};
        apex::sample_value(tmp, count);
    } else {
        DEBUG_PRINT("%lu: %s End task: %p, region: %p\n", apex_threadid, tmp_str,
        (void*)task_data, (void*)parallel_data);
        apex_ompt_stop(task_data);
    }
}

/* Event #21, task at master begin or end       */
extern "C" void apex_ompt_master (
    ompt_scope_endpoint_t endpoint, /* endpoint of master region           */
    ompt_data_t *parallel_data,     /* data of parallel region             */
    ompt_data_t *task_data,         /* data of task                        */
    const void *codeptr_ra          /* return address of runtime call      */
) {
    if (endpoint == ompt_scope_begin) {
        if (codeptr_ra != nullptr) {
            char regionIDstr[128] = {0};
            sprintf(regionIDstr, "OpenMP Master: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        } else {
            apex_ompt_start("OpenMP Master", task_data, parallel_data, true);
        }
    } else {
        apex_ompt_stop(task_data);
    }
}

/* Event #23, sync region begin or end */
extern "C" void apex_ompt_sync_region (
    ompt_sync_region_t kind,        /* kind of sync region                 */
    ompt_scope_endpoint_t endpoint, /* endpoint of sync region             */
    ompt_data_t *parallel_data,     /* data of parallel region             */
    ompt_data_t *task_data,         /* data of task                        */
    const void *codeptr_ra          /* return address of runtime call      */
) {
    char * tmp_str;
    static const char * barrier_str = "Barrier";
    static const char * barrier_i_str = "Implicit Barrier";
    static const char * barrier_e_str = "Explicit Barrier";
    static const char * barrier_imp_str = "Barrier Implementation";
    static const char * task_str = "Task";
    static const char * task_group_str = "Task Group";
    static const char * reduction_str = "Reduction";
    static const char * unknown_str = "Unknown";
    switch (kind) {
        case ompt_sync_region_barrier:
            tmp_str = const_cast<char*>(barrier_str);
            break;
        case ompt_sync_region_barrier_implicit:
            tmp_str = const_cast<char*>(barrier_i_str);
            break;
        case ompt_sync_region_barrier_explicit:
            tmp_str = const_cast<char*>(barrier_e_str);
            break;
        case ompt_sync_region_barrier_implementation:
            tmp_str = const_cast<char*>(barrier_imp_str);
            break;
        case ompt_sync_region_taskwait:
            tmp_str = const_cast<char*>(task_str);
            break;
        case ompt_sync_region_taskgroup:
            tmp_str = const_cast<char*>(task_group_str);
            break;
        case ompt_sync_region_reduction:
            tmp_str = const_cast<char*>(reduction_str);
            break;
        default:
            tmp_str = const_cast<char*>(unknown_str);
            break;
    }
    if (endpoint == ompt_scope_begin) {
        char regionIDstr[128] = {0};
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP %s: UNRESOLVED ADDR %p", tmp_str, codeptr_ra);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        } else {
            sprintf(regionIDstr, "OpenMP %s", tmp_str);
            apex_ompt_start(regionIDstr, task_data, parallel_data, true);
        }
    } else {
        apex_ompt_stop(task_data);
    }
}

/* Event #29, flush event */
extern "C" void apex_ompt_flush (
    ompt_data_t *thread_data, /* data of thread                      */
    const void *codeptr_ra    /* return address of runtime call      */
) {
    APEX_UNUSED(thread_data);
    if (codeptr_ra != nullptr) {
        char regionIDstr[128] = {0};
        sprintf(regionIDstr, "OpenMP Flush: UNRESOLVED ADDR %p", codeptr_ra);
        apex::sample_value(regionIDstr, 1);
    } else {
        apex::sample_value(std::string("OpenMP Flush"),1);
    }
}

/* Event #30, cancel event */
extern "C" void apex_ompt_cancel (
    ompt_data_t *task_data,   /* data of task                        */
    int flags,                /* cancel flags                        */
    const void *codeptr_ra    /* return address of runtime call      */
) {
    char regionIDstr[128] = {0};
    if (flags & ompt_cancel_parallel) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Parallel: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Parallel"),1);
        }
    }
    if (flags & ompt_cancel_sections) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Sections: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Sections"),1);
        }
    }
    if (flags & ompt_cancel_loop) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Do: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Do"),1);
        }
    }
    if (flags & ompt_cancel_taskgroup) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Taskgroup: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Taskgroup"),1);
        }
    }
    if (flags & ompt_cancel_activated) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Activated: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Activated"),1);
        }
    }
    if (flags & ompt_cancel_detected) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr, "OpenMP Cancel Detected: UNRESOLVED ADDR %p",
            codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Detected"),1);
        }
    }
    if (flags & ompt_cancel_discarded_task) {
        if (codeptr_ra != nullptr) {
            sprintf(regionIDstr,
            "OpenMP Cancel Discarded Task: UNRESOLVED ADDR %p", codeptr_ra);
            apex::sample_value(std::string(regionIDstr),1);
        } else {
            apex::sample_value(std::string("OpenMP Cancel Discarded Task"),1);
        }
    }
    apex_ompt_stop(task_data);
}

/* Event #30, cancel event */
extern "C" void apex_ompt_idle (
    ompt_scope_endpoint_t endpoint /* endpoint of idle time               */
) {
    static APEX_NATIVE_TLS apex::profiler* p = nullptr;
    if (endpoint == ompt_scope_begin) {
        p = apex::start("OpenMP Idle");
    } else {
        apex::stop(p);
    }
}

/**********************************************************************/
/* End Optional events */
/**********************************************************************/

// This function is for checking that the function registration worked.
int apex_ompt_register(ompt_callbacks_t e, ompt_callback_t c ,
    const char * name) {
  DEBUG_PRINT("Registering OMPT callback %s...",name); fflush(stderr);
  if (ompt_set_callback(e, c) == 0) { \
    fprintf(stderr,"\n\tFailed to register OMPT callback %s!\n",name);
    fflush(stderr);
  } else {
    DEBUG_PRINT("success.\n");
  }
  return 0;
}

extern "C" {

int ompt_initialize(ompt_function_lookup_t lookup, int initial_device_num,
    ompt_data_t* tool_data) {
    APEX_UNUSED(initial_device_num);
    APEX_UNUSED(tool_data);
    {
        std::unique_lock<std::mutex> l(apex_apex_threadid_mutex);
        apex_threadid = apex_numthreads++;
    }
    DEBUG_PRINT("Getting OMPT functions..."); fflush(stderr);
    ompt_function_lookup = lookup;
    ompt_finalize_tool = (ompt_finalize_tool_t)
        lookup("ompt_finalize_tool");
    ompt_set_callback = (ompt_set_callback_t)
        lookup("ompt_set_callback");
    ompt_get_callback = (ompt_get_callback_t)
        lookup("ompt_get_callback");
    ompt_get_task_info = (ompt_get_task_info_t)
        lookup("ompt_get_task_info");
    ompt_get_task_memory = (ompt_get_task_memory_t)
        lookup("ompt_get_task_memory");
    ompt_get_thread_data = (ompt_get_thread_data_t)
        lookup("ompt_get_thread_data");
    ompt_get_parallel_info = (ompt_get_parallel_info_t)
        lookup("ompt_get_parallel_info");
    ompt_get_unique_id = (ompt_get_unique_id_t)
        lookup("ompt_get_unique_id");
    ompt_get_num_places = (ompt_get_num_places_t)
        lookup("ompt_get_num_places");
    ompt_get_num_devices = (ompt_get_num_devices_t)
        lookup("ompt_get_num_devices");
    ompt_get_num_procs = (ompt_get_num_procs_t)
        lookup("ompt_get_num_procs");
    ompt_get_place_proc_ids = (ompt_get_place_proc_ids_t)
        lookup("ompt_get_place_proc_ids");
    ompt_get_place_num = (ompt_get_place_num_t)
        lookup("ompt_get_place_num");
    ompt_get_partition_place_nums = (ompt_get_partition_place_nums_t)
        lookup("ompt_get_partition_place_nums");
    ompt_get_proc_id = (ompt_get_proc_id_t)
        lookup("ompt_get_proc_id");
    ompt_get_target_info = (ompt_get_target_info_t)
        lookup("ompt_get_target_info");
    ompt_enumerate_states = (ompt_enumerate_states_t)
        lookup("ompt_enumerate_states");
    ompt_enumerate_mutex_impls = (ompt_enumerate_mutex_impls_t)
        lookup("ompt_enumerate_mutex_impls");

    DEBUG_PRINT("success.\n");

    apex::init("OpenMP Program",0,1);
    DEBUG_PRINT("Registering OMPT events...\n"); fflush(stderr);

    /* Mandatory events */

    // Event 1: thread begin
    apex_ompt_register(ompt_callback_thread_begin,
        (ompt_callback_t)&apex_thread_begin, "thread_begin");
    // Event 2: thread end
    apex_ompt_register(ompt_callback_thread_end,
        (ompt_callback_t)&apex_thread_end, "thread_end");
    // Event 3: parallel begin
    apex_ompt_register(ompt_callback_parallel_begin,
        (ompt_callback_t)&apex_parallel_region_begin, "parallel_begin");
    // Event 4: parallel end
    apex_ompt_register(ompt_callback_parallel_end,
        (ompt_callback_t)&apex_parallel_region_end, "parallel_end");
    if (apex::apex_options::ompt_high_overhead_events()) {
        // Event 5: task create
        apex_ompt_register(ompt_callback_task_create,
            (ompt_callback_t)&apex_task_create, "task_create");
        // Event 6: task schedule (start/stop)
        apex_ompt_register(ompt_callback_task_schedule,
            (ompt_callback_t)&apex_task_schedule, "task_schedule");
        // Event 7: implicit task (start/stop)
        apex_ompt_register(ompt_callback_implicit_task,
            (ompt_callback_t)&apex_implicit_task, "implicit_task");
    }

 #if 0
    // Event 8: target
    apex_ompt_register(ompt_callback_target,
        (ompt_callback_t)&apex_target, "target");
    // Event 9: target data operation
    apex_ompt_register(ompt_callback_target_data_op,
        (ompt_callback_t)&apex_target_data_op, "target_data_operation");
    // Event 10: target submit
    apex_ompt_register(ompt_callback_target_submit,
        (ompt_callback_t)&apex_target_submit, "target_submit");
    // Event 11: control tool
    apex_ompt_register(ompt_callback_control_tool,
        (ompt_callback_t)&apex_control, "event_control");
    // Event 12: device initialize
    apex_ompt_register(ompt_callback_device_initialize,
        (ompt_callback_t)&apex_device_initialize, "device_initialize");
    // Event 13: device finalize
    apex_ompt_register(ompt_callback_device_finalize,
        (ompt_callback_t)&apex_device_finalize, "device_finalize");
    // Event 14: device load
    apex_ompt_register(ompt_callback_device_load,
        (ompt_callback_t)&apex_device_load, "device_load");
    // Event 15: device unload
    apex_ompt_register(ompt_callback_device_unload,
        (ompt_callback_t)&apex_device_unload, "device_unload");
#endif

    /* optional events */

    if (!apex::apex_options::ompt_required_events_only()) {
        // Event 20: task at work begin or end
        apex_ompt_register(ompt_callback_work,
            (ompt_callback_t)&apex_ompt_work, "work");
        /* Event 21: task at master begin or end     */
        apex_ompt_register(ompt_callback_master,
            (ompt_callback_t)&apex_ompt_master, "master");
#if 0
        /* Event 22: target map                      */
#endif
        /* Event 29: after executing flush           */
        apex_ompt_register(ompt_callback_flush,
            (ompt_callback_t)&apex_ompt_flush, "flush");
        /* Event 30: cancel innermost binding region */
        apex_ompt_register(ompt_callback_cancel,
            (ompt_callback_t)&apex_ompt_cancel, "cancel");

        if (apex::apex_options::ompt_high_overhead_events()) {
            // Event 16: sync region wait begin or end
            apex_ompt_register(ompt_callback_sync_region_wait,
                (ompt_callback_t)&apex_sync_region_wait, "sync_region_wait");
#if 0
            // Event 17: mutex released
            apex_ompt_register(ompt_callback_mutex_released,
                (ompt_callback_t)&apex_mutex_released, "mutex_released");
            // Event 18: report task dependences
            apex_ompt_register(ompt_callback_report_task_dependences,
                (ompt_callback_t)&apex_report_task_dependences,
                "mutex_report_task_dependences");
            // Event 19: report task dependence
            apex_ompt_register(ompt_callback_report_task_dependence,
                (ompt_callback_t)&apex_report_task_dependence,
                "mutex_report_task_dependence");
#endif
            /* Event 23: sync region begin or end        */
            apex_ompt_register(ompt_callback_sync_region,
                (ompt_callback_t)&apex_ompt_sync_region, "sync_region");
            /* Event 31: begin or end idle state         */
//            apex_ompt_register(ompt_callback_idle,
//                (ompt_callback_t)&apex_ompt_idle, "idle");
#if 0
            /* Event 24: lock init                       */
            /* Event 25: lock destroy                    */
            /* Event 26: mutex acquire                   */
            apex_ompt_register(ompt_callback_mutex_acquire,
                (ompt_callback_t)&apex_mutex_acquire, "mutex_acquire");
            /* Event 27: mutex acquired                  */
            apex_ompt_register(ompt_callback_mutex_acquired,
                (ompt_callback_t)&apex_mutex_acquired, "mutex_acquired");
            /* Event 28: nest lock                       */
#endif
        }

    }

    DEBUG_PRINT("done.\n"); fflush(stderr);
    return 1;
}

void ompt_finalize(ompt_data_t* tool_data)
{
    APEX_UNUSED(tool_data);
    DEBUG_PRINT("OpenMP runtime is shutting down...\n");
    apex::finalize();
}

/* According to the OpenMP 5.0 specification, this function needs to be
 * defined in the application address space.  The runtime will see it,
 * and run it. */
ompt_start_tool_result_t * ompt_start_tool(
    unsigned int omp_version, const char *runtime_version) {
    APEX_UNUSED(runtime_version); // in case we aren't printing debug messages
    DEBUG_PRINT("APEX: OMPT Tool Start, version %d, '%s'\n",
        omp_version, runtime_version);
    if (_OPENMP != omp_version) {
       DEBUG_PRINT("APEX: WARNING! %d != %d (OpenMP Version used to compile APEX)\n",
          omp_version, _OPENMP);
    }
    static ompt_start_tool_result_t result;
    result.initialize = &ompt_initialize;
    result.finalize = &ompt_finalize;
    result.tool_data.value = 0L;
    result.tool_data.ptr = nullptr;
    return &result;
}

} // extern "C"
