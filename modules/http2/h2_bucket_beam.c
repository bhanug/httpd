/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_time.h>
#include <apr_buckets.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

#include <httpd.h>
#include <http_protocol.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_util.h"
#include "h2_bucket_beam.h"

static void h2_beam_emitted(h2_bucket_beam *beam, h2_beam_proxy *proxy);

#define H2_BPROXY_NEXT(e)             APR_RING_NEXT((e), link)
#define H2_BPROXY_PREV(e)             APR_RING_PREV((e), link)
#define H2_BPROXY_REMOVE(e)           APR_RING_REMOVE((e), link)

#define H2_BPROXY_LIST_INIT(b)        APR_RING_INIT(&(b)->list, h2_beam_proxy, link);
#define H2_BPROXY_LIST_SENTINEL(b)    APR_RING_SENTINEL(&(b)->list, h2_beam_proxy, link)
#define H2_BPROXY_LIST_EMPTY(b)       APR_RING_EMPTY(&(b)->list, h2_beam_proxy, link)
#define H2_BPROXY_LIST_FIRST(b)       APR_RING_FIRST(&(b)->list)
#define H2_BPROXY_LIST_LAST(b)	      APR_RING_LAST(&(b)->list)
#define H2_PROXY_BLIST_INSERT_HEAD(b, e) do {				\
	h2_beam_proxy *ap__b = (e);                                        \
	APR_RING_INSERT_HEAD(&(b)->list, ap__b, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_INSERT_TAIL(b, e) do {				\
	h2_beam_proxy *ap__b = (e);					\
	APR_RING_INSERT_TAIL(&(b)->list, ap__b, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_CONCAT(a, b) do {					\
        APR_RING_CONCAT(&(a)->list, &(b)->list, h2_beam_proxy, link);	\
    } while (0)
#define H2_BPROXY_LIST_PREPEND(a, b) do {					\
        APR_RING_PREPEND(&(a)->list, &(b)->list, h2_beam_proxy, link);	\
    } while (0)


/*******************************************************************************
 * beam bucket with reference to beam and bucket it represents
 ******************************************************************************/

const apr_bucket_type_t h2_bucket_type_beam;

#define H2_BUCKET_IS_BEAM(e)     (e->type == &h2_bucket_type_beam)

struct h2_beam_proxy {
    apr_bucket_refcount refcount;
    APR_RING_ENTRY(h2_beam_proxy) link;
    h2_bucket_beam *beam;
    apr_bucket *bred;
    apr_size_t n;
};

static const char Dummy = '\0';

static apr_status_t beam_bucket_read(apr_bucket *b, const char **str, 
                                     apr_size_t *len, apr_read_type_e block)
{
    h2_beam_proxy *d = b->data;
    if (d->bred) {
        const char *data;
        apr_status_t status = apr_bucket_read(d->bred, &data, len, block);
        if (status == APR_SUCCESS) {
            *str = data + b->start;
            *len = b->length;
        }
        return status;
    }
    *str = &Dummy;
    *len = 0;
    return APR_ECONNRESET;
}

static void beam_bucket_destroy(void *data)
{
    h2_beam_proxy *d = data;

    if (apr_bucket_shared_destroy(d)) {
        /* When the beam gets destroyed before this bucket, it will
         * NULLify its reference here. This is not protected by a mutex,
         * so it will not help with race conditions.
         * But it lets us shut down memory pool with circulare beam
         * references. */
        if (d->beam) {
            h2_beam_emitted(d->beam, d);
        }
        apr_bucket_free(d);
    }
}

static apr_bucket * h2_beam_bucket_make(apr_bucket *b, 
                                        h2_bucket_beam *beam,
                                        apr_bucket *bred, apr_size_t n)
{
    h2_beam_proxy *d;

    d = apr_bucket_alloc(sizeof(*d), b->list);
    H2_BPROXY_LIST_INSERT_TAIL(&beam->proxies, d);
    d->beam = beam;
    d->bred = bred;
    d->n = n;
    
    b = apr_bucket_shared_make(b, d, 0, bred? bred->length : 0);
    b->type = &h2_bucket_type_beam;

    return b;
}

static apr_bucket *h2_beam_bucket_create(h2_bucket_beam *beam,
                                         apr_bucket *bred,
                                         apr_bucket_alloc_t *list,
                                         apr_size_t n)
{
    apr_bucket *b = apr_bucket_alloc(sizeof(*b), list);

    APR_BUCKET_INIT(b);
    b->free = apr_bucket_free;
    b->list = list;
    return h2_beam_bucket_make(b, beam, bred, n);
}

const apr_bucket_type_t h2_bucket_type_beam = {
    "BEAM", 5, APR_BUCKET_DATA,
    beam_bucket_destroy,
    beam_bucket_read,
    apr_bucket_setaside_noop,
    apr_bucket_shared_split,
    apr_bucket_shared_copy
};

/*******************************************************************************
 * h2_blist, a brigade without allocations
 ******************************************************************************/

static apr_array_header_t *beamers;
 
void h2_register_bucket_beamer(h2_bucket_beamer *beamer)
{
    if (!beamers) {
        beamers = apr_array_make(apr_hook_global_pool, 10, 
                                 sizeof(h2_bucket_beamer*));
    }
    APR_ARRAY_PUSH(beamers, h2_bucket_beamer*) = beamer;
}

static apr_bucket *h2_beam_bucket(h2_bucket_beam *beam, 
                                  apr_bucket_brigade *dest,
                                  const apr_bucket *src)
{
    apr_bucket *b = NULL;
    int i;
    if (beamers) {
        for (i = 0; i < beamers->nelts && b == NULL; ++i) {
            h2_bucket_beamer *beamer;
            
            beamer = APR_ARRAY_IDX(beamers, i, h2_bucket_beamer*);
            b = beamer(beam, dest, src);
        }
    }
    return b;
}


apr_size_t h2_util_bl_print(char *buffer, apr_size_t bmax, 
                            const char *tag, const char *sep, 
                            h2_blist *bl)
{
    apr_size_t off = 0;
    const char *sp = "";
    apr_bucket *b;
    
    if (bl) {
        memset(buffer, 0, bmax--);
        off += apr_snprintf(buffer+off, bmax-off, "%s(", tag);
        for (b = H2_BLIST_FIRST(bl); 
             bmax && (b != H2_BLIST_SENTINEL(bl));
             b = APR_BUCKET_NEXT(b)) {
            
            off += h2_util_bucket_print(buffer+off, bmax-off, b, sp);
            sp = " ";
        }
        off += apr_snprintf(buffer+off, bmax-off, ")%s", sep);
    }
    else {
        off += apr_snprintf(buffer+off, bmax-off, "%s(null)%s", tag, sep);
    }
    return off;
}



/*******************************************************************************
 * bucket beam that can transport buckets across threads
 ******************************************************************************/

static apr_status_t enter_yellow(h2_bucket_beam *beam, h2_beam_lock *pbl)
{
    h2_beam_mutex_enter *enter = beam->m_enter;
    if (enter) {
        void *ctx = beam->m_ctx;
        if (ctx) {
            return enter(ctx, pbl);
        }
    }
    pbl->mutex = NULL;
    pbl->leave = NULL;
    return APR_SUCCESS;
}

static void leave_yellow(h2_bucket_beam *beam, h2_beam_lock *pbl)
{
    if (pbl->leave) {
        pbl->leave(pbl->leave_ctx, pbl->mutex);
    }
}

static void report_consumption(h2_bucket_beam *beam, int force)
{
    if (force || beam->received_bytes != beam->reported_consumed_bytes) {
        if (beam->consumed_fn) { 
            beam->consumed_fn(beam->consumed_ctx, beam, beam->received_bytes
                              - beam->reported_consumed_bytes);
        }
        beam->reported_consumed_bytes = beam->received_bytes;
    }
}

static void report_production(h2_bucket_beam *beam, int force)
{
    if (force || beam->sent_bytes != beam->reported_produced_bytes) {
        if (beam->produced_fn) { 
            beam->produced_fn(beam->produced_ctx, beam, beam->sent_bytes
                              - beam->reported_produced_bytes);
        }
        beam->reported_produced_bytes = beam->sent_bytes;
    }
}

static apr_size_t calc_buffered(h2_bucket_beam *beam)
{
    apr_size_t len = 0;
    apr_bucket *b;
    for (b = H2_BLIST_FIRST(&beam->send_list); 
         b != H2_BLIST_SENTINEL(&beam->send_list);
         b = APR_BUCKET_NEXT(b)) {
        if (b->length == ((apr_size_t)-1)) {
            /* do not count */
        }
        else if (APR_BUCKET_IS_FILE(b)) {
            /* if unread, has no real mem footprint. how to test? */
        }
        else {
            len += b->length;
        }
    }
    return len;
}

static void r_purge_sent(h2_bucket_beam *beam)
{
    apr_bucket *b;
    /* delete all sender buckets in purge brigade, needs to be called
     * from sender thread only */
    while (!H2_BLIST_EMPTY(&beam->purge_list)) {
        b = H2_BLIST_FIRST(&beam->purge_list);
        apr_bucket_delete(b);
    }
}

static apr_size_t calc_space_left(h2_bucket_beam *beam)
{
    if (beam->max_buf_size > 0) {
        apr_off_t len = calc_buffered(beam);
        return (beam->max_buf_size > len? (beam->max_buf_size - len) : 0);
    }
    return APR_SIZE_MAX;
}

static apr_status_t wait_cond(h2_bucket_beam *beam, apr_thread_mutex_t *lock)
{
    if (beam->timeout > 0) {
        return apr_thread_cond_timedwait(beam->m_cond, lock, beam->timeout);
    }
    else {
        return apr_thread_cond_wait(beam->m_cond, lock);
    }
}

static apr_status_t r_wait_space(h2_bucket_beam *beam, apr_read_type_e block,
                                 h2_beam_lock *pbl, apr_size_t *premain) 
{
    *premain = calc_space_left(beam);
    while (!beam->aborted && *premain <= 0 
           && (block == APR_BLOCK_READ) && pbl->mutex) {
        apr_status_t status;
        report_production(beam, 1);
        status = wait_cond(beam, pbl->mutex);
        if (APR_STATUS_IS_TIMEUP(status)) {
            return status;
        }
        r_purge_sent(beam);
        *premain = calc_space_left(beam);
    }
    return beam->aborted? APR_ECONNABORTED : APR_SUCCESS;
}

static void h2_beam_emitted(h2_bucket_beam *beam, h2_beam_proxy *proxy)
{
    h2_beam_lock bl;
    apr_bucket *b, *next;

    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        /* even when beam buckets are split, only the one where
         * refcount drops to 0 will call us */
        H2_BPROXY_REMOVE(proxy);
        /* invoked from receiver thread, the last beam bucket for the send
         * bucket is about to be destroyed.
         * remove it from the hold, where it should be now */
        if (proxy->bred) {
            for (b = H2_BLIST_FIRST(&beam->hold_list); 
                 b != H2_BLIST_SENTINEL(&beam->hold_list);
                 b = APR_BUCKET_NEXT(b)) {
                 if (b == proxy->bred) {
                    break;
                 }
            }
            if (b != H2_BLIST_SENTINEL(&beam->hold_list)) {
                /* bucket is in hold as it should be, mark this one
                 * and all before it for purging. We might have placed meta
                 * buckets without a green proxy into the hold before it 
                 * and schedule them for purging now */
                for (b = H2_BLIST_FIRST(&beam->hold_list); 
                     b != H2_BLIST_SENTINEL(&beam->hold_list);
                     b = next) {
                    next = APR_BUCKET_NEXT(b);
                    if (b == proxy->bred) {
                        APR_BUCKET_REMOVE(b);
                        H2_BLIST_INSERT_TAIL(&beam->purge_list, b);
                        break;
                    }
                    else if (APR_BUCKET_IS_METADATA(b)) {
                        APR_BUCKET_REMOVE(b);
                        H2_BLIST_INSERT_TAIL(&beam->purge_list, b);
                    }
                    else {
                        /* another data bucket before this one in hold. this
                         * is normal since DATA buckets need not be destroyed
                         * in order */
                    }
                }
                
                proxy->bred = NULL;
            }
            else {
                /* it should be there unless we screwed up */
                ap_log_perror(APLOG_MARK, APLOG_WARNING, 0, beam->send_pool, 
                              APLOGNO(03384) "h2_beam(%d-%s): emitted bucket not "
                              "in hold, n=%d", beam->id, beam->tag, 
                              (int)proxy->n);
                ap_assert(!proxy->bred);
            }
        }
        /* notify anyone waiting on space to become available */
        if (!bl.mutex) {
            r_purge_sent(beam);
        }
        else if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
        leave_yellow(beam, &bl);
    }
}

static void h2_blist_cleanup(h2_blist *bl)
{
    apr_bucket *e;

    while (!H2_BLIST_EMPTY(bl)) {
        e = H2_BLIST_FIRST(bl);
        apr_bucket_delete(e);
    }
}

static apr_status_t beam_close(h2_bucket_beam *beam)
{
    if (!beam->closed) {
        beam->closed = 1;
        if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
    }
    return APR_SUCCESS;
}

static void beam_set_send_pool(h2_bucket_beam *beam, apr_pool_t *pool);
static void beam_set_recv_pool(h2_bucket_beam *beam, apr_pool_t *pool);

static apr_status_t beam_recv_cleanup(void *data)
{
    h2_bucket_beam *beam = data;
    /* receiver pool has gone away, clear references */
    beam->recv_buffer = NULL;
    beam->recv_pool = NULL;
    return APR_SUCCESS;
}

static void beam_set_recv_pool(h2_bucket_beam *beam, apr_pool_t *pool) 
{
    /* if the beam owner is the sender, monitor receiver pool lifetime */ 
    if (beam->owner == H2_BEAM_OWNER_SEND && beam->recv_pool != pool) {
        if (beam->recv_pool) {
            apr_pool_cleanup_kill(beam->recv_pool, beam, beam_recv_cleanup);
        }
        beam->recv_pool = pool;
        if (beam->recv_pool) {
            apr_pool_pre_cleanup_register(beam->recv_pool, beam, beam_recv_cleanup);
        }
    }
}

static apr_status_t beam_send_cleanup(void *data)
{
    h2_bucket_beam *beam = data;
    /* sender has gone away, clear up all references to its memory */
    r_purge_sent(beam);
    h2_blist_cleanup(&beam->send_list);
    report_consumption(beam, 0);
    while (!H2_BPROXY_LIST_EMPTY(&beam->proxies)) {
        h2_beam_proxy *proxy = H2_BPROXY_LIST_FIRST(&beam->proxies);
        H2_BPROXY_REMOVE(proxy);
        proxy->beam = NULL;
        proxy->bred = NULL;
    }
    h2_blist_cleanup(&beam->purge_list);
    h2_blist_cleanup(&beam->hold_list);
    beam->send_pool = NULL;
    return APR_SUCCESS;
}

static void beam_set_send_pool(h2_bucket_beam *beam, apr_pool_t *pool) 
{
    /* if the beam owner is the receiver, monitor sender pool lifetime */
    if (beam->owner == H2_BEAM_OWNER_RECV && beam->send_pool != pool) {
        if (beam->send_pool) {
            apr_pool_cleanup_kill(beam->send_pool, beam, beam_send_cleanup);
        }
        beam->send_pool = pool;
        if (beam->send_pool) {
            apr_pool_pre_cleanup_register(beam->send_pool, beam, beam_send_cleanup);
        }
    }
}

static apr_status_t beam_cleanup(void *data)
{
    h2_bucket_beam *beam = data;
    apr_status_t status = APR_SUCCESS;
    /* owner of the beam is going away, depending on its role, cleanup
     * strategies differ. */
    beam_close(beam);
    switch (beam->owner) {
        case H2_BEAM_OWNER_SEND:
            status = beam_send_cleanup(beam);
            beam->recv_buffer = NULL;
            beam->recv_pool = NULL;
            break;
        case H2_BEAM_OWNER_RECV:
            if (beam->recv_buffer) {
                apr_brigade_destroy(beam->recv_buffer);
            }
            beam->recv_buffer = NULL;
            beam->recv_pool = NULL;
            if (!H2_BLIST_EMPTY(&beam->send_list)) {
                ap_assert(beam->send_pool);
            }
            if (beam->send_pool) {
                /* sender has not cleaned up, its pool still lives.
                 * this is normal if the sender uses cleanup via a bucket
                 * such as the BUCKET_EOR for requests. In that case, the
                 * beam should have lost its mutex protection, meaning
                 * it is no longer used multi-threaded and we can safely
                 * purge all remaining sender buckets. */
                apr_pool_cleanup_kill(beam->send_pool, beam, beam_send_cleanup);
                ap_assert(!beam->m_enter);
                beam_send_cleanup(beam);
            }
            ap_assert(H2_BPROXY_LIST_EMPTY(&beam->proxies));
            ap_assert(H2_BLIST_EMPTY(&beam->send_list));
            ap_assert(H2_BLIST_EMPTY(&beam->hold_list));
            ap_assert(H2_BLIST_EMPTY(&beam->purge_list));
            break;
        default:
            ap_assert(NULL);
            break;
    }
    return status;
}

apr_status_t h2_beam_destroy(h2_bucket_beam *beam)
{
    apr_pool_cleanup_kill(beam->pool, beam, beam_cleanup);
    return beam_cleanup(beam);
}

apr_status_t h2_beam_create(h2_bucket_beam **pbeam, apr_pool_t *pool, 
                            int id, const char *tag, 
                            h2_beam_owner_t owner,
                            apr_size_t max_buf_size)
{
    h2_bucket_beam *beam;
    apr_status_t status = APR_SUCCESS;
    
    beam = apr_pcalloc(pool, sizeof(*beam));
    if (!beam) {
        return APR_ENOMEM;
    }

    beam->id = id;
    beam->tag = tag;
    beam->pool = pool;
    beam->owner = owner;
    H2_BLIST_INIT(&beam->send_list);
    H2_BLIST_INIT(&beam->hold_list);
    H2_BLIST_INIT(&beam->purge_list);
    H2_BPROXY_LIST_INIT(&beam->proxies);
    beam->max_buf_size = max_buf_size;
    apr_pool_pre_cleanup_register(pool, beam, beam_cleanup);

    *pbeam = beam;
    
    return status;
}

void h2_beam_buffer_size_set(h2_bucket_beam *beam, apr_size_t buffer_size)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->max_buf_size = buffer_size;
        leave_yellow(beam, &bl);
    }
}

apr_size_t h2_beam_buffer_size_get(h2_bucket_beam *beam)
{
    h2_beam_lock bl;
    apr_size_t buffer_size = 0;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        buffer_size = beam->max_buf_size;
        leave_yellow(beam, &bl);
    }
    return buffer_size;
}

void h2_beam_mutex_set(h2_bucket_beam *beam, 
                       h2_beam_mutex_enter m_enter,
                       apr_thread_cond_t *cond,
                       void *m_ctx)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->m_enter = m_enter;
        beam->m_ctx   = m_ctx;
        beam->m_cond  = cond;
        leave_yellow(beam, &bl);
    }
}

void h2_beam_timeout_set(h2_bucket_beam *beam, apr_interval_time_t timeout)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->timeout = timeout;
        leave_yellow(beam, &bl);
    }
}

apr_interval_time_t h2_beam_timeout_get(h2_bucket_beam *beam)
{
    h2_beam_lock bl;
    apr_interval_time_t timeout = 0;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        timeout = beam->timeout;
        leave_yellow(beam, &bl);
    }
    return timeout;
}

void h2_beam_abort(h2_bucket_beam *beam)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        if (!beam->aborted) {
            beam->aborted = 1;
            r_purge_sent(beam);
            h2_blist_cleanup(&beam->send_list);
            report_consumption(beam, 0);
        }
        if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
        leave_yellow(beam, &bl);
    }
}

