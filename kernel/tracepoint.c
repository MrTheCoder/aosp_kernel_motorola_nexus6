/*
 * Copyright (C) 2008-2014 Mathieu Desnoyers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/tracepoint.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/static_key.h>

extern struct tracepoint * const __start___tracepoints_ptrs[];
extern struct tracepoint * const __stop___tracepoints_ptrs[];

/* Set to 1 to enable tracepoint debug output */
static const int tracepoint_debug;

#ifdef CONFIG_MODULES
/*
 * Tracepoint module list mutex protects the local module list.
 */
static DEFINE_MUTEX(tracepoint_module_list_mutex);

/* Local list of struct tp_module */
static LIST_HEAD(tracepoint_module_list);
#endif /* CONFIG_MODULES */

/*
 * tracepoints_mutex protects the builtin and module tracepoints.
 * tracepoints_mutex nests inside tracepoint_module_list_mutex.
 */
static DEFINE_MUTEX(tracepoints_mutex);

/*
 * Note about RCU :
 * It is used to delay the free of multiple probes array until a quiescent
 * state is reached.
 */
struct tp_probes {
	union {
		struct rcu_head rcu;
		struct list_head list;
	} u;
	struct tracepoint_func probes[0];
};

static inline void *allocate_probes(int count)
{
	struct tp_probes *p  = kmalloc(count * sizeof(struct tracepoint_func)
			+ sizeof(struct tp_probes), GFP_KERNEL);
	return p == NULL ? NULL : p->probes;
}

static void rcu_free_old_probes(struct rcu_head *head)
{
	kfree(container_of(head, struct tp_probes, u.rcu));
}

static inline void release_probes(struct tracepoint_func *old)
{
	if (old) {
		struct tp_probes *tp_probes = container_of(old,
			struct tp_probes, probes[0]);
		call_rcu_sched(&tp_probes->u.rcu, rcu_free_old_probes);
	}
}

static void debug_print_probes(struct tracepoint_func *funcs)
{
	int i;

	if (!tracepoint_debug || !funcs)
		return;

	for (i = 0; funcs[i].func; i++)
		printk(KERN_DEBUG "Probe %d : %p\n", i, funcs[i].func);
}

static struct tracepoint_func *func_add(struct tracepoint_func **funcs,
		struct tracepoint_func *tp_func)
{
	int nr_probes = 0;
	struct tracepoint_func *old, *new;

	if (WARN_ON(!tp_func->func))
		return ERR_PTR(-EINVAL);

	debug_print_probes(*funcs);
	old = *funcs;
	if (old) {
		/* (N -> N+1), (N != 0, 1) probes */
		for (nr_probes = 0; old[nr_probes].func; nr_probes++)
			if (old[nr_probes].func == tp_func->func &&
			    old[nr_probes].data == tp_func->data)
				return ERR_PTR(-EEXIST);
	}
	/* + 2 : one for new probe, one for NULL func */
	new = allocate_probes(nr_probes + 2);
	if (new == NULL)
		return ERR_PTR(-ENOMEM);
	if (old)
		memcpy(new, old, nr_probes * sizeof(struct tracepoint_func));
	new[nr_probes] = *tp_func;
	new[nr_probes + 1].func = NULL;
	*funcs = new;
	debug_print_probes(*funcs);
	return old;
}

static void *func_remove(struct tracepoint_func **funcs,
		struct tracepoint_func *tp_func)
{
	int nr_probes = 0, nr_del = 0, i;
	struct tracepoint_func *old, *new;

	old = *funcs;

	if (!old)
		return ERR_PTR(-ENOENT);

	debug_print_probes(*funcs);
	/* (N -> M), (N > 1, M >= 0) probes */
	if (tp_func->func) {
		for (nr_probes = 0; old[nr_probes].func; nr_probes++) {
			if (old[nr_probes].func == tp_func->func &&
			     old[nr_probes].data == tp_func->data)
				nr_del++;
		}
	}

