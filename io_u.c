#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <assert.h>

#include "fio.h"

/*
 * Change this define to play with the timeout handling
 */
#undef FIO_USE_TIMEOUT

struct io_completion_data {
	int nr;				/* input */

	int error;			/* output */
	unsigned long bytes_done[2];	/* output */
	struct timeval time;		/* output */
};

/*
 * The ->file_map[] contains a map of blocks we have or have not done io
 * to yet. Used to make sure we cover the entire range in a fair fashion.
 */
static int random_map_free(struct thread_data *td, struct fio_file *f,
			   const unsigned long long block)
{
	unsigned int idx = RAND_MAP_IDX(td, f, block);
	unsigned int bit = RAND_MAP_BIT(td, f, block);

	dprint(FD_RANDOM, "free: b=%llu, idx=%u, bit=%u\n", block, idx, bit);

	return (f->file_map[idx] & (1UL << bit)) == 0;
}

/*
 * Mark a given offset as used in the map.
 */
static void mark_random_map(struct thread_data *td, struct io_u *io_u)
{
	unsigned int min_bs = td->o.rw_min_bs;
	struct fio_file *f = io_u->file;
	unsigned long long block;
	unsigned int blocks;
	unsigned int nr_blocks;

	block = (io_u->offset - f->file_offset) / (unsigned long long) min_bs;
	blocks = 0;
	nr_blocks = (io_u->buflen + min_bs - 1) / min_bs;

	while (blocks < nr_blocks) {
		unsigned int idx, bit;

		/*
		 * If we have a mixed random workload, we may
		 * encounter blocks we already did IO to.
		 */
		if ((td->o.ddir_nr == 1) && !random_map_free(td, f, block))
			break;

		idx = RAND_MAP_IDX(td, f, block);
		bit = RAND_MAP_BIT(td, f, block);

		fio_assert(td, idx < f->num_maps);

		f->file_map[idx] |= (1UL << bit);
		block++;
		blocks++;
	}

	if ((blocks * min_bs) < io_u->buflen)
		io_u->buflen = blocks * min_bs;
}

static inline unsigned long long last_block(struct thread_data *td,
					    struct fio_file *f,
					    enum fio_ddir ddir)
{
	unsigned long long max_blocks;

	max_blocks = f->io_size / (unsigned long long) td->o.min_bs[ddir];
	if (!max_blocks)
		return 0;

	return max_blocks - 1;
}

/*
 * Return the next free block in the map.
 */
static int get_next_free_block(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, unsigned long long *b)
{
	unsigned long long min_bs = td->o.rw_min_bs;
	int i;

	i = f->last_free_lookup;
	*b = (i * BLOCKS_PER_MAP);
	while ((*b) * min_bs < f->real_file_size) {
		if (f->file_map[i] != -1UL) {
			*b += fio_ffz(f->file_map[i]);
			if (*b > last_block(td, f, ddir))
				break;
			f->last_free_lookup = i;
			return 0;
		}

		*b += BLOCKS_PER_MAP;
		i++;
	}

	dprint(FD_IO, "failed finding a free block\n");
	return 1;
}

static int get_next_rand_offset(struct thread_data *td, struct fio_file *f,
				enum fio_ddir ddir, unsigned long long *b)
{
	unsigned long long r;
	int loops = 5;

	do {
		r = os_random_long(&td->random_state);
		dprint(FD_RANDOM, "off rand %llu\n", r);
		*b = (last_block(td, f, ddir) - 1)
			* (r / ((unsigned long long) RAND_MAX + 1.0));

		/*
		 * if we are not maintaining a random map, we are done.
		 */
		if (td->o.norandommap)
			return 0;

		/*
		 * calculate map offset and check if it's free
		 */
		if (random_map_free(td, f, *b))
			return 0;

		dprint(FD_RANDOM, "get_next_rand_offset: offset %llu busy\n",
									*b);
	} while (--loops);