apr_status_t h2_beam_close(h2_bucket_beam *beam)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        r_purge_sent(beam);
        beam_close(beam);
        report_consumption(beam, 0);
        leave_yellow(beam, &bl);
    }
    return beam->aborted? APR_ECONNABORTED : APR_SUCCESS;
}

apr_status_t h2_beam_wait_empty(h2_bucket_beam *beam, apr_read_type_e block)
{
    apr_status_t status;
    h2_beam_lock bl;
    
    if ((status = enter_yellow(beam, &bl)) == APR_SUCCESS) {
        while (status == APR_SUCCESS
               && !H2_BLIST_EMPTY(&beam->send_list)
               && !H2_BPROXY_LIST_EMPTY(&beam->proxies)) {
            if (block == APR_NONBLOCK_READ || !bl.mutex) {
                status = APR_EAGAIN;
                break;
            }
            if (beam->m_cond) {
                apr_thread_cond_broadcast(beam->m_cond);
            }
            status = wait_cond(beam, bl.mutex);
        }
        leave_yellow(beam, &bl);
    }
    return status;
}

static void move_to_hold(h2_bucket_beam *beam, 
                         apr_bucket_brigade *red_brigade)
{
    apr_bucket *b;
    while (red_brigade && !APR_BRIGADE_EMPTY(red_brigade)) {
        b = APR_BRIGADE_FIRST(red_brigade);
        APR_BUCKET_REMOVE(b);
        H2_BLIST_INSERT_TAIL(&beam->send_list, b);
    }
}

