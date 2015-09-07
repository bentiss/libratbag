/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <string.h>

#include "libratbag-hidraw.h"
#include "libratbag-private.h"

/* defined in include/linux.hid.h in the kernel, but not exported */
#ifndef HID_MAX_BUFFER_SIZE
#define HID_MAX_BUFFER_SIZE	4096		/* 4kb */
#endif

int
ratbag_open_hidraw(struct ratbag_device *device)
{
	struct hidraw_devinfo info;
	int fd, res;
	const char *devnode;
	pthread_mutexattr_t mutex_attr;

	if (!device->udev_hidraw)
		return -EINVAL;

	devnode = udev_device_get_devnode(device->udev_hidraw);
	fd = ratbag_open_path(device, devnode, O_RDWR);
	if (fd < 0)
		goto err;

	/* Get Raw Info */
	res = ioctl(fd, HIDIOCGRAWINFO, &info);
	if (res < 0) {
		log_error(device->ratbag,
			  "error while getting info from device");
		goto err;
	}

	device->hidraw.fd = fd;

	pthread_mutexattr_init(&mutex_attr);
	pthread_mutex_init(&device->hidraw.lock, &mutex_attr);
	pthread_mutex_init(&device->hidraw.grab_lock, &mutex_attr);
	res = pipe(device->hidraw.pipe_fds);
	if (res < 0) {
		log_error(device->ratbag,
			  "error while initializing hidraw");
		goto err_threads;
	}

	return 0;

err_threads:
	pthread_mutex_destroy(&device->hidraw.lock);
	pthread_mutex_destroy(&device->hidraw.grab_lock);
err:
	if (fd >= 0)
		ratbag_close_fd(device, fd);
	return -errno;
}

void
ratbag_close_hidraw(struct ratbag_device *device)
{
	ratbag_hidraw_stop_events(device);
	pthread_mutex_destroy(&device->hidraw.lock);
	pthread_mutex_destroy(&device->hidraw.grab_lock);

	ratbag_close_fd(device, device->hidraw.fd);
	device->hidraw.fd = -1;
}

static int
ratbag_hidraw_soft_lock_events(struct ratbag_device *device)
{
	if (!device->hidraw.use_thread)
		return 0;

	return pthread_mutex_lock(&device->hidraw.lock);
}

int
ratbag_hidraw_lock_events(struct ratbag_device *device)
{
	char buf = '\n';

	if (!device->hidraw.use_thread)
		return 0;

	/* We steal the lock of the reading thread by:
	 * - marking that we want it by grabbing grab_lock
	 * - notify the poll of the hidraw_read that we want to to interrupt
	 * - at this point, hidraw_read_input should return
	 * - the event thread tries to grab grab_lock
	 * - we grab the read lock
	 * - then release the grab_lock so the event thread can re-read next
	 *   time we released the lock
	 */
	pthread_mutex_lock(&device->hidraw.grab_lock);
	write(device->hidraw.pipe_fds[1], &buf, 1);
	pthread_mutex_lock(&device->hidraw.lock);
	pthread_mutex_unlock(&device->hidraw.grab_lock);
	return 0;
}

int
ratbag_hidraw_unlock_events(struct ratbag_device *device)
{
	if (!device->hidraw.use_thread)
		return 0;

	return pthread_mutex_unlock(&device->hidraw.lock);
}

static void *
hidraw_events_thread(void *data)
{
	struct ratbag_device *device = (struct ratbag_device *)data;
	uint8_t buf;

	while (device->hidraw.use_thread) {
		ratbag_hidraw_soft_lock_events(device);
		ratbag_hidraw_read_input_report(device, &buf, 1, 1);
		ratbag_hidraw_unlock_events(device);

		/* we make sure the caller of the interrupt has taken the
		 * read lock mutex */
		pthread_mutex_lock(&device->hidraw.grab_lock);
		pthread_mutex_unlock(&device->hidraw.grab_lock);
	}

	return NULL;
}