	/*
	 * we get here, if we didn't suceed in looking up a block. generate
	 * a random start offset into the filemap, and find the first free
	 * block from there.
	 */
	loops = 10;
	do {
		f->last_free_lookup = (f->num_maps - 1) * (r / (RAND_MAX+1.0));
		if (!get_next_free_block(td, f, ddir, b))
			return 0;

		r = os_random_long(&td->random_state);
	} while (--loops);

	/*
	 * that didn't work either, try exhaustive search from the start
	 */
	f->last_free_lookup = 0;
	return get_next_free_block(td, f, ddir, b);
}

/*
 * For random io, generate a random new block and see if it's used. Repeat
 * until we find a free one. For sequential io, just return the end of
 * the last io issued.
 */
static int get_next_offset(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	unsigned long long b;
	enum fio_ddir ddir = io_u->ddir;

	if (td_random(td) && (td->o.ddir_nr && !--td->ddir_nr)) {
		td->ddir_nr = td->o.ddir_nr;

		if (get_next_rand_offset(td, f, ddir, &b))
			return 1;
	} else {
		if (f->last_pos >= f->real_file_size) {
			if (!td_random(td) ||
			     get_next_rand_offset(td, f, ddir, &b))
				return 1;
		} else
			b = (f->last_pos - f->file_offset) / td->o.min_bs[ddir];
	}

	io_u->offset = (b * td->o.min_bs[ddir]) + f->file_offset;
	if (io_u->offset >= f->real_file_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= size %llu\n",
					io_u->offset, f->real_file_size);
		return 1;
	}

	return 0;
}

static unsigned int get_next_buflen(struct thread_data *td, struct io_u *io_u)
{
	const int ddir = io_u->ddir;
	unsigned int buflen;
	long r;

	if (td->o.min_bs[ddir] == td->o.max_bs[ddir])
		buflen = td->o.min_bs[ddir];
	else {
		r = os_random_long(&td->bsrange_state);
		if (!td->o.bssplit_nr) {
			buflen = (unsigned int)
					(1 + (double) (td->o.max_bs[ddir] - 1)
					* r / (RAND_MAX + 1.0));
		} else {
			long perc = 0;
			unsigned int i;

			for (i = 0; i < td->o.bssplit_nr; i++) {
				struct bssplit *bsp = &td->o.bssplit[i];

				buflen = bsp->bs;
				perc += bsp->perc;
				if (r <= ((LONG_MAX / 100L) * perc))
					break;
			}
		}
		if (!td->o.bs_unaligned) {
			buflen = (buflen + td->o.min_bs[ddir] - 1)
					& ~(td->o.min_bs[ddir] - 1);
		}
	}

	if (io_u->offset + buflen > io_u->file->real_file_size) {
		dprint(FD_IO, "lower buflen %u -> %u (ddir=%d)\n", buflen,
						td->o.min_bs[ddir], ddir);
		buflen = td->o.min_bs[ddir];
	}

	return buflen;
}

static void set_rwmix_bytes(struct thread_data *td)
{
	unsigned long long rbytes;
	unsigned int diff;

	/*
	 * we do time or byte based switch. this is needed because
	 * buffered writes may issue a lot quicker than they complete,
	 * whereas reads do not.
	 */
	rbytes = td->io_bytes[td->rwmix_ddir] - td->rwmix_bytes;
	diff = td->o.rwmix[td->rwmix_ddir ^ 1];

	td->rwmix_bytes = td->io_bytes[td->rwmix_ddir]
				+ (rbytes * ((100 - diff)) / diff);
}

static inline enum fio_ddir get_rand_ddir(struct thread_data *td)
{
	unsigned int v;
	long r;

	r = os_random_long(&td->rwmix_state);
	v = 1 + (int) (100.0 * (r / (RAND_MAX + 1.0)));
	if (v < td->o.rwmix[DDIR_READ])
		return DDIR_READ;

	return DDIR_WRITE;
}

/*
 * Return the data direction for the next io_u. If the job is a
 * mixed read/write workload, check the rwmix cycle and switch if
 * necessary.
 */
