/*
 * Copyright (C) 2019-2020 Paul Kocialkowski <contact@paulk.fr>
 * Copyright (C) 2020 Bootlin
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <linux/media.h>

int media_device_info(int media_fd, struct media_device_info *device_info)
{
	int ret;

	ret = ioctl(media_fd, MEDIA_IOC_DEVICE_INFO, device_info);
	if (ret)
		return -errno;

	return 0;
}

int media_topology_get(int media_fd, struct media_v2_topology *topology)
{
	int ret;

	ret = ioctl(media_fd, MEDIA_IOC_G_TOPOLOGY, topology);
	if (ret)
		return -errno;

	return 0;
}

struct media_v2_entity *media_topology_entity_find_by_name(struct media_v2_topology *topology,
                                                               const char *name) {
    struct media_v2_entity *entities;
    unsigned int i;

    if (!topology || !topology->num_entities || !topology->ptr_entities)
        return NULL;

    entities = (struct media_v2_entity *)topology->ptr_entities;

    for (i = 0; i < topology->num_entities; i++) {
        struct media_v2_entity *entity = &entities[i];
        printf("entity name: %s, %s\n", entity->name, name);
        if (strncmp(name, entity->name, strlen(name)) == 0)
            return entity;
    }

    return NULL;
}

struct media_v2_entity *media_topology_entity_find_by_function(struct media_v2_topology *topology,
							       unsigned int function)
{
	struct media_v2_entity *entities;
	unsigned int i;

	if (!topology || !topology->num_entities || !topology->ptr_entities)
		return NULL;

	entities = (struct media_v2_entity *)topology->ptr_entities;

	for (i = 0; i < topology->num_entities; i++) {
		struct media_v2_entity *entity = &entities[i];

		if (entity->function == function)
			return entity;
	}

	return NULL;
}

struct media_v2_interface *media_topology_interface_find_by_id(struct media_v2_topology *topology,
							       unsigned int id)
{
	struct media_v2_interface *interfaces;
	unsigned int interfaces_count;
	unsigned int i;

	if (!topology || !topology->num_interfaces || !topology->ptr_interfaces)
		return NULL;

	interfaces = (struct media_v2_interface *)topology->ptr_interfaces;
	interfaces_count = topology->num_interfaces;

	for (i = 0; i < interfaces_count; i++) {
		struct media_v2_interface *interface = &interfaces[i];

		if (interface->id == id)
			return interface;
	}

	return NULL;
}

struct media_v2_pad *media_topology_pad_find_by_entity(struct media_v2_topology *topology,
						       unsigned int entity_id,
						       unsigned int flags)
{
	struct media_v2_pad *pads;
	unsigned int pads_count;
	unsigned int i;

	if (!topology || !topology->num_pads || !topology->ptr_pads)
		return NULL;

	pads = (struct media_v2_pad *)topology->ptr_pads;
	pads_count = topology->num_pads;

	for (i = 0; i < pads_count; i++) {
		struct media_v2_pad *pad = &pads[i];

		if (pad->entity_id == entity_id &&
		    (pad->flags & flags) == flags)
			return pad;
	}

	return NULL;
}

struct media_v2_pad *media_topology_pad_find_by_id(struct media_v2_topology *topology,
						   unsigned int id)
{
	struct media_v2_pad *pads;
	unsigned int pads_count;
	unsigned int i;

	if (!topology || !topology->num_pads || !topology->ptr_pads)
		return NULL;

	pads = (struct media_v2_pad *)topology->ptr_pads;
	pads_count = topology->num_pads;

	for (i = 0; i < pads_count; i++) {
		struct media_v2_pad *pad = &pads[i];

		if (pad->id == id)
			return pad;
	}

	return NULL;
}

struct media_v2_link *media_topology_link_find_by_pad(struct media_v2_topology *topology,
						      unsigned int pad_id,
						      unsigned int pad_flags)
{
	struct media_v2_link *links;
	unsigned int links_count;
	unsigned int i;

	if (!topology || !topology->num_links || !topology->ptr_links)
		return NULL;

	links = (struct media_v2_link *)topology->ptr_links;
	links_count = topology->num_links;

	for (i = 0; i < links_count; i++) {
		struct media_v2_link *link = &links[i];

		if ((pad_flags & MEDIA_PAD_FL_SINK && link->sink_id == pad_id) ||
		    (pad_flags & MEDIA_PAD_FL_SOURCE && link->source_id == pad_id))
			return link;
	}

	return NULL;
}

struct media_v2_link *media_topology_link_find_by_entity(struct media_v2_topology *topology,
							 unsigned int entity_id,
							 unsigned int pad_flags)
{
	struct media_v2_link *links;
	unsigned int links_count;
	unsigned int i;

	if (!topology || !topology->num_links || !topology->ptr_links)
		return NULL;

	links = (struct media_v2_link *)topology->ptr_links;
	links_count = topology->num_links;

	for (i = 0; i < links_count; i++) {
		struct media_v2_link *link = &links[i];

		if ((pad_flags & MEDIA_PAD_FL_SINK && link->sink_id == entity_id) ||
		    (pad_flags & MEDIA_PAD_FL_SOURCE && link->source_id == entity_id))
			return link;
	}

	return NULL;
}

int media_request_alloc(int media_fd)
{
	int request_fd;
	int ret;

	ret = ioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd);
	if (ret)
		return -errno;

	return request_fd;
}

int media_request_queue(int request_fd)
{
	int ret;

	ret = ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
	if (ret)
		return -errno;

	return 0;
}

int media_request_reinit(int request_fd)
{
	int ret;

	ret = ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT, NULL);
	if (ret)
		return -errno;

	return 0;
}

int media_request_poll(int request_fd, struct timeval *timeout)
{
	fd_set except_fds;
	int ret;

	FD_ZERO(&except_fds);
	FD_SET(request_fd, &except_fds);

	ret = select(request_fd + 1, NULL, NULL, &except_fds, timeout);
	if (ret < 0)
		return -errno;

	if (!FD_ISSET(request_fd, &except_fds))
		return 0;

	return ret;
}