static apr_status_t append_bucket(h2_bucket_beam *beam, 
                                  apr_bucket *b,
                                  apr_read_type_e block,
                                  h2_beam_lock *pbl)
{
    const char *data;
    apr_size_t len;
    apr_size_t space_left = 0;
    apr_status_t status;
    
    if (APR_BUCKET_IS_METADATA(b)) {
        if (APR_BUCKET_IS_EOS(b)) {
            beam->closed = 1;
        }
        APR_BUCKET_REMOVE(b);
        H2_BLIST_INSERT_TAIL(&beam->send_list, b);
        return APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_FILE(b)) {
        /* file bucket lengths do not really count */
    }
    else {
        space_left = calc_space_left(beam);
        if (space_left > 0 && b->length == ((apr_size_t)-1)) {
            const char *data;
            status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
            if (status != APR_SUCCESS) {
                return status;
            }
        }
        
        if (space_left < b->length) {
            status = r_wait_space(beam, block, pbl, &space_left);
            if (status != APR_SUCCESS) {
                return status;
            }
            if (space_left <= 0) {
                return APR_EAGAIN;
            }
        }
        /* space available, maybe need bucket split */
    }
    

    /* The fundamental problem is that reading a red bucket from
     * a green thread is a total NO GO, because the bucket might use
     * its pool/bucket_alloc from a foreign thread and that will
     * corrupt. */
    status = APR_ENOTIMPL;
    if (APR_BUCKET_IS_TRANSIENT(b)) {
        /* this takes care of transient buckets and converts them
         * into heap ones. Other bucket types might or might not be
         * affected by this. */
        status = apr_bucket_setaside(b, beam->send_pool);
    }
    else if (APR_BUCKET_IS_HEAP(b)) {
        /* For heap buckets read from a green thread is fine. The
         * data will be there and live until the bucket itself is
         * destroyed. */
        status = APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_POOL(b)) {
        /* pool buckets are bastards that register at pool cleanup
         * to morph themselves into heap buckets. That may happen anytime,
         * even after the bucket data pointer has been read. So at
         * any time inside the green thread, the pool bucket memory
         * may disappear. yikes. */
        status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
        if (status == APR_SUCCESS) {
            apr_bucket_heap_make(b, data, len, NULL);
        }
    }
    else if (APR_BUCKET_IS_FILE(b)) {
        /* For file buckets the problem is their internal readpool that
         * is used on the first read to allocate buffer/mmap.
         * Since setting aside a file bucket will de-register the
         * file cleanup function from the previous pool, we need to
         * call that from a red thread. 
         * Additionally, we allow callbacks to prevent beaming file
         * handles across. The use case for this is to limit the number 
         * of open file handles and rather use a less efficient beam
         * transport. */
        apr_file_t *fd = ((apr_bucket_file *)b->data)->fd;
        int can_beam = 1;
        if (beam->last_beamed != fd && beam->can_beam_fn) {
            can_beam = beam->can_beam_fn(beam->can_beam_ctx, beam, fd);
        }
        if (can_beam) {
            beam->last_beamed = fd;
            status = apr_bucket_setaside(b, beam->send_pool);
        }
        /* else: enter ENOTIMPL case below */
    }
    
    if (status == APR_ENOTIMPL) {
        /* we have no knowledge about the internals of this bucket,
         * but hope that after read, its data stays immutable for the
         * lifetime of the bucket. (see pool bucket handling above for
         * a counter example).
         * We do the read while in a red thread, so that the bucket may
         * use pools/allocators safely. */
        if (space_left < APR_BUCKET_BUFF_SIZE) {
            space_left = APR_BUCKET_BUFF_SIZE;
        }
        if (space_left < b->length) {
            apr_bucket_split(b, space_left);
        }
        status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
        if (status == APR_SUCCESS) {
            status = apr_bucket_setaside(b, beam->send_pool);
        }
    }
    
    if (status != APR_SUCCESS && status != APR_ENOTIMPL) {
        return status;
    }
    
    APR_BUCKET_REMOVE(b);
    H2_BLIST_INSERT_TAIL(&beam->send_list, b);
    beam->sent_bytes += b->length;
    
    return APR_SUCCESS;
}