static enum fio_ddir get_rw_ddir(struct thread_data *td)
{
	if (td_rw(td)) {
		struct timeval now;
		unsigned long elapsed;
		unsigned int cycle;

		fio_gettime(&now, NULL);
		elapsed = mtime_since_now(&td->rwmix_switch);

		/*
		 * if this is the first cycle, make it shorter
		 */
		cycle = td->o.rwmixcycle;
		if (!td->rwmix_bytes)
			cycle /= 10;

		/*
		 * Check if it's time to seed a new data direction.
		 */
		if (elapsed >= cycle ||
		    td->io_bytes[td->rwmix_ddir] >= td->rwmix_bytes) {
			unsigned long long max_bytes;
			enum fio_ddir ddir;

			/*
			 * Put a top limit on how many bytes we do for
			 * one data direction, to avoid overflowing the
			 * ranges too much
			 */
			ddir = get_rand_ddir(td);
			max_bytes = td->this_io_bytes[ddir];
			if (max_bytes >=
			    (td->o.size * td->o.rwmix[ddir] / 100)) {
				if (!td->rw_end_set[ddir]) {
					td->rw_end_set[ddir] = 1;
					memcpy(&td->rw_end[ddir], &now,
						sizeof(now));
				}
				ddir ^= 1;
			}

			if (ddir != td->rwmix_ddir)
				set_rwmix_bytes(td);

			td->rwmix_ddir = ddir;
			memcpy(&td->rwmix_switch, &now, sizeof(now));
		}
		return td->rwmix_ddir;
	} else if (td_read(td))
		return DDIR_READ;
	else
		return DDIR_WRITE;
}

void put_io_u(struct thread_data *td, struct io_u *io_u)
{
	assert((io_u->flags & IO_U_F_FREE) == 0);
	io_u->flags |= IO_U_F_FREE;

	if (io_u->file) {
		int ret = put_file(td, io_u->file);

		if (ret)
			td_verror(td, ret, "file close");
	}

	io_u->file = NULL;
	list_del(&io_u->list);
	list_add(&io_u->list, &td->io_u_freelist);
	td->cur_depth--;
}

void requeue_io_u(struct thread_data *td, struct io_u **io_u)
{
	struct io_u *__io_u = *io_u;

	__io_u->flags |= IO_U_F_FREE;
	if ((__io_u->flags & IO_U_F_FLIGHT) && (__io_u->ddir != DDIR_SYNC))
		td->io_issues[__io_u->ddir]--;

	__io_u->flags &= ~IO_U_F_FLIGHT;

	list_del(&__io_u->list);
	list_add_tail(&__io_u->list, &td->io_u_requeues);
	td->cur_depth--;
	*io_u = NULL;
}

static int fill_io_u(struct thread_data *td, struct io_u *io_u)
{
	if (td->io_ops->flags & FIO_NOIO)
		goto out;

	/*
	 * see if it's time to sync
	 */
	if (td->o.fsync_blocks &&
	   !(td->io_issues[DDIR_WRITE] % td->o.fsync_blocks) &&
	     td->io_issues[DDIR_WRITE] && should_fsync(td)) {
		io_u->ddir = DDIR_SYNC;
		goto out;
	}

	io_u->ddir = get_rw_ddir(td);

	/*
	 * See if it's time to switch to a new zone
	 */
	if (td->zone_bytes >= td->o.zone_size) {
		td->zone_bytes = 0;
		io_u->file->last_pos += td->o.zone_skip;
		td->io_skip_bytes += td->o.zone_skip;
	}

	/*
	 * No log, let the seq/rand engine retrieve the next buflen and
	 * position.
	 */
	if (get_next_offset(td, io_u)) {
		dprint(FD_IO, "io_u %p, failed getting offset\n", io_u);
		return 1;
	}

	io_u->buflen = get_next_buflen(td, io_u);
	if (!io_u->buflen) {
		dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
		return 1;
	}

	if (io_u->offset + io_u->buflen > io_u->file->real_file_size) {
		dprint(FD_IO, "io_u %p, offset too large\n", io_u);
		dprint(FD_IO, "  off=%llu/%lu > %llu\n", io_u->offset,
				io_u->buflen, io_u->file->real_file_size);
		return 1;
	}

	/*
	 * mark entry before potentially trimming io_u
	 */
	if (td_random(td) && !td->o.norandommap)
		mark_random_map(td, io_u);

	/*
	 * If using a write iolog, store this entry.
	 */
out:
	dprint_io_u(io_u, "fill_io_u");
	td->zone_bytes += io_u->buflen;
	log_io_u(td, io_u);
	return 0;
}