	/*
	 * If probe is NULL, then nr_probes = nr_del = 0, and then the
	 * entire entry will be removed.
	 */
	if (nr_probes - nr_del == 0) {
		/* N -> 0, (N > 1) */
		*funcs = NULL;
		debug_print_probes(*funcs);
		return old;
	} else {
		int j = 0;
		/* N -> M, (N > 1, M > 0) */
		/* + 1 for NULL */
		new = allocate_probes(nr_probes - nr_del + 1);
		if (new == NULL)
			return ERR_PTR(-ENOMEM);
		for (i = 0; old[i].func; i++)
			if (old[i].func != tp_func->func
					|| old[i].data != tp_func->data)
				new[j++] = old[i];
		new[nr_probes - nr_del].func = NULL;
		*funcs = new;
	}
	debug_print_probes(*funcs);
	return old;
}

/*
 * Add the probe function to a tracepoint.
 */
static int tracepoint_add_func(struct tracepoint *tp,
		struct tracepoint_func *func)
{
	struct tracepoint_func *old, *tp_funcs;

	if (tp->regfunc && !static_key_enabled(&tp->key))
		tp->regfunc();

	tp_funcs = tp->funcs;
	old = func_add(&tp_funcs, func);
	if (IS_ERR(old)) {
		WARN_ON_ONCE(1);
		return PTR_ERR(old);
	}
	release_probes(old);

	/*
	 * rcu_assign_pointer has a smp_wmb() which makes sure that the new
	 * probe callbacks array is consistent before setting a pointer to it.
	 * This array is referenced by __DO_TRACE from
	 * include/linux/tracepoints.h. A matching smp_read_barrier_depends()
	 * is used.
	 */
	rcu_assign_pointer(tp->funcs, tp_funcs);
	if (!static_key_enabled(&tp->key))
		static_key_slow_inc(&tp->key);
	return 0;
}

/*
 * Remove a probe function from a tracepoint.
 * Note: only waiting an RCU period after setting elem->call to the empty
 * function insures that the original callback is not used anymore. This insured
 * by preempt_disable around the call site.
 */
static int tracepoint_remove_func(struct tracepoint *tp,
		struct tracepoint_func *func)
{
	struct tracepoint_func *old, *tp_funcs;

	tp_funcs = tp->funcs;
	old = func_remove(&tp_funcs, func);
	if (IS_ERR(old)) {
		WARN_ON_ONCE(1);
		return PTR_ERR(old);
	}
	release_probes(old);

	if (!tp_funcs) {
		/* Removed last function */
		if (tp->unregfunc && static_key_enabled(&tp->key))
			tp->unregfunc();

		if (static_key_enabled(&tp->key))
			static_key_slow_dec(&tp->key);
	}
	rcu_assign_pointer(tp->funcs, tp_funcs);
	return 0;
}

/**
 * tracepoint_probe_register -  Connect a probe to a tracepoint
 * @tp: tracepoint
 * @probe: probe handler
 *
 * Returns 0 if ok, error value on error.
 * Note: if @tp is within a module, the caller is responsible for
 * unregistering the probe before the module is gone. This can be
 * performed either with a tracepoint module going notifier, or from
 * within module exit functions.
 */
