/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) Red Hat, Inc., 2009, 2010, 2011
 * Copyright (C) Amit Shah <amit.shah@redhat.com>, 2009, 2010, 2011
 *
 * From Linux kernel include/uapi/linux/virtio_console.h
 */
#ifndef _LINUX_VIRTIO_CONSOLE_H
#define _LINUX_VIRTIO_CONSOLE_H

/* Feature bits */
#define VIRTIO_CONSOLE_F_SIZE	0	/* Does host provide console size? */
#define VIRTIO_CONSOLE_F_MULTIPORT 1	/* Does host provide multiple ports? */
#define VIRTIO_CONSOLE_F_EMERG_WRITE 2 /* Does host support emergency write? */

#define VIRTIO_CONSOLE_BAD_ID		(~(__u32)0)

struct virtio_console_config {
	/* colums of the screens */
	__u16 cols;
	/* rows of the screens */
	__u16 rows;
	/* max. number of ports this device can hold */
	__u32 max_nr_ports;
	/* emergency write register */
	__u32 emerg_wr;
} __attribute__((packed));

/*
 * A message that's passed between the Host and the Guest for a
 * particular port.
 */
struct virtio_console_control {
	__u32 id;		/* Port number */
	__u16 event;	/* The kind of control event (see below) */
	__u16 value;	/* Extra information for the key */
};

/* Some events for control messages */
#define VIRTIO_CONSOLE_DEVICE_READY	0
#define VIRTIO_CONSOLE_PORT_ADD		1
#define VIRTIO_CONSOLE_PORT_REMOVE	2
#define VIRTIO_CONSOLE_PORT_READY	3
#define VIRTIO_CONSOLE_CONSOLE_PORT	4
#define VIRTIO_CONSOLE_RESIZE		5
#define VIRTIO_CONSOLE_PORT_OPEN	6
#define VIRTIO_CONSOLE_PORT_NAME	7


#endif