void io_u_mark_depth(struct thread_data *td, struct io_u *io_u)
{
	int index = 0;

	if (io_u->ddir == DDIR_SYNC)
		return;

	switch (td->cur_depth) {
	default:
		index = 6;
		break;
	case 32 ... 63:
		index = 5;
		break;
	case 16 ... 31:
		index = 4;
		break;
	case 8 ... 15:
		index = 3;
		break;
	case 4 ... 7:
		index = 2;
		break;
	case 2 ... 3:
		index = 1;
	case 1:
		break;
	}

	td->ts.io_u_map[index]++;
	td->ts.total_io_u[io_u->ddir]++;
}

static void io_u_mark_lat_usec(struct thread_data *td, unsigned long usec)
{
	int index = 0;

	assert(usec < 1000);

	switch (usec) {
	case 750 ... 999:
		index = 9;
		break;
	case 500 ... 749:
		index = 8;
		break;
	case 250 ... 499:
		index = 7;
		break;
	case 100 ... 249:
		index = 6;
		break;
	case 50 ... 99:
		index = 5;
		break;
	case 20 ... 49:
		index = 4;
		break;
	case 10 ... 19:
		index = 3;
		break;
	case 4 ... 9:
		index = 2;
		break;
	case 2 ... 3:
		index = 1;
	case 0 ... 1:
		break;
	}

	assert(index < FIO_IO_U_LAT_U_NR);
	td->ts.io_u_lat_u[index]++;
}

static void io_u_mark_lat_msec(struct thread_data *td, unsigned long msec)
{
	int index = 0;

	switch (msec) {
	default:
		index = 11;
		break;
	case 1000 ... 1999:
		index = 10;
		break;
	case 750 ... 999:
		index = 9;
		break;
	case 500 ... 749:
		index = 8;
		break;
	case 250 ... 499:
		index = 7;
		break;
	case 100 ... 249:
		index = 6;
		break;
	case 50 ... 99:
		index = 5;
		break;
	case 20 ... 49:
		index = 4;
		break;
	case 10 ... 19:
		index = 3;
		break;
	case 4 ... 9:
		index = 2;
		break;
	case 2 ... 3:
		index = 1;
	case 0 ... 1:
		break;
	}

	assert(index < FIO_IO_U_LAT_M_NR);
	td->ts.io_u_lat_m[index]++;
}

static void io_u_mark_latency(struct thread_data *td, unsigned long usec)
{
	if (usec < 1000)
		io_u_mark_lat_usec(td, usec);
	else
		io_u_mark_lat_msec(td, usec / 1000);
}

/*
 * Get next file to service by choosing one at random
 */
static struct fio_file *get_next_file_rand(struct thread_data *td, int goodf,
					   int badf)
{
	struct fio_file *f;
	int fno;

	do {
		long r = os_random_long(&td->next_file_state);

		fno = (unsigned int) ((double) td->o.nr_files
			* (r / (RAND_MAX + 1.0)));
		f = td->files[fno];
		if (f->flags & FIO_FILE_DONE)
			continue;

		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf)) {
			dprint(FD_FILE, "get_next_file_rand: %p\n", f);
			return f;
		}
	} while (1);
}

/*
 * Get next file to service by doing round robin between all available ones
 */