apr_status_t h2_beam_send(h2_bucket_beam *beam, 
                          apr_bucket_brigade *red_brigade, 
                          apr_read_type_e block)
{
    apr_bucket *b;
    apr_status_t status = APR_SUCCESS;
    h2_beam_lock bl;

    /* Called from the red thread to add buckets to the beam */
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        r_purge_sent(beam);
        if (red_brigade) {
            beam_set_send_pool(beam, red_brigade->p);
        }
        
        if (beam->aborted) {
            move_to_hold(beam, red_brigade);
            status = APR_ECONNABORTED;
        }
        else if (red_brigade) {
            int force_report = !APR_BRIGADE_EMPTY(red_brigade); 
            while (!APR_BRIGADE_EMPTY(red_brigade)
                   && status == APR_SUCCESS) {
                b = APR_BRIGADE_FIRST(red_brigade);
                status = append_bucket(beam, b, block, &bl);
            }
            report_production(beam, force_report);
            if (beam->m_cond) {
                apr_thread_cond_broadcast(beam->m_cond);
            }
        }
        report_consumption(beam, 0);
        leave_yellow(beam, &bl);
    }
    return status;
}

apr_status_t h2_beam_receive(h2_bucket_beam *beam, 
                             apr_bucket_brigade *bb, 
                             apr_read_type_e block,
                             apr_off_t readbytes)
{
    h2_beam_lock bl;
    apr_bucket *bred, *bgreen, *ng;
    int transferred = 0;
    apr_status_t status = APR_SUCCESS;
    apr_off_t remain = readbytes;
    
    /* Called from the green thread to take buckets from the beam */
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
transfer:
        if (beam->aborted) {
            if (beam->recv_buffer && !APR_BRIGADE_EMPTY(beam->recv_buffer)) {
                apr_brigade_cleanup(beam->recv_buffer);
            }
            status = APR_ECONNABORTED;
            goto leave;
        }

        /* transfer enough buckets from our green brigade, if we have one */
        beam_set_recv_pool(beam, bb->p);
        while (beam->recv_buffer
               && !APR_BRIGADE_EMPTY(beam->recv_buffer)
               && (readbytes <= 0 || remain >= 0)) {
            bgreen = APR_BRIGADE_FIRST(beam->recv_buffer);
            if (readbytes > 0 && bgreen->length > 0 && remain <= 0) {
                break;
            }            
            APR_BUCKET_REMOVE(bgreen);
            APR_BRIGADE_INSERT_TAIL(bb, bgreen);
            remain -= bgreen->length;
            ++transferred;
        }

        /* transfer from our red brigade, transforming red buckets to
         * green ones until we have enough */
        while (!H2_BLIST_EMPTY(&beam->send_list) && (readbytes <= 0 || remain >= 0)) {
            bred = H2_BLIST_FIRST(&beam->send_list);
            bgreen = NULL;
            
            if (readbytes > 0 && bred->length > 0 && remain <= 0) {
                break;
            }
                        
            if (APR_BUCKET_IS_METADATA(bred)) {
                if (APR_BUCKET_IS_EOS(bred)) {
                    bgreen = apr_bucket_eos_create(bb->bucket_alloc);
                    beam->close_sent = 1;
                }
                else if (APR_BUCKET_IS_FLUSH(bred)) {
                    bgreen = apr_bucket_flush_create(bb->bucket_alloc);
                }
                else if (AP_BUCKET_IS_ERROR(bred)) {
                    ap_bucket_error *eb = (ap_bucket_error *)bred;
                    bgreen = ap_bucket_error_create(eb->status, eb->data,
                                                    bb->p, bb->bucket_alloc);
                }
            }
            else if (APR_BUCKET_IS_FILE(bred)) {
                /* This is set aside into the target brigade pool so that 
                 * any read operation messes with that pool and not 
                 * the red one. */
                apr_bucket_file *f = (apr_bucket_file *)bred->data;
                apr_file_t *fd = f->fd;
                int setaside = (f->readpool != bb->p);
                
                if (setaside) {
                    status = apr_file_setaside(&fd, fd, bb->p);
                    if (status != APR_SUCCESS) {
                        goto leave;
                    }
                    ++beam->files_beamed;
                }
                ng = apr_brigade_insert_file(bb, fd, bred->start, bred->length, 
                                             bb->p);
#if APR_HAS_MMAP
                /* disable mmap handling as this leads to segfaults when
                 * the underlying file is changed while memory pointer has
                 * been handed out. See also PR 59348 */
                apr_bucket_file_enable_mmap(ng, 0);
#endif
                remain -= bred->length;
                ++transferred;
                APR_BUCKET_REMOVE(bred);
                H2_BLIST_INSERT_TAIL(&beam->hold_list, bred);
                ++transferred;
                continue;
            }
            else {
                /* create a "green" standin bucket. we took care about the
                 * underlying red bucket and its data when we placed it into
                 * the red brigade.
                 * the beam bucket will notify us on destruction that bred is
                 * no longer needed. */
                bgreen = h2_beam_bucket_create(beam, bred, bb->bucket_alloc,
                                               beam->buckets_sent++);
            }
            
            /* Place the red bucket into our hold, to be destroyed when no
             * green bucket references it any more. */
            APR_BUCKET_REMOVE(bred);
            H2_BLIST_INSERT_TAIL(&beam->hold_list, bred);
            beam->received_bytes += bred->length;
            if (bgreen) {
                APR_BRIGADE_INSERT_TAIL(bb, bgreen);
                remain -= bgreen->length;
                ++transferred;
            }
            else {
                bgreen = h2_beam_bucket(beam, bb, bred);
                while (bgreen && bgreen != APR_BRIGADE_SENTINEL(bb)) {
                    ++transferred;
                    remain -= bgreen->length;
                    bgreen = APR_BUCKET_NEXT(bgreen);
                }
            }
        }

        if (readbytes > 0 && remain < 0) {
            /* too much, put some back */
            remain = readbytes;
            for (bgreen = APR_BRIGADE_FIRST(bb);
                 bgreen != APR_BRIGADE_SENTINEL(bb);
                 bgreen = APR_BUCKET_NEXT(bgreen)) {
                 remain -= bgreen->length;
                 if (remain < 0) {
                     apr_bucket_split(bgreen, bgreen->length+remain);
                     beam->recv_buffer = apr_brigade_split_ex(bb, 
                                                        APR_BUCKET_NEXT(bgreen), 
                                                        beam->recv_buffer);
                     break;
                 }
            }
        }

        if (beam->closed 
            && (!beam->recv_buffer || APR_BRIGADE_EMPTY(beam->recv_buffer))
            && H2_BLIST_EMPTY(&beam->send_list)) {
            /* beam is closed and we have nothing more to receive */ 
            if (!beam->close_sent) {
                apr_bucket *b = apr_bucket_eos_create(bb->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(bb, b);
                beam->close_sent = 1;
                ++transferred;
                status = APR_SUCCESS;
            }
        }
        
        if (transferred) {
            if (beam->m_cond) {
                apr_thread_cond_broadcast(beam->m_cond);
            }
            status = APR_SUCCESS;
        }
        else if (beam->closed) {
            status = APR_EOF;
        }
        else if (block == APR_BLOCK_READ && bl.mutex && beam->m_cond) {
            status = wait_cond(beam, bl.mutex);
            if (status != APR_SUCCESS) {
                goto leave;
            }
            goto transfer;
        }
        else {
            if (beam->m_cond) {
                apr_thread_cond_broadcast(beam->m_cond);
            }
            status = APR_EAGAIN;
        }
leave:        
        leave_yellow(beam, &bl);
    }
    return status;
}

