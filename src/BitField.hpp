#ifndef BITFIELD_HPP
#define BITFIELD_HPP

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <pthread.h>

enum packet_type {
	PT_PAGE,
	PT_DATUM
};

struct bit_field {
	//bit_field
	unsigned int pages;
	unsigned int pagesize;
	unsigned int *data;

	pthread_mutex_t *pagelocks;

	unsigned int biggest_tracked_allocated_page;

	//device mirror
	unsigned char **invalidators;
	unsigned int invalidators_c;

	unsigned int **device_data;
	unsigned int *device_ids;
	unsigned int devices_c;

	//network mirror
	int *sockets;
	struct sockaddr_in6 *ip6addrs;
        struct ipv6_mreq *ip6groups;

	int *sockets_out;
	struct sockaddr_in6 *ip6addrs_out;

	unsigned int sockets_c;

	void **filters;
	void **parents;
	unsigned int filters_c;
};

void bit_field_init(struct bit_field *bf, unsigned int pages, unsigned int pagesize);

unsigned int bit_field_add_data(struct bit_field *bf, const unsigned int datum);
unsigned int bit_field_add_bulk(struct bit_field *bf, const unsigned int *data, const unsigned int data_len);

unsigned int bit_field_add_data_to_segment(struct bit_field *bf, const unsigned int index, const unsigned int datum);
unsigned int bit_field_add_bulk_to_segment(struct bit_field *bf, const unsigned int index, const unsigned int *data, const unsigned int data_len);

void bit_field_update_data(struct bit_field *bf, const unsigned int index, const unsigned int datum, const unsigned char broadcast);

unsigned int bit_field_remove_data_from_segment(struct bit_field *bf, const unsigned int index, const unsigned int datum);

unsigned int bit_field_register_device(struct bit_field *bf, unsigned int device_id);
void bit_field_update_device(const struct bit_field *bf, unsigned int device_id);

void bit_field_listen(struct bit_field *bf);
void *bit_field_listenloop(void *bf_v);
void bit_field_listenloop_start(struct bit_field *bf);

void bit_field_register_filter(struct bit_field *bf, void *parent, void *filter);

void bit_field_broadcast_page(struct bit_field *bf, unsigned int page);

void bit_field_dump(const struct bit_field *bf);

#endif /* BITFIELD_HPP */