static struct fio_file *get_next_file_rr(struct thread_data *td, int goodf,
					 int badf)
{
	unsigned int old_next_file = td->next_file;
	struct fio_file *f;

	do {
		f = td->files[td->next_file];

		td->next_file++;
		if (td->next_file >= td->o.nr_files)
			td->next_file = 0;

		if (f->flags & FIO_FILE_DONE) {
			f = NULL;
			continue;
		}

		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf))
			break;

		f = NULL;
	} while (td->next_file != old_next_file);

	dprint(FD_FILE, "get_next_file_rr: %p\n", f);
	return f;
}

static struct fio_file *get_next_file(struct thread_data *td)
{
	struct fio_file *f;

	assert(td->o.nr_files <= td->files_index);

	if (!td->nr_open_files || td->nr_done_files >= td->o.nr_files) {
		dprint(FD_FILE, "get_next_file: nr_open=%d, nr_done=%d,"
				" nr_files=%d\n", td->nr_open_files,
						  td->nr_done_files,
						  td->o.nr_files);
		return NULL;
	}

	f = td->file_service_file;
	if (f && (f->flags & FIO_FILE_OPEN) && td->file_service_left--)
		goto out;

	if (td->o.file_service_type == FIO_FSERVICE_RR)
		f = get_next_file_rr(td, FIO_FILE_OPEN, FIO_FILE_CLOSING);
	else
		f = get_next_file_rand(td, FIO_FILE_OPEN, FIO_FILE_CLOSING);

	td->file_service_file = f;
	td->file_service_left = td->file_service_nr - 1;
out:
	dprint(FD_FILE, "get_next_file: %p\n", f);
	return f;
}

static struct fio_file *find_next_new_file(struct thread_data *td)
{
	struct fio_file *f;

	if (!td->nr_open_files || td->nr_done_files >= td->o.nr_files)
		return NULL;

	if (td->o.file_service_type == FIO_FSERVICE_RR)
		f = get_next_file_rr(td, 0, FIO_FILE_OPEN);
	else
		f = get_next_file_rand(td, 0, FIO_FILE_OPEN);

	return f;
}

static int set_io_u_file(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f;

	do {
		f = get_next_file(td);
		if (!f)
			return 1;

set_file:
		io_u->file = f;
		get_file(f);

		if (!fill_io_u(td, io_u))
			break;

		/*
		 * td_io_close() does a put_file() as well, so no need to
		 * do that here.
		 */
		io_u->file = NULL;
		td_io_close_file(td, f);
		f->flags |= FIO_FILE_DONE;
		td->nr_done_files++;

		/*
		 * probably not the right place to do this, but see
		 * if we need to open a new file
		 */
		if (td->nr_open_files < td->o.open_files &&
		    td->o.open_files != td->o.nr_files) {
			f = find_next_new_file(td);

			if (!f || td_io_open_file(td, f))
				return 1;

			goto set_file;
		}
	} while (1);

	return 0;
}


struct io_u *__get_io_u(struct thread_data *td)
{
	struct io_u *io_u = NULL;

	if (!list_empty(&td->io_u_requeues))
		io_u = list_entry(td->io_u_requeues.next, struct io_u, list);
	else if (!queue_full(td)) {
		io_u = list_entry(td->io_u_freelist.next, struct io_u, list);

		io_u->buflen = 0;
		io_u->resid = 0;
		io_u->file = NULL;
		io_u->end_io = NULL;
	}

	if (io_u) {
		assert(io_u->flags & IO_U_F_FREE);
		io_u->flags &= ~IO_U_F_FREE;

		io_u->error = 0;
		list_del(&io_u->list);
		list_add(&io_u->list, &td->io_u_busylist);
		td->cur_depth++;
	}

	return io_u;
}

/*
 * Return an io_u to be processed. Gets a buflen and offset, sets direction,
 * etc. The returned io_u is fully ready to be prepped and submitted.
 */
struct io_u *get_io_u(struct thread_data *td)
{
	struct fio_file *f;
	struct io_u *io_u;

