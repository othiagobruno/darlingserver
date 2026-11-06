#include <darlingserver/duct-tape.h>
#include <darlingserver/duct-tape/stubs.h>

#include <mach/mach_types.h>
#include <mach/message.h>
#include <kern/kern_types.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_right.h>
#include <kern/ipc_kobject.h>
#include <kern/ipc_misc.h>

#include <stdlib.h>

// XNU and Linux libc headers can't be combined in this unit
int close(int fd);
int dup(int fd);

// Fileports normally wrap a kernel `struct fileglob`; here the "fileglob" is just a descriptor owned by darlingserver
// (a dup of the client's descriptor) that is closed when the port loses its last send right.
struct dtape_fileport {
	int fd;
};

void fileport_releasefg(struct fileglob* fg) {
	struct dtape_fileport* fileport = (struct dtape_fileport*)fg;
	close(fileport->fd);
	free(fileport);
};

uint32_t dtape_fileport_makeport(int fd) {
	struct dtape_fileport* fileport = malloc(sizeof(*fileport));
	ipc_port_t port;
	mach_port_name_t name;

	if (!fileport) {
		return MACH_PORT_NULL;
	}

	fileport->fd = fd;

	port = fileport_alloc((struct fileglob*)fileport);
	if (!IP_VALID(port)) {
		free(fileport);
		return MACH_PORT_NULL;
	}

	// consumes the send right
	name = ipc_port_copyout_send(port, current_space());
	if (!MACH_PORT_VALID(name)) {
		return MACH_PORT_NULL;
	}

	// ownership of the descriptor moved to the fileport
	return name;
};

int dtape_fileport_makefd(uint32_t port_name) {
	ipc_port_t port = IP_NULL;
	struct dtape_fileport* fileport;
	int fd = -1;

	if (ipc_port_translate_send(current_space(), port_name, &port) != KERN_SUCCESS) {
		return -1;
	}

	// the port is locked on success
	if (ip_active(port) && ip_kotype(port) == IKOT_FILEPORT) {
		fileport = (struct dtape_fileport*)ip_get_kobject(port);
		if (fileport) {
			fd = dup(fileport->fd);
		}
	}
	ip_unlock(port);

	return fd;
};