int tracepoint_probe_register(struct tracepoint *tp, void *probe, void *data)
{
	struct tracepoint_func tp_func;
	int ret;

	mutex_lock(&tracepoints_mutex);
	tp_func.func = probe;
	tp_func.data = data;
	ret = tracepoint_add_func(tp, &tp_func);
	mutex_unlock(&tracepoints_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(tracepoint_probe_register);

/**
 * tracepoint_probe_unregister -  Disconnect a probe from a tracepoint
 * @tp: tracepoint
 * @probe: probe function pointer
 *
 * Returns 0 if ok, error value on error.
 */
int tracepoint_probe_unregister(struct tracepoint *tp, void *probe, void *data)
{
	struct tracepoint_func tp_func;
	int ret;

	mutex_lock(&tracepoints_mutex);
	tp_func.func = probe;
	tp_func.data = data;
	ret = tracepoint_remove_func(tp, &tp_func);
	mutex_unlock(&tracepoints_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(tracepoint_probe_unregister);

static LIST_HEAD(old_probes);

/**
 * tracepoint_get_iter_range - Get a next tracepoint iterator given a range.
 * @tracepoint: current tracepoints (in), next tracepoint (out)
 * @begin: beginning of the range
 * @end: end of the range
 *
 * Returns whether a next tracepoint has been found (1) or not (0).
 * Will return the first tracepoint in the range if the input tracepoint is
 * NULL.
 */
static int tracepoint_get_iter_range(struct tracepoint * const **tracepoint,
	struct tracepoint * const *begin, struct tracepoint * const *end)
{
	if (!*tracepoint && begin != end) {
		*tracepoint = begin;
		return 1;
	}
	if (*tracepoint >= begin && *tracepoint < end)
		return 1;
	return 0;
}

#ifdef CONFIG_MODULES
static void tracepoint_get_iter(struct tracepoint_iter *iter)
{
	int found = 0;
	struct tp_module *iter_mod;

	/* Core kernel tracepoints */
	if (!iter->module) {
		found = tracepoint_get_iter_range(&iter->tracepoint,
				__start___tracepoints_ptrs,
				__stop___tracepoints_ptrs);
		if (found)
			goto end;
	}
	/* Tracepoints in modules */
	mutex_lock(&tracepoints_mutex);
	list_for_each_entry(iter_mod, &tracepoint_module_list, list) {
		/*
		 * Sorted module list
		 */
		if (iter_mod < iter->module)
			continue;
		else if (iter_mod > iter->module)
			iter->tracepoint = NULL;
		found = tracepoint_get_iter_range(&iter->tracepoint,
			iter_mod->tracepoints_ptrs,
			iter_mod->tracepoints_ptrs
				+ iter_mod->num_tracepoints);
		if (found) {
			iter->module = iter_mod;
			break;
		}
	}
	mutex_unlock(&tracepoints_mutex);
end:
	if (!found)
		tracepoint_iter_reset(iter);
}
#else /* CONFIG_MODULES */
static void tracepoint_get_iter(struct tracepoint_iter *iter)
{
	int found = 0;

	/* Core kernel tracepoints */
	found = tracepoint_get_iter_range(&iter->tracepoint,
			__start___tracepoints_ptrs,
			__stop___tracepoints_ptrs);
	if (!found)
		tracepoint_iter_reset(iter);
}
#endif /* CONFIG_MODULES */

void tracepoint_iter_start(struct tracepoint_iter *iter)
{
	tracepoint_get_iter(iter);
}
EXPORT_SYMBOL_GPL(tracepoint_iter_start);

void tracepoint_iter_next(struct tracepoint_iter *iter)
{
	iter->tracepoint++;
	/*
	 * iter->tracepoint may be invalid because we blindly incremented it.
	 * Make sure it is valid by marshalling on the tracepoints, getting the
	 * tracepoints from following modules if necessary.
	 */
	tracepoint_get_iter(iter);
}
EXPORT_SYMBOL_GPL(tracepoint_iter_next);

void tracepoint_iter_stop(struct tracepoint_iter *iter)
{
}
EXPORT_SYMBOL_GPL(tracepoint_iter_stop);

void tracepoint_iter_reset(struct tracepoint_iter *iter)
{
#ifdef CONFIG_MODULES
	iter->module = NULL;
#endif /* CONFIG_MODULES */
	iter->tracepoint = NULL;
}
EXPORT_SYMBOL_GPL(tracepoint_iter_reset);

#ifdef CONFIG_MODULES
bool trace_module_has_bad_taint(struct module *mod)
{
	return mod->taints & ~((1 << TAINT_OOT_MODULE) | (1 << TAINT_CRAP));
}

static BLOCKING_NOTIFIER_HEAD(tracepoint_notify_list);

/**
 * register_tracepoint_notifier - register tracepoint coming/going notifier
 * @nb: notifier block
 *
 * Notifiers registered with this function are called on module
 * coming/going with the tracepoint_module_list_mutex held.
 * The notifier block callback should expect a "struct tp_module" data
 * pointer.
 */
int register_tracepoint_module_notifier(struct notifier_block *nb)
{
	struct tp_module *tp_mod;
	int ret;

	mutex_lock(&tracepoint_module_list_mutex);
	ret = blocking_notifier_chain_register(&tracepoint_notify_list, nb);
	if (ret)
		goto end;
	list_for_each_entry(tp_mod, &tracepoint_module_list, list)
		(void) nb->notifier_call(nb, MODULE_STATE_COMING, tp_mod);
end:
	mutex_unlock(&tracepoint_module_list_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(register_tracepoint_module_notifier);

/**
 * unregister_tracepoint_notifier - unregister tracepoint coming/going notifier
 * @nb: notifier block
 *
 * The notifier block callback should expect a "struct tp_module" data
 * pointer.
 */
int unregister_tracepoint_module_notifier(struct notifier_block *nb)
{
	struct tp_module *tp_mod;
	int ret;

	mutex_lock(&tracepoint_module_list_mutex);
	ret = blocking_notifier_chain_unregister(&tracepoint_notify_list, nb);
	if (ret)
		goto end;
	list_for_each_entry(tp_mod, &tracepoint_module_list, list)
		(void) nb->notifier_call(nb, MODULE_STATE_GOING, tp_mod);
end:
	mutex_unlock(&tracepoint_module_list_mutex);
	return ret;

}
EXPORT_SYMBOL_GPL(unregister_tracepoint_module_notifier);

/*
 * Ensure the tracer unregistered the module's probes before the module
 * teardown is performed. Prevents leaks of probe and data pointers.
 */
static void tp_module_going_check_quiescent(struct tracepoint * const *begin,
		struct tracepoint * const *end)
{
	struct tracepoint * const *iter;

	if (!begin)
		return;
	for (iter = begin; iter < end; iter++)
		WARN_ON_ONCE((*iter)->funcs);
}

static int tracepoint_module_coming(struct module *mod)
{
	struct tp_module *tp_mod, *iter;
	int ret = 0;

	if (!mod->num_tracepoints)
		return 0;

	/*
	 * We skip modules that taint the kernel, especially those with different
	 * module headers (for forced load), to make sure we don't cause a crash.
	 * Staging and out-of-tree GPL modules are fine.
	 */
	if (trace_module_has_bad_taint(mod))
		return 0;
	mutex_lock(&tracepoint_module_list_mutex);
	tp_mod = kmalloc(sizeof(struct tp_module), GFP_KERNEL);
	if (!tp_mod) {
		ret = -ENOMEM;
		goto end;
	}
	tp_mod->num_tracepoints = mod->num_tracepoints;
	tp_mod->tracepoints_ptrs = mod->tracepoints_ptrs;

	/*
	 * tracepoint_module_list is kept sorted by struct module pointer
	 * address for iteration on tracepoints from a seq_file that can release
	 * the mutex between calls.
	 */
	list_for_each_entry_reverse(iter, &tracepoint_module_list, list) {
		BUG_ON(iter == tp_mod);	/* Should never be in the list twice */
		if (iter < tp_mod) {
			/* We belong to the location right after iter. */
			list_add(&tp_mod->list, &iter->list);
			goto module_added;
		}
	}
	/* We belong to the beginning of the list */
	list_add(&tp_mod->list, &tracepoint_module_list);
module_added:
	blocking_notifier_call_chain(&tracepoint_notify_list,
			MODULE_STATE_COMING, tp_mod);
end:
	mutex_unlock(&tracepoint_module_list_mutex);
	return ret;
}

static void tracepoint_module_going(struct module *mod)
{
	struct tp_module *tp_mod;

	if (!mod->num_tracepoints)
		return;

	mutex_lock(&tracepoint_module_list_mutex);
	list_for_each_entry(tp_mod, &tracepoint_module_list, list) {
		if (tp_mod->tracepoints_ptrs == mod->tracepoints_ptrs) {
			blocking_notifier_call_chain(&tracepoint_notify_list,
					MODULE_STATE_GOING, tp_mod);
			list_del(&tp_mod->list);
			kfree(tp_mod);
			/*
			 * Called the going notifier before checking for
			 * quiescence.
			 */
			tp_module_going_check_quiescent(mod->tracepoints_ptrs,
				mod->tracepoints_ptrs + mod->num_tracepoints);
			break;
		}
	}
	/*
	 * In the case of modules that were tainted at "coming", we'll simply
	 * walk through the list without finding it. We cannot use the "tainted"
	 * flag on "going", in case a module taints the kernel only after being
	 * loaded.
	 */
	mutex_unlock(&tracepoint_module_list_mutex);
}

static int tracepoint_module_notify(struct notifier_block *self,
		unsigned long val, void *data)
{
	struct module *mod = data;
	int ret = 0;

	switch (val) {
	case MODULE_STATE_COMING:
		ret = tracepoint_module_coming(mod);
		break;
	case MODULE_STATE_LIVE:
		break;
	case MODULE_STATE_GOING:
		tracepoint_module_going(mod);
		break;
	case MODULE_STATE_UNFORMED:
		break;
	}
	return ret;
}

static struct notifier_block tracepoint_module_nb = {
	.notifier_call = tracepoint_module_notify,
	.priority = 0,
};

static __init int init_tracepoints(void)
{
	int ret;

	ret = register_module_notifier(&tracepoint_module_nb);
	if (ret) {
		pr_warning("Failed to register tracepoint module enter notifier\n");
	}
	return ret;
}
__initcall(init_tracepoints);
#endif /* CONFIG_MODULES */

static void for_each_tracepoint_range(struct tracepoint * const *begin,
		struct tracepoint * const *end,
		void (*fct)(struct tracepoint *tp, void *priv),
		void *priv)
{
	struct tracepoint * const *iter;

	if (!begin)
		return;
	for (iter = begin; iter < end; iter++)
		fct(*iter, priv);
}

/**
 * for_each_kernel_tracepoint - iteration on all kernel tracepoints
 * @fct: callback
 * @priv: private data
 */
void for_each_kernel_tracepoint(void (*fct)(struct tracepoint *tp, void *priv),
		void *priv)
{
	for_each_tracepoint_range(__start___tracepoints_ptrs,
		__stop___tracepoints_ptrs, fct, priv);
}
EXPORT_SYMBOL_GPL(for_each_kernel_tracepoint);

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS

/* NB: reg/unreg are called while guarded with the tracepoints_mutex */
static int sys_tracepoint_refcount;

void syscall_regfunc(void)
{
	struct task_struct *p, *t;

	if (!sys_tracepoint_refcount) {
		read_lock(&tasklist_lock);
		for_each_process_thread(p, t) {
			set_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);
		}
		read_unlock(&tasklist_lock);
	}
	sys_tracepoint_refcount++;
}

void syscall_unregfunc(void)
{
	struct task_struct *p, *t;

	sys_tracepoint_refcount--;
	if (!sys_tracepoint_refcount) {
		read_lock(&tasklist_lock);
		for_each_process_thread(p, t) {
			clear_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);
		}
		read_unlock(&tasklist_lock);
	}
}
#endif