	io_u = __get_io_u(td);
	if (!io_u) {
		dprint(FD_IO, "__get_io_u failed\n");
		return NULL;
	}

	/*
	 * from a requeue, io_u already setup
	 */
	if (io_u->file)
		goto out;

	/*
	 * If using an iolog, grab next piece if any available.
	 */
	if (td->o.read_iolog_file) {
		if (read_iolog_get(td, io_u))
			goto err_put;
	} else if (set_io_u_file(td, io_u)) {
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	f = io_u->file;
	assert(f->flags & FIO_FILE_OPEN);

	if (io_u->ddir != DDIR_SYNC) {
		if (!io_u->buflen && !(td->io_ops->flags & FIO_NOIO)) {
			dprint(FD_IO, "get_io_u: zero buflen on %p\n", io_u);
			goto err_put;
		}

		f->last_pos = io_u->offset + io_u->buflen;

		if (td->o.verify != VERIFY_NONE)
			populate_verify_io_u(td, io_u);
	}

	/*
	 * Set io data pointers.
	 */
	io_u->endpos = io_u->offset + io_u->buflen;
	io_u->xfer_buf = io_u->buf;
	io_u->xfer_buflen = io_u->buflen;
out:
	if (!td_io_prep(td, io_u)) {
		fio_gettime(&io_u->start_time, NULL);
		return io_u;
	}
err_put:
	dprint(FD_IO, "get_io_u failed\n");
	put_io_u(td, io_u);
	return NULL;
}

void io_u_log_error(struct thread_data *td, struct io_u *io_u)
{
	const char *msg[] = { "read", "write", "sync" };

	log_err("fio: io_u error");

	if (io_u->file)
		log_err(" on file %s", io_u->file->file_name);

	log_err(": %s\n", strerror(io_u->error));

	log_err("     %s offset=%llu, buflen=%lu\n", msg[io_u->ddir],
					io_u->offset, io_u->xfer_buflen);

	if (!td->error)
		td_verror(td, io_u->error, "io_u error");
}

static void io_completed(struct thread_data *td, struct io_u *io_u,
			 struct io_completion_data *icd)
{
	unsigned long usec;

	dprint_io_u(io_u, "io complete");

	assert(io_u->flags & IO_U_F_FLIGHT);
	io_u->flags &= ~IO_U_F_FLIGHT;

	if (io_u->ddir == DDIR_SYNC) {
		td->last_was_sync = 1;
		return;
	}

	td->last_was_sync = 0;

	if (!io_u->error) {
		unsigned int bytes = io_u->buflen - io_u->resid;
		const enum fio_ddir idx = io_u->ddir;
		int ret;

		td->io_blocks[idx]++;
		td->io_bytes[idx] += bytes;
		td->this_io_bytes[idx] += bytes;

		usec = utime_since(&io_u->issue_time, &icd->time);

		add_clat_sample(td, idx, usec);
		add_bw_sample(td, idx, &icd->time);
		io_u_mark_latency(td, usec);

		if (td_write(td) && idx == DDIR_WRITE &&
		    td->o.do_verify &&
		    td->o.verify != VERIFY_NONE)
			log_io_piece(td, io_u);

		icd->bytes_done[idx] += bytes;

		if (io_u->end_io) {
			ret = io_u->end_io(td, io_u);
			if (ret && !icd->error)
				icd->error = ret;
		}
	} else {
		icd->error = io_u->error;
		io_u_log_error(td, io_u);
	}
}

static void init_icd(struct io_completion_data *icd, int nr)
{
	fio_gettime(&icd->time, NULL);

	icd->nr = nr;

	icd->error = 0;
	icd->bytes_done[0] = icd->bytes_done[1] = 0;
}

static void ios_completed(struct thread_data *td,
			  struct io_completion_data *icd)
{
	struct io_u *io_u;
	int i;

