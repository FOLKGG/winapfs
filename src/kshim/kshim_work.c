/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: work queues. One worker thread runs every queued and delayed work
 * item in due-time order, like a single-threaded system_wq. The driver uses
 * delayed work to commit transactions a little after the last change and
 * plain work for orphan cleanup.
 */
#include <kshim.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

struct kshim_work_state {
	struct list_head link;   /* on g_queue while pending */
	u64 due;                 /* GetTickCount64() */
	bool queued;
	bool running;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;     /* worker wakeup */
static CONDITION_VARIABLE g_done = CONDITION_VARIABLE_INIT;   /* a work finished */
static LIST_HEAD(g_queue);
static HANDLE g_thread;
static bool g_stop;
static __declspec(thread) struct work_struct *t_current;
void *system_wq = (void *)1;

static DWORD WINAPI worker(LPVOID arg)
{
	AcquireSRWLockExclusive(&g_lock);
	while (!g_stop) {
		struct kshim_work_state *s = NULL;
		struct work_struct *w = NULL;
		u64 now = GetTickCount64(), next = UINT64_MAX;
		struct list_head *pos;

		list_for_each(pos, &g_queue) {
			struct kshim_work_state *cand = container_of(pos, struct kshim_work_state, link);
			if (cand->due <= now) {
				s = cand;
				break;
			}
			if (cand->due < next)
				next = cand->due;
		}
		if (!s) {
			DWORD wait = next == UINT64_MAX ? INFINITE : (DWORD)min(next - now, 60000);
			SleepConditionVariableSRW(&g_cv, &g_lock, wait, 0);
			continue;
		}
		/* find the work_struct owning this state */
		w = *(struct work_struct **)((char *)s + sizeof(*s));
		list_del_init(&s->link);
		s->queued = false;
		s->running = true;
		w->pending = 0;
		ReleaseSRWLockExclusive(&g_lock);

		t_current = w;
		w->func(w);
		t_current = NULL;

		AcquireSRWLockExclusive(&g_lock);
		s->running = false;
		WakeAllConditionVariable(&g_done);
	}
	ReleaseSRWLockExclusive(&g_lock);
	return 0;
}

/* The state block is followed by a back pointer to its work_struct. */
static struct kshim_work_state *state_with_owner(struct work_struct *w)
{
	if (!w->kshim) {
		char *mem = calloc(1, sizeof(struct kshim_work_state) + sizeof(void *));
		struct kshim_work_state *s = (struct kshim_work_state *)mem;
		INIT_LIST_HEAD(&s->link);
		*(struct work_struct **)(mem + sizeof(*s)) = w;
		w->kshim = s;
	}
	return w->kshim;
}

void kshim_init_work(struct work_struct *w, work_func_t f)
{
	w->func = f;
	w->pending = 0;
	w->kshim = NULL;
	state_with_owner(w);
}

static void ensure_thread(void)
{
	if (!g_thread) {
		g_stop = false;
		g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	}
}

/* Queue w to run after delay ms. If already pending: keep the earlier due
 * time unless modify is set (mod_delayed_work semantics). */
static bool queue(struct work_struct *w, unsigned long delay, bool modify)
{
	struct kshim_work_state *s;
	bool was_pending;

	AcquireSRWLockExclusive(&g_lock);
	ensure_thread();
	s = state_with_owner(w);
	was_pending = s->queued;
	if (!s->queued) {
		s->queued = true;
		s->due = GetTickCount64() + delay;
		list_add_tail(&s->link, &g_queue);
		w->pending = 1;
	} else if (modify) {
		s->due = GetTickCount64() + delay;
	}
	WakeConditionVariable(&g_cv);
	ReleaseSRWLockExclusive(&g_lock);
	return !was_pending || modify;
}

bool schedule_work(struct work_struct *w) { return queue(w, 0, false); }
bool queue_work(void *wq, struct work_struct *w) { return queue(w, 0, false); }
bool schedule_delayed_work(struct delayed_work *w, unsigned long delay) { return queue(&w->work, delay, false); }
bool queue_delayed_work(void *wq, struct delayed_work *w, unsigned long delay) { return queue(&w->work, delay, false); }
bool mod_delayed_work(void *wq, struct delayed_work *w, unsigned long delay) { return queue(&w->work, delay, true); }

static bool cancel(struct work_struct *w, bool sync)
{
	struct kshim_work_state *s;
	bool was_pending;

	AcquireSRWLockExclusive(&g_lock);
	s = state_with_owner(w);
	was_pending = s->queued;
	if (s->queued) {
		list_del_init(&s->link);
		s->queued = false;
		w->pending = 0;
	}
	/* a work cancelling itself must not wait for itself */
	while (sync && s->running && t_current != w)
		SleepConditionVariableSRW(&g_done, &g_lock, INFINITE, 0);
	ReleaseSRWLockExclusive(&g_lock);
	return was_pending;
}

bool cancel_delayed_work(struct delayed_work *w) { return cancel(&w->work, false); }
bool cancel_delayed_work_sync(struct delayed_work *w) { return cancel(&w->work, true); }
bool cancel_work_sync(struct work_struct *w) { return cancel(w, true); }

/* Run a pending work now (in the caller's thread for delayed work that isn't
 * due yet is not allowed: Linux flush_delayed_work queues it immediately and
 * waits), then wait for it to finish. */
static bool flush(struct work_struct *w, bool expedite)
{
	struct kshim_work_state *s;
	bool did = false;

	AcquireSRWLockExclusive(&g_lock);
	s = state_with_owner(w);
	if (s->queued && expedite) {
		s->due = 0;
		WakeConditionVariable(&g_cv);
	}
	while ((s->queued || s->running) && t_current != w) {
		did = true;
		SleepConditionVariableSRW(&g_done, &g_lock, INFINITE, 0);
	}
	ReleaseSRWLockExclusive(&g_lock);
	return did;
}

bool flush_work(struct work_struct *w) { return flush(w, true); }
bool flush_delayed_work(struct delayed_work *w) { return flush(&w->work, true); }

struct work_struct *kshim_current_work(void)
{
	return t_current;
}

void kshim_work_shutdown(void)
{
	HANDLE t;
	AcquireSRWLockExclusive(&g_lock);
	g_stop = true;
	t = g_thread;
	g_thread = NULL;
	WakeAllConditionVariable(&g_cv);
	ReleaseSRWLockExclusive(&g_lock);
	if (t) {
		WaitForSingleObject(t, INFINITE);
		CloseHandle(t);
	}
}
