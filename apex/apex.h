/*  Copyright (c) 2014 University of Oregon
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */

/*
 * APEX external API
 *
 */

/*
 * The C API is not required for HPX support. 
 * But don't delete it just yet. 
 */

#ifndef APEX_H
#define APEX_H

#include "apex_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void apex_init(const char * thread_name);
void apex_init_args(int argc, char** argv, const char * thread_name);
void apex_finalize();
void* apex_start(const char * timer_name);
//void apex_resume(const char * timer_name);
//void apex_stop(const char * timer_name);
void apex_stop_profiler(void * profiler);
void* apex_start_addr(void * function_address);
//void apex_resume_addr(void * function_address);
void apex_resume_profiler(void * profiler);
//void apex_stop_addr(void * function_address);
void apex_sample_value(const char * name, double value);
void apex_set_node_id(int id);
double apex_version(void);
void apex_node_id(int id);
void apex_register_thread(const char * name);
void apex_track_power(void);
void apex_track_power_here(void);
void apex_enable_tracking_power(void);
void apex_disable_tracking_power(void);
void apex_set_interrupt_interval(int seconds);
apex_policy_handle* apex_register_policy(const apex_event_type when, int (*f)(apex_context const));
apex_policy_handle* apex_register_periodic_policy(unsigned long period, int (*f)(apex_context const));
apex_profile* apex_get_profile(void *);

#ifdef __cplusplus
}
#endif

#endif //APEX_H