	for (i = 0; i < icd->nr; i++) {
		io_u = td->io_ops->event(td, i);

		io_completed(td, io_u, icd);
		put_io_u(td, io_u);
	}
}

/*
 * Complete a single io_u for the sync engines.
 */
long io_u_sync_complete(struct thread_data *td, struct io_u *io_u)
{
	struct io_completion_data icd;

	init_icd(&icd, 1);
	io_completed(td, io_u, &icd);
	put_io_u(td, io_u);

	if (!icd.error)
		return icd.bytes_done[0] + icd.bytes_done[1];

	td_verror(td, icd.error, "io_u_sync_complete");
	return -1;
}

/*
 * Called to complete min_events number of io for the async engines.
 */
long io_u_queued_complete(struct thread_data *td, int min_events)
{
	struct io_completion_data icd;
	struct timespec *tvp = NULL;
	int ret;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 0, };

	dprint(FD_IO, "io_u_queued_completed: min=%d\n", min_events);

	if (!min_events)
		tvp = &ts;

	ret = td_io_getevents(td, min_events, td->cur_depth, tvp);
	if (ret < 0) {
		td_verror(td, -ret, "td_io_getevents");
		return ret;
	} else if (!ret)
		return ret;

	init_icd(&icd, ret);
	ios_completed(td, &icd);
	if (!icd.error)
		return icd.bytes_done[0] + icd.bytes_done[1];

	td_verror(td, icd.error, "io_u_queued_complete");
	return -1;
}

/*
 * Call when io_u is really queued, to update the submission latency.
 */
void io_u_queued(struct thread_data *td, struct io_u *io_u)
{
	unsigned long slat_time;

	slat_time = utime_since(&io_u->start_time, &io_u->issue_time);
	add_slat_sample(td, io_u->ddir, slat_time);
}

#ifdef FIO_USE_TIMEOUT
void io_u_set_timeout(struct thread_data *td)
{
	assert(td->cur_depth);

	td->timer.it_interval.tv_sec = 0;
	td->timer.it_interval.tv_usec = 0;
	td->timer.it_value.tv_sec = IO_U_TIMEOUT + IO_U_TIMEOUT_INC;
	td->timer.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &td->timer, NULL);
	fio_gettime(&td->timeout_end, NULL);
}

static void io_u_dump(struct io_u *io_u)
{
	unsigned long t_start = mtime_since_now(&io_u->start_time);
	unsigned long t_issue = mtime_since_now(&io_u->issue_time);

	log_err("io_u=%p, t_start=%lu, t_issue=%lu\n", io_u, t_start, t_issue);
	log_err("  buf=%p/%p, len=%lu/%lu, offset=%llu\n", io_u->buf,
						io_u->xfer_buf, io_u->buflen,
						io_u->xfer_buflen,
						io_u->offset);
	log_err("  ddir=%d, fname=%s\n", io_u->ddir, io_u->file->file_name);
}
#else
void io_u_set_timeout(struct thread_data fio_unused *td)
{
}
#endif

#ifdef FIO_USE_TIMEOUT
static void io_u_timeout_handler(int fio_unused sig)
{
	struct thread_data *td, *__td;
	pid_t pid = getpid();
	struct list_head *entry;
	struct io_u *io_u;
	int i;

	log_err("fio: io_u timeout\n");

	/*
	 * TLS would be nice...
	 */
	td = NULL;
	for_each_td(__td, i) {
		if (__td->pid == pid) {
			td = __td;
			break;
		}
	}

	if (!td) {
		log_err("fio: io_u timeout, can't find job\n");
		exit(1);
	}

	if (!td->cur_depth) {
		log_err("fio: timeout without pending work?\n");
		return;
	}

	log_err("fio: io_u timeout: job=%s, pid=%d\n", td->o.name, td->pid);

	list_for_each(entry, &td->io_u_busylist) {
		io_u = list_entry(entry, struct io_u, list);

		io_u_dump(io_u);
	}

	td_verror(td, ETIMEDOUT, "io_u timeout");
	exit(1);
}
#endif

void io_u_init_timeout(void)
{
#ifdef FIO_USE_TIMEOUT
	signal(SIGALRM, io_u_timeout_handler);
#endif
}
