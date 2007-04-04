/*
 * Copyright (c) 2005, Junio C Hamano
 */
#include "cache.h"

static struct lock_file *lock_file_list;
static const char *alternate_index_output;

static void remove_lock_file(void)
{
	while (lock_file_list) {
		if (lock_file_list->filename[0])
			unlink(lock_file_list->filename);
		lock_file_list = lock_file_list->next;
	}
}

static void remove_lock_file_on_signal(int signo)
{
	remove_lock_file();
	signal(SIGINT, SIG_DFL);
	raise(signo);
}

static int lock_file(struct lock_file *lk, const char *path)
{
	int fd;
	sprintf(lk->filename, "%s.lock", path);
	fd = open(lk->filename, O_RDWR | O_CREAT | O_EXCL, 0666);
	if (0 <= fd) {
		if (!lk->on_list) {
			lk->next = lock_file_list;
			lock_file_list = lk;
			lk->on_list = 1;
		}
		if (lock_file_list) {
			signal(SIGINT, remove_lock_file_on_signal);
			atexit(remove_lock_file);
		}
		if (adjust_shared_perm(lk->filename))
			return error("cannot fix permission bits on %s",
				     lk->filename);
	}
	else
		lk->filename[0] = 0;
	return fd;
}

int hold_lock_file_for_update(struct lock_file *lk, const char *path, int die_on_error)
{
	int fd = lock_file(lk, path);
	if (fd < 0 && die_on_error)
		die("unable to create '%s.lock': %s", path, strerror(errno));
	return fd;
}

int commit_lock_file(struct lock_file *lk)
{
	char result_file[PATH_MAX];
	int i;
	strcpy(result_file, lk->filename);
	i = strlen(result_file) - 5; /* .lock */
	result_file[i] = 0;
	i = rename(lk->filename, result_file);
	lk->filename[0] = 0;
	return i;
}

int hold_locked_index(struct lock_file *lk, int die_on_error)
{
	return hold_lock_file_for_update(lk, get_index_file(), die_on_error);
}

void set_alternate_index_output(const char *name)
{
	alternate_index_output = name;
}

int commit_locked_index(struct lock_file *lk)
{
	if (alternate_index_output) {
		int result = rename(lk->filename, alternate_index_output);
		lk->filename[0] = 0;
		return result;
	}
	else
		return commit_lock_file(lk);
}

void rollback_lock_file(struct lock_file *lk)
{
	if (lk->filename[0])
		unlink(lk->filename);
	lk->filename[0] = 0;
}

