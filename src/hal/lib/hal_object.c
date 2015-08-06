//  non-inlined  hal_object_t accessors
//  inlined accessors are in hal_object.h
#include "config.h"
#include "rtapi.h"
#include "rtapi_mbarrier.h"
#include "hal.h"
#include "hal_priv.h"
#include "hal_object.h"
#include "hal_list.h"
#include "hal_internal.h"

#define MAX_OBJECT_NAME_LEN 127

int hh_set_namefv(halhdr_t *hh, const char *fmt, va_list ap)
{
    char buf[MAX_OBJECT_NAME_LEN];
    int sz = rtapi_vsnprintf(buf, sizeof(buf), fmt, ap);
    if(sz == -1 || sz > sizeof(buf)) {
        HALFAIL_RC(ENOMEM, "length %d invalid for name starting with '%s'",
		   sz, buf);
    }
    char *s = shmalloc_desc(sz + 1); // include trailing zero
    if (s == NULL) {
        HALFAIL_RC(ENOMEM, "out of memory allocating %d bytes for '%s'",
	       sz+1, buf);
    }
    strcpy(s, buf);
    hh->_name_ptr = SHMOFF(s);
    hal_data->str_alloc += (sz + 1);
    return 0;
}

int hh_set_namef(halhdr_t *hh, const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = hh_set_namefv(hh, fmt, ap);
    va_end(ap);
    return ret;
}

int  hh_init_hdrfv(halhdr_t *hh,
		   const hal_object_type type,
		   const int owner_id,
		   const char *fmt, va_list ap)
{
    dlist_init_entry(&hh->list);
    hh_set_object_type(hh, type);
    hh_set_id(hh, rtapi_next_handle());
    hh_set_owner_id(hh, owner_id);
    hh_set_valid(hh);
    return hh_set_namefv(hh, fmt, ap);
}