void h2_beam_on_consumed(h2_bucket_beam *beam, 
                         h2_beam_io_callback *cb, void *ctx)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->consumed_fn = cb;
        beam->consumed_ctx = ctx;
        leave_yellow(beam, &bl);
    }
}

void h2_beam_on_produced(h2_bucket_beam *beam, 
                         h2_beam_io_callback *cb, void *ctx)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->produced_fn = cb;
        beam->produced_ctx = ctx;
        leave_yellow(beam, &bl);
    }
}

void h2_beam_on_file_beam(h2_bucket_beam *beam, 
                          h2_beam_can_beam_callback *cb, void *ctx)
{
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        beam->can_beam_fn = cb;
        beam->can_beam_ctx = ctx;
        leave_yellow(beam, &bl);
    }
}


apr_off_t h2_beam_get_buffered(h2_bucket_beam *beam)
{
    apr_bucket *b;
    apr_off_t l = 0;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        for (b = H2_BLIST_FIRST(&beam->send_list); 
            b != H2_BLIST_SENTINEL(&beam->send_list);
            b = APR_BUCKET_NEXT(b)) {
            /* should all have determinate length */
            l += b->length;
        }
        leave_yellow(beam, &bl);
    }
    return l;
}

apr_off_t h2_beam_get_mem_used(h2_bucket_beam *beam)
{
    apr_bucket *b;
    apr_off_t l = 0;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        for (b = H2_BLIST_FIRST(&beam->send_list); 
            b != H2_BLIST_SENTINEL(&beam->send_list);
            b = APR_BUCKET_NEXT(b)) {
            if (APR_BUCKET_IS_FILE(b)) {
                /* do not count */
            }
            else {
                /* should all have determinate length */
                l += b->length;
            }
        }
        leave_yellow(beam, &bl);
    }
    return l;
}

int h2_beam_empty(h2_bucket_beam *beam)
{
    int empty = 1;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        empty = (H2_BLIST_EMPTY(&beam->send_list) 
                 && (!beam->recv_buffer || APR_BRIGADE_EMPTY(beam->recv_buffer)));
        leave_yellow(beam, &bl);
    }
    return empty;
}

int h2_beam_holds_proxies(h2_bucket_beam *beam)
{
    int has_proxies = 1;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        has_proxies = !H2_BPROXY_LIST_EMPTY(&beam->proxies);
        leave_yellow(beam, &bl);
    }
    return has_proxies;
}

int h2_beam_was_received(h2_bucket_beam *beam)
{
    int happend = 0;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        happend = (beam->received_bytes > 0);
        leave_yellow(beam, &bl);
    }
    return happend;
}

apr_size_t h2_beam_get_files_beamed(h2_bucket_beam *beam)
{
    apr_size_t n = 0;
    h2_beam_lock bl;
    
    if (enter_yellow(beam, &bl) == APR_SUCCESS) {
        n = beam->files_beamed;
        leave_yellow(beam, &bl);
    }
    return n;
}

int h2_beam_no_files(void *ctx, h2_bucket_beam *beam, apr_file_t *file)
{
    return 0;
}