int
ratbag_hidraw_start_events(struct ratbag_device *device)
{
	int ret;

	/* we can start the thread only once */
	assert(device->hidraw.use_thread == 0);

	device->hidraw.use_thread = 1;

	ret = pthread_create(&device->hidraw.events_thread, NULL,
			     hidraw_events_thread, (void *)device);
	return ret;
}

void
ratbag_hidraw_stop_events(struct ratbag_device *device)
{
	if (device->hidraw.use_thread) {
		device->hidraw.use_thread = 0;
		pthread_join(device->hidraw.events_thread, NULL);
	}
}

int
ratbag_hidraw_raw_request(struct ratbag_device *device, unsigned char reportnum,
			  uint8_t *buf, size_t len, unsigned char rtype, int reqtype)
{
	char tmp_buf[HID_MAX_BUFFER_SIZE];
	int rc;

	if (len < 1 || len > HID_MAX_BUFFER_SIZE || !buf || device->hidraw.fd < 0)
		return -EINVAL;

	if (rtype != HID_FEATURE_REPORT)
		return -ENOTSUP;

	/* we need to get the lock on the file to prevent concurrent accesses */
	ratbag_hidraw_lock_events(device);

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		memset(tmp_buf, 0, len);
		tmp_buf[0] = reportnum;

		rc = ioctl(device->hidraw.fd, HIDIOCGFEATURE(len), tmp_buf);
		if (rc < 0) {
			rc = -errno;
			goto out;
		}

		memcpy(buf, tmp_buf, rc);
		goto out;
	case HID_REQ_SET_REPORT:
		buf[0] = reportnum;
		rc = ioctl(device->hidraw.fd, HIDIOCSFEATURE(len), buf);
		if (rc < 0)
			rc = -errno;

		goto out;
	}

	rc = -EINVAL;

out:
	ratbag_hidraw_unlock_events(device);
	return rc;
}

int
ratbag_hidraw_output_report(struct ratbag_device *device, uint8_t *buf, size_t len)
{
	int rc;

	if (len < 1 || len > HID_MAX_BUFFER_SIZE || !buf || device->hidraw.fd < 0)
		return -EINVAL;

	rc = write(device->hidraw.fd, buf, len);

	if (rc < 0)
		return -errno;

	if (rc != (int)len)
		return -EIO;

	return 0;
}

/* 4096 is the max allowed by the HID spec */
#define HIDRAW_MAX_READ		4096

int
ratbag_hidraw_read_input_report(struct ratbag_device *device, uint8_t *buf,
				size_t len, int propagate)
{
	int rc;
	struct pollfd fds[2] = {0};
	uint8_t read_buf[HIDRAW_MAX_READ];
	unsigned int len_read;

	if (len < 1 || !buf || device->hidraw.fd < 0 || len > HIDRAW_MAX_READ)
		return -EINVAL;

	fds[0].fd = device->hidraw.fd;
	fds[0].events = POLLIN;
	fds[1].fd = device->hidraw.pipe_fds[0];
	fds[1].events = POLLIN;

	rc = poll(fds, 2, 1000);
	if (rc == -1)
		return -errno;

	if (rc == 0)
		return -ETIMEDOUT;

	if (fds[1].revents == POLLIN) {
		/* clear the pipe */
		read(device->hidraw.pipe_fds[0], read_buf, HIDRAW_MAX_READ);
		return -EINTR;
	}

	rc = read(device->hidraw.fd, read_buf, HIDRAW_MAX_READ);
	if (rc < 0)
		return -errno;

	len_read = (unsigned)rc;

	if (propagate && device->driver->raw_event)
		device->driver->raw_event(device, read_buf, len_read);

	memcpy(buf, read_buf, len_read < len ? len_read : len);
	return rc;
}

int
ratbag_hidraw_propagate_report(struct ratbag_device *device, uint8_t *buf, size_t len)
{
	if (!device->driver->raw_event)
		return 0;

	return device->driver->raw_event(device, buf, len);
}