int  hh_init_hdrf(halhdr_t *hh,
		  const hal_object_type type,
		  const int owner_id,
		  const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hh_init_hdrfv(hh, type, owner_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hh_clear_hdr(halhdr_t *hh)
{
    int ret = hh_is_valid(hh);
    hh_set_id(hh, 0);
    hh_set_owner_id(hh, 0);
    if (hh->_name_ptr) {
	void *s = SHMPTR(hh->_name_ptr);
	hal_data->str_freed += strlen(s) + 1;
	shmfree_desc(s);
	hh->_name_ptr = 0;
    }
    hh_set_invalid(hh);
    hh->_refcnt = 0;
    return ret;
}

const char *hal_object_typestr(const unsigned type)
{
    switch (type) {
    case HAL_PIN           : return "PIN"; break;
    case HAL_SIGNAL        : return "SIGNAL"; break;
    case HAL_PARAM         : return "PARAM"; break;
    case HAL_THREAD        : return "THREAD"; break;
    case HAL_FUNCT         : return "FUNCT"; break;
    case HAL_COMPONENT     : return "COMPONENT"; break;
    case HAL_VTABLE        : return "VTABLE"; break;
    case HAL_INST          : return "INST"; break;
    case HAL_RING          : return "RING"; break;
    case HAL_GROUP         : return "GROUP"; break;
    case HAL_MEMBER        : return "MEMBER"; break;
    case HAL_PLUG          : return "PLUG"; break;
    default:  return "**invalid**"; break;
    }
}

int hh_snprintf(char *buf, size_t size, const halhdr_t *hh)
{
    return rtapi_snprintf(buf, size,
			  "%s %s id=%d owner=%d valid=%d refcnt=%d "
			  "legacy=%d rmb=%d wmb=%d",
			  hh_get_object_typestr(hh),
			  hh_get_name(hh),
			  hh_get_id(hh),
			  hh_get_owner_id(hh),
			  hh_is_valid(hh),
			  hh_get_refcnt(hh),
			  hh_get_legacy(hh),
			  hh_get_rmb(hh),
			  hh_get_wmb(hh));
}

void *halg_create_objectfv(const bool use_hal_mutex,
			  const size_t size,
			  const int type,
			  const int owner_id,
			  const char *fmt,
			  va_list ap)
{
    halhdr_t *new = shmalloc_desc(size);
    if (new == NULL) {
	char name[HAL_MAX_NAME_LEN+1];
	rtapi_vsnprintf(name, sizeof(name), fmt, ap);
	HALFAIL_NULL(ENOMEM, "insufficient memory for %s %s size=%zu",
		     hal_object_typestr(type), name, size);
    }
    int ret =  hh_init_hdrfv(new, type, owner_id, fmt, ap);
    if (ret) {
	shmfree_desc(new);
	return NULL;
    }
    return new;
}


// iterator callback for halg_add_object()
// determines insertion point
static int find_previous(hal_object_ptr o, foreach_args_t *args)
{
    hal_object_ptr new = (hal_object_ptr) args->user_ptr1;

    // compare the name of the current object to the name of
    // the object to be inserted:
    int ret = strcmp(hh_get_name(new.hdr), hh_get_name(o.hdr));
    if (ret < 0) {
	// advance point of insertion:
	args->user_ptr2 = (void *)&o.hdr->list;
	return 1; // stop iteration
    }
    return 0; // continue iteration
}

void halg_add_object(const bool use_hal_mutex,
		     hal_object_ptr o)
{
    // args.user_ptr2 is the potential point of insertion
    // for the new object within the object list.
    // start at head, and iterate over objects of the same type.
    foreach_args_t args =  {
	// visit objects of the same type as the one to be inserted:
	.type = hh_get_object_type(o.hdr),
	// object to be inserted
	.user_ptr1 = o.any,
	// current head
	.user_ptr2 = OBJECTLIST,
    };
    halg_foreach(0, &args, find_previous);
    // insert new object after the new insertion point.
    // if nothing found, insert after head.
    dlist_add_before(&o.hdr->list, args.user_ptr2);

    // make sure all values visible everywhere
    rtapi_smp_mb();
}

int halg_free_object(const bool use_hal_mutex,
		      hal_object_ptr o)
{
    WITH_HAL_MUTEX_IF(use_hal_mutex);

    if (hh_get_refcnt(o.hdr)) {
	HALFAIL_RC(EBUSY, "not deleting %s %s - still referenced (refcount=%d)",
		   hh_get_object_typestr(o.hdr),
		   hh_get_name(o.hdr),
		   hh_get_refcnt(o.hdr));
    }
    // unlink from list of active objects
    dlist_remove_entry(&o.hdr->list);
    // zap the header, including valid bit
    hh_clear_hdr(o.hdr);
    // return descriptor memory to HAL heap
    shmfree_desc(o.hdr);

    // make sure all values visible everywhere
    rtapi_smp_mb();
    return 0;
}

int halg_foreach(bool use_hal_mutex,
		 foreach_args_t *args,
		 hal_object_callback_t callback)
{
    halhdr_t *hh, *tmp;
    int nvisited = 0, result;

    CHECK_NULL(args);
    {
	// run with HAL mutex if use_hal_mutex nonzero:
	WITH_HAL_MUTEX_IF(use_hal_mutex);

	dlist_for_each_entry_safe(hh, tmp, OBJECTLIST, list) {

	    // cop out if the object list emptied by now
	    // (may happen through callbacks when deleting)
	    if (dlist_empty_careful(&hh->list))
		break;

	    // 1. select by type if given
	    if (args->type && (hh_get_object_type(hh) != args->type))
		continue;

	    // 2. by id if nonzero
	    if  (args->id && (args->id != hh_get_id(hh)))
		continue;

	    // 3. by owner id if nonzero
	    if (args->owner_id && (args->owner_id != hh_get_owner_id(hh)))
		continue;

	    // 4. by owning comp (directly-legacy case, or indirectly -
	    // for pins, params and functs owned by an instance).
	    // see comments near the foreach_args definition in hal_object.h.
	    if (args->owning_comp) {
		hal_comp_t *oc = halpr_find_owning_comp(hh_get_owner_id(hh));
		if (oc == NULL)
		    continue;  // a bug, halpr_find_owning_comp will log already
		if (!(ho_id(oc) == args->owning_comp))
		    continue;
	    }

	    // 5. by name if non-NULL - prefix match OK for name strings
	    if (args->name && strcmp(hh_get_name(hh), args->name))
		continue;

	    nvisited++;
	    if (callback) {
		result = callback((hal_object_ptr)hh, args);
		if (result < 0) {
		    // callback signalled an error, pass that back up.
		    return result;
		} else if (result > 0) {
		    // callback signalled 'stop iterating'.
		    // pass back the number of visited objects sp far.
		    return nvisited;
		} else {
		    // callback signalled 'OK to continue'
		    // fall through
		}
	    } else {
		// null callback passed in.
		// same meaning as returning 0 from the callback:
		// continue iterating.
		// return value will be the number of matches.
	    }
	}
    } // no match, try the next one

    // if we get here, we ran through all the matched objects,
    // so return match count
    return nvisited;
}


// specialisations for common tasks

// set read and/or write barriers on a HAL object
// read_barrier, write_barrier values:
//   0..unset
//   1..set
//  -1..leave as is
int halg_object_setbarriers(const int use_hal_mutex,
			    hal_object_ptr o,
			    const int read_barrier,
			    const int write_barrier)
{
    CHECK_HALDATA();
    CHECK_NULL(o.any);
    {
	WITH_HAL_MUTEX_IF(use_hal_mutex);

	if (!hh_valid(o.hdr)) {
	    HALFAIL_RC(EINVAL, "object at %p invalid",  o.any);
	}
	switch (hh_get_object_type(o.hdr)) {
	case HAL_PIN:
	case HAL_PARAM:
	    if (hh_get_legacy(o.hdr)) {
		HALFAIL_RC(EINVAL, "barriers not supported on legacy %s %s",
			   hh_get_object_typestr(o.hdr), hh_get_name(o.hdr));
	    }
	    break;
	default: ;
	}
	bool old_rmb = hh_get_rmb(o.hdr);
	bool old_wmb = hh_get_wmb(o.hdr);
	if (read_barrier > -1)
	    hh_set_rmb(o.hdr, read_barrier);
	if (write_barrier > -1)
	    hh_set_wmb(o.hdr, write_barrier);
	HALDBG("setting barriers on %s '%s': rmb: %d->%d  wmb: %d->%d",
	       hh_get_object_typestr(o.hdr),
	       hh_get_name(o.hdr),
	       old_rmb, hh_get_rmb(o.hdr),
	       old_wmb, hh_get_wmb(o.hdr));

	// if setting barriers on signal, propagate to pins:
	if (hh_get_object_type(o.hdr) == HAL_SIGNAL)
	    halg_signal_propagate_barriers(0, o.sig);
    }
    return 0;
}

hal_object_ptr halg_find_object_by_name(const int use_hal_mutex,
					const int type,
					const char *name)
{
    foreach_args_t args =  {
	.type = type,
	.name = (char *)name,
    };
    if (halg_foreach(use_hal_mutex, &args, yield_match))
	return (hal_object_ptr) args.user_ptr1;
    return HO_NULL;
}

hal_object_ptr halg_find_object_by_id(const int use_hal_mutex,
				      const int type,
				      const int id)
{
    foreach_args_t args =  {
	.type = type,
	.id = id
    };
    if (halg_foreach(use_hal_mutex, &args, yield_match) == 1)
	return (hal_object_ptr) args.user_ptr1;
    return HO_NULL;
}
