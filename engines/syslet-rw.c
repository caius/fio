/*
 * read/write() engine that uses syslet to be async
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "../fio.h"
#include "../os.h"

#ifdef FIO_HAVE_SYSLET

struct syslet_data {
	struct io_u **events;
	unsigned int nr_events;
	
	struct async_head_user *ahu;
	struct syslet_uatom **ring;
	unsigned int ring_index;
};

/*
 * Inspect the ring to see if we have completed events
 */
static void fio_syslet_complete(struct thread_data *td)
{
	struct syslet_data *sd = td->io_ops->data;

	do {
		struct syslet_uatom *atom;
		struct io_u *io_u;
		long ret;

		atom = sd->ring[sd->ring_index];
		if (!atom)
			break;

		sd->ring[sd->ring_index] = NULL;
		if (++sd->ring_index == td->iodepth)
			sd->ring_index = 0;

		io_u = atom->private;
		ret = *atom->ret_ptr;
		if (ret > 0)
			io_u->resid = io_u->xfer_buflen - ret;
		else if (ret < 0)
			io_u->error = ret;

		sd->events[sd->nr_events++] = io_u;
	} while (1);
}

static int fio_syslet_getevents(struct thread_data *td, int min,
				int fio_unused max,
				struct timespec fio_unused *t)
{
	struct syslet_data *sd = td->io_ops->data;
	int get_events;
	long ret;

	do {
		fio_syslet_complete(td);

		/*
		 * do we have enough immediate completions?
		 */
		if (sd->nr_events >= (unsigned int) min)
			break;

		/*
		 * OK, we need to wait for some events...
		 */
		get_events = min - sd->nr_events;
		ret = async_wait(get_events);
		if (ret < 0)
			return errno;
	} while (1);

	ret = sd->nr_events;
	sd->nr_events = 0;
	return ret;
}

static struct io_u *fio_syslet_event(struct thread_data *td, int event)
{
	struct syslet_data *sd = td->io_ops->data;

	return sd->events[event];
}

static void init_atom(struct syslet_uatom *atom, int nr, void *arg0,
		      void *arg1, void *arg2, void *arg3, void *ret_ptr,
		      unsigned long flags, void *priv)
{
	atom->flags = flags;
	atom->nr = nr;
	atom->ret_ptr = ret_ptr;
	atom->next = NULL;
	atom->arg_ptr[0] = arg0;
	atom->arg_ptr[1] = arg1;
	atom->arg_ptr[2] = arg2;
	atom->arg_ptr[3] = arg3;
	atom->arg_ptr[4] = atom->arg_ptr[5] = NULL;
	atom->private = priv;
}

/*
 * Use seek atom for sync
 */
static void fio_syslet_prep_sync(struct io_u *io_u, struct fio_file *f)
{
	init_atom(&io_u->req.atom, __NR_fsync, &f->fd, NULL, NULL, NULL,
		  &io_u->req.ret, 0, io_u);
}

static void fio_syslet_prep_rw(struct io_u *io_u, struct fio_file *f)
{
	int nr;

	/*
	 * prepare rw
	 */
	if (io_u->ddir == DDIR_READ)
		nr = __NR_pread64;
	else
		nr = __NR_pwrite64;

	init_atom(&io_u->req.atom, nr, &f->fd, &io_u->xfer_buf,
		  &io_u->xfer_buflen, &io_u->offset, &io_u->req.ret, 0, io_u);
}

static int fio_syslet_prep(struct thread_data fio_unused *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	if (io_u->ddir == DDIR_SYNC)
		fio_syslet_prep_sync(io_u, f);
	else
		fio_syslet_prep_rw(io_u, f);

	return 0;
}

static int fio_syslet_queue(struct thread_data *td, struct io_u *io_u)
{
	struct syslet_data *sd = td->io_ops->data;
	long ret;

	/*
	 * On sync completion, the atom is returned. So on NULL return
	 * it's queued asynchronously.
	 */
	if (!async_exec(&io_u->req.atom))
		return FIO_Q_QUEUED;

	/*
	 * completed sync
	 */
	ret = io_u->req.ret;
	if (ret != (long) io_u->xfer_buflen) {
		if (ret > 0) {
			io_u->resid = io_u->xfer_buflen - ret;
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else
			io_u->error = errno;
	}

	if (!io_u->error)
		sd->events[sd->nr_events++] = io_u;
	else
		td_verror(td, io_u->error);

	return FIO_Q_COMPLETED;
}

static int async_head_init(struct syslet_data *sd, unsigned int depth)
{
	unsigned long ring_size;

	sd->ahu = malloc(sizeof(struct async_head_user));
	memset(sd->ahu, 0, sizeof(struct async_head_user));

	ring_size = sizeof(struct syslet_uatom *) * depth;
	sd->ring = malloc(ring_size);
	memset(sd->ring, 0, ring_size);

	sd->ahu->completion_ring = sd->ring;
	sd->ahu->ring_size_bytes = ring_size;
	sd->ahu->max_nr_threads = -1;

	if (async_register(sd->ahu, sizeof(*sd->ahu)) < 0) {
		perror("async_register");
		fprintf(stderr, "fio: syslet likely not supported\n");
		free(sd->ring);
		free(sd->ahu);
		return 1;
	}

	return 0;
}

static void async_head_exit(struct syslet_data *sd)
{
	if (async_unregister(sd->ahu, sizeof(*sd->ahu)) < 0)
		perror("async_register");

	free(sd->ahu);
	free(sd->ring);
}

static void fio_syslet_cleanup(struct thread_data *td)
{
	struct syslet_data *sd = td->io_ops->data;

	if (sd) {
		async_head_exit(sd);
		free(sd->events);
		free(sd);
		td->io_ops->data = NULL;
	}
}

static int fio_syslet_init(struct thread_data *td)
{
	struct syslet_data *sd;


	sd = malloc(sizeof(*sd));
	memset(sd, 0, sizeof(*sd));
	sd->events = malloc(sizeof(struct io_u *) * td->iodepth);
	memset(sd->events, 0, sizeof(struct io_u *) * td->iodepth);

	/*
	 * This will handily fail for kernels where syslet isn't available
	 */
	if (async_head_init(sd, td->iodepth)) {
		free(sd->events);
		free(sd);
		return 1;
	}

	td->io_ops->data = sd;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "syslet-rw",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_syslet_init,
	.prep		= fio_syslet_prep,
	.queue		= fio_syslet_queue,
	.getevents	= fio_syslet_getevents,
	.event		= fio_syslet_event,
	.cleanup	= fio_syslet_cleanup,
};

#else /* FIO_HAVE_SYSLET */

/*
 * When we have a proper configure system in place, we simply wont build
 * and install this io engine. For now install a crippled version that
 * just complains and fails to load.
 */
static int fio_syslet_init(struct thread_data fio_unused *td)
{
	fprintf(stderr, "fio: syslet not available\n");
	return 1;
}

static struct ioengine_ops ioengine = {
	.name		= "syslet-rw",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_syslet_init,
};

#endif /* FIO_HAVE_SYSLET */

static void fio_init fio_syslet_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_syslet_unregister(void)
{
	unregister_ioengine(&ioengine);
}