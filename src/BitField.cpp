#include "BitField.hpp"

#include "env.h"

#ifdef NVCC_PRESENT
	#include <cuda_runtime.h>
#endif

#include "Utils.hpp"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <cassert>
#include <errno.h>

//TODO:
//	solves everything 	-> 	implement registration of defragmentation callbacks (to make it possible to move pages, to reduce total fieldsize)
//	reduces unused space 	-> 	maybe circumvent by implementing counting of free space before reaching occupied by "to relocated data" <-- should be very quick to implement maybe
//
//	implement update_all_pages flag/parameter/option
//
//	remember some empty line configurations for faster new allocations when bit field is big
//	if >= certain size skip to index, etc.

void bit_field_init(struct bit_field *bf, unsigned int pages, unsigned int pagesize) {
	bf->data = (unsigned int *) malloc((pages*(pagesize+1))*sizeof(unsigned int));
	memset(bf->data, 0, (pages*(pagesize+1))*sizeof(unsigned int));
	bf->pages = pages;
	bf->pagesize = pagesize;

	bf->biggest_tracked_allocated_page = 0;

	bf->invalidators_c = 0;
	bf->devices_c = 0;

	bf->sockets_c = 0;
	bf->filters_c = 0;
}

/* INTERNAL */
void bit_field_set_invalidators(struct bit_field *bf, unsigned int page, unsigned int page_count) {
	for (int i = 0; i < bf->invalidators_c; i++) {
		for (int j = 0; j < page_count; j++) {
			(&(*bf->invalidators[i]))[(page+j)/8] |= 1 << ((page+j) % 8);
		}
	}
}

void bit_field_lock_all(struct bit_field *bf) {
        if (bf->sockets_c > 0) {
                for (int i = 0; i < bf->pages; i++) {
                        pthread_mutex_lock(&bf->pagelocks[i]);
                }
        }
}

void bit_field_unlock_all(struct bit_field *bf) {
        if (bf->sockets_c > 0) {
                for (int i = 0; i < bf->pages; i++) {
                        pthread_mutex_unlock(&bf->pagelocks[i]);
                }
        }
}

void bit_field_lock_pages(struct bit_field *bf, unsigned int page, unsigned int pagecount) {
        if (bf->sockets_c > 0) {
                for (int i = page; i < page+pagecount; i++) {
                        pthread_mutex_lock(&bf->pagelocks[i]);
                }
        }
}

void bit_field_unlock_pages(struct bit_field *bf, unsigned int page, unsigned int pagecount) {
        if (bf->sockets_c > 0) {
                for (int i = page; i < page+pagecount; i++) {
                        pthread_mutex_unlock(&bf->pagelocks[i]);
                }
        }
}

void bit_field_add_pages(struct bit_field *bf, unsigned int pages) {
	if (bf->sockets_c > 0) {
		for (int i = 0; i < bf->pages; i++) {
			pthread_mutex_lock(&bf->pagelocks[i]);
		}
	}

	bf->data = (unsigned int *) realloc(bf->data, (bf->pages+pages)*(bf->pagesize+1)*sizeof(unsigned int));
	memset(&bf->data[bf->pages*(bf->pagesize+1)], 0, pages*(bf->pagesize+1)*sizeof(unsigned int));
#ifdef NVCC_PRESENT
	for (int i = 0; i < bf->invalidators_c; i++) {
		bf->invalidators[i] = (unsigned char *) realloc(&(*bf->invalidators[i]), (bf->pages+7+pages+7)/8 * sizeof(unsigned char));
		memset(&(bf->invalidators[i])[(bf->pages+7)/8], 1, (pages+7)/8 * sizeof(unsigned char));
	}
	for (int i = 0; i < bf->devices_c; i++) {
		unsigned int *device_ptr = NULL;
		cudaError_t err = cudaSuccess;
	        err = cudaSetDevice(bf->device_ids[i]);
		err = cudaMalloc((void **)&device_ptr, (bf->pages+pages)*(bf->pagesize+1)* sizeof(int));
		err = cudaMemcpy(device_ptr, bf->device_data[i], bf->pages*(bf->pagesize+1)*sizeof(int), cudaMemcpyDeviceToDevice);
		cudaFree(bf->device_data[i]);												//maybe do some delayed free
		bf->device_data[i] = device_ptr;
	}
#endif
	if (bf->sockets_c > 0) {
		bf->pagelocks = (pthread_mutex_t *) realloc(bf->pagelocks, (bf->pages+pages)*sizeof(pthread_mutex_t));
		for (int i = bf->pages; i < bf->pages+pages; i++) {
			pthread_mutex_init(&bf->pagelocks[i], NULL);
		}
	}

	bf->pages += pages;

	if (bf->sockets_c > 0) {
		for (int i = 0; i < bf->pages-pages; i++) {
			pthread_mutex_unlock(&bf->pagelocks[i]);
		}
	}
}

unsigned int bit_field_get_pagetype(const struct bit_field *bf, const unsigned int page) {
	return (bf->data[page*(bf->pagesize+1)]);
}

unsigned int bit_field_get_pagetype_from_index(const struct bit_field *bf, const unsigned int index) {
	return (bf->data[index-(index % (bf->pagesize+1))]);
}

void bit_field_set_pagetype(struct bit_field *bf, const unsigned int page, const unsigned int type) {
	bf->data[page*(bf->pagesize+1)] = type;
}

unsigned int bit_field_get_value(const struct bit_field *bf, const unsigned int page, const unsigned int position) {
	return bf->data[page*(bf->pagesize+1)+1+position];
}

unsigned int bit_field_get_index(const struct bit_field *bf, const unsigned int page, const unsigned int position) {
	return page*(bf->pagesize+1)+1+position;
}

unsigned int bit_field_get_page_from_index(const struct bit_field *bf, const unsigned int index) {
	return (index / (bf->pagesize+1));
}

unsigned int bit_field_get_free_location(struct bit_field *bf, const unsigned int size, const unsigned int skip) {
	//improve (skip, etc)
	bit_field_lock_all(bf);

	int skip_ac = skip;
	if (bf->pages > 10000 && skip_ac == 0) {
		skip_ac = bf->biggest_tracked_allocated_page;
	}
	for (int i = skip_ac; i < bf->pages; i++) {
		if (bit_field_get_pagetype(bf, i) == 0) {
			int type = 1;
			while (type < size+1) type *= 2;
			if (type > bf->pagesize) {
				int occupied = 0;
				for (int j = 1; j < ceil(type / (float)bf->pagesize); j++) {
					while (i+j >= bf->pages) bit_field_add_pages(bf, bf->pages/2);
					if (bit_field_get_pagetype(bf, i+j) != 0) {
						occupied = j;
						break;
					}
				}
				if (occupied != 0) {
					i += (occupied-1);
					continue;
				}
			}
			bit_field_set_pagetype(bf, i, type);
			if (i > bf->biggest_tracked_allocated_page) {
				bf->biggest_tracked_allocated_page = i;
//				printf("bf->pages: %i, fpol: %i\r\n", bf->pages, bf->biggest_tracked_allocated_page);
			}
		}

		int type = bit_field_get_pagetype(bf, i);
		if (type >= size+1 && type < bf->pagesize) {
			for (int j = 0; j < bf->pagesize-type+1; j += type) {
                                if (bit_field_get_value(bf, i, j) == 0) {
					bit_field_set_invalidators(bf, i, 1);
					bit_field_unlock_all(bf);
                                        return bit_field_get_index(bf, i, j);
                                }
                        }
		} else if (type == bf->pagesize) {
			if (bit_field_get_value(bf, i, 0) == 0) {
				bit_field_set_invalidators(bf, i, 1);
				bit_field_unlock_all(bf);
				return bit_field_get_index(bf, i, 0);
			}
		} else if (type > bf->pagesize) {
			if (bit_field_get_value(bf, i, 0) == 0) {
				bit_field_set_invalidators(bf, i, ceil(type/(float)(bf->pagesize+1)));
				bit_field_unlock_all(bf);
				return bit_field_get_index(bf, i, 0);
			}
			i += floor((type/(float)(bf->pagesize+1)));
		}
	}
	int old_pages = bf->pages;
	bit_field_unlock_all(bf);
	bit_field_add_pages(bf, bf->pages/2);
	return bit_field_get_free_location(bf, size, old_pages);
}
/* END INTERNAL */

unsigned int bit_field_add_data(struct bit_field *bf, const unsigned int datum) {
	int index = bit_field_get_free_location(bf, 1, 0);
	bit_field_lock_pages(bf, bit_field_get_page_from_index(bf, index), 1);
	bf->data[index] = 1;
	bf->data[index+1] = datum;
	bit_field_unlock_pages(bf, bit_field_get_page_from_index(bf, index), 1);
	return index;
}

unsigned int bit_field_add_bulk(struct bit_field *bf, const unsigned int *data, const unsigned int data_len) {
	int index = bit_field_get_free_location(bf, data_len, 0);
	unsigned int pagecount = bit_field_get_page_from_index(bf, index+data_len) - bit_field_get_page_from_index(bf, index) + 1;
	bit_field_lock_pages(bf, bit_field_get_page_from_index(bf, index), pagecount);
	bf->data[index] = data_len;
	memcpy(&bf->data[index+1], data, data_len*sizeof(unsigned int));
	bit_field_unlock_pages(bf, bit_field_get_page_from_index(bf, index), pagecount);
	return index;
}

//TODO: a lot of space for improvement
unsigned int bit_field_add_data_to_segment(struct bit_field *bf, const unsigned int index, const unsigned int datum) {
	int page = bit_field_get_page_from_index(bf, index);

	bit_field_lock_pages(bf, page, 1);

	int size = bf->data[index];
	unsigned int pagecount = bit_field_get_page_from_index(bf, index+size+1) - page + 1;
	if (pagecount > 1) {
		bit_field_lock_pages(bf, page+1, pagecount-1);
	}

	int pagetype = bit_field_get_pagetype_from_index(bf, index);
	if (size+1+1 < pagetype) {
		bf->data[index+1+size] = datum;
		bf->data[index]++;
		bit_field_set_invalidators(bf, page, 1);
		if (bit_field_get_page_from_index(bf, index+1+size) != page) {
			bit_field_set_invalidators(bf, bit_field_get_page_from_index(bf, index+1+size), 1);
		}
		bit_field_unlock_pages(bf, page, pagecount);
		return index;
	}

	int new_index = bit_field_get_free_location(bf, size+1, 0);

	//recheck concurrency
	bit_field_lock_pages(bf, bit_field_get_page_from_index(bf, new_index), pagecount);

	memcpy(&bf->data[new_index], &bf->data[index], (size+1)*sizeof(unsigned int));

	//delete old line/s
	memset(&bf->data[index], 0, (size+1)*sizeof(unsigned int));
	bit_field_set_invalidators(bf, page, ceil(pagetype/(float)(bf->pagesize+1)));

	//clear pagetype if old page empty
	if (pagetype <= bf->pagesize) {
		int is_empty = 1;
		for (int i = page*(bf->pagesize+1)+1; i < page*(bf->pagesize+1)+1 + bf->pagesize; i += pagetype) {
                        if (bf->data[i] != 0) {
				is_empty = 0;
				break;
                        }
                }
		if (is_empty == 1) {
			bit_field_set_pagetype(bf, page, 0);
		}
	} else {
		bit_field_set_pagetype(bf, page, 0);
	}
	//invalidate old line/s
//	bit_field_set_invalidators(bf, page, ceil(pagetype/(float)(bf->pagesize+1)));

	//update data (shouled be invalidated due to get_free_location call)
	bf->data[new_index] = size+1;
	bf->data[new_index+1+size] = datum;

	bit_field_unlock_pages(bf, page, pagecount);
	bit_field_unlock_pages(bf, bit_field_get_page_from_index(bf, new_index), pagecount);
	return new_index;
}

unsigned int bit_field_add_bulk_to_segment(struct bit_field *bf, const unsigned int index, const unsigned int *data, const unsigned int data_len) {
	int page = bit_field_get_page_from_index(bf, index);

	bit_field_lock_pages(bf, page, 1);
	int size = bf->data[index];

	unsigned int pagecount = bit_field_get_page_from_index(bf, index+size+1+data_len) - page + 1;
	if (pagecount > 1) {
		bit_field_lock_pages(bf, page+1, pagecount-1);
	}

	int pagetype = bit_field_get_pagetype_from_index(bf, index);
        if (size+1+data_len < pagetype) {
		memcpy(&bf->data[index+1+size], data, data_len*sizeof(unsigned int));
		bf->data[index] += data_len;

		//greedy invalidators must be improved, to only invalidate the actual invalid lines
		bit_field_set_invalidators(bf, page, ceil(pagetype/(float)(bf->pagesize+1)));

		bit_field_unlock_pages(bf, page, pagecount);
                return index;
        }

        int new_index = bit_field_get_free_location(bf, size+data_len, 0);
	bit_field_lock_pages(bf, bit_field_get_page_from_index(bf, new_index), pagecount);

        memcpy(&bf->data[new_index], &bf->data[index], (size+1)*sizeof(unsigned int));

	//delete old line/s
        memset(&bf->data[index], 0, (size+1)*sizeof(unsigned int));
	bit_field_set_invalidators(bf, page, ceil(pagetype/(float)(bf->pagesize+1)));

        //clear pagetype if old page/s empty
        if (pagetype <= bf->pagesize) {
                int is_empty = 1;
                for (int i = page*(bf->pagesize+1)+1; i < page*(bf->pagesize+1)+1 + bf->pagesize; i += pagetype) {
                        if (bf->data[i] != 0) {
                                is_empty = 0;
                                break;
                        }
                }
                if (is_empty == 1) {
                        bit_field_set_pagetype(bf, page, 0);
                }
        } else {
                bit_field_set_pagetype(bf, page, 0);
        }
	//invalidate old line/s
//	bit_field_set_invalidators(bf, page, ceil(pagetype/(float)(bf->pagesize+1)));

        //update data (should be invalidated due to get_free_location call)
	memcpy(&bf->data[new_index+1+size], data, data_len*sizeof(unsigned int));
	bf->data[new_index] = size+data_len;

        bit_field_unlock_pages(bf, page, pagecount);
        bit_field_unlock_pages(bf, bit_field_get_page_from_index(bf, new_index), pagecount);

        return new_index;
}

void bit_field_update_data(struct bit_field *bf, const unsigned int index, const unsigned int datum, const unsigned char broadcast) {
	int page = bit_field_get_page_from_index(bf, index);
	bit_field_lock_pages(bf, page, 1);
	bf->data[index] = datum;

	if (broadcast > 0) {
		unsigned int size = 3*sizeof(unsigned int);
		unsigned int *packet = (unsigned int *) malloc(size);
		packet[0] = PT_DATUM;
		packet[1] = index;
		packet[2] = datum;

		int ret = -1;
		for (int i = 0; i < bf->sockets_c; i++) {
			ret = sendto(bf->sockets_out[i], packet, size, 0, (struct sockaddr*)&bf->ip6addrs_out[i], sizeof(bf->ip6addrs_out[i]));
		        if (ret == -1 || ret != size) {
        		        fprintf(stderr, "ERROR: Unable to send packet, %i, %i\r\n", ret, errno);
//	        	        return;
	        	}
		}
	}
	bit_field_set_invalidators(bf, page, 1);
	bit_field_unlock_pages(bf, page, 1);
}

unsigned int bit_field_remove_data_from_segment(struct bit_field *bf, const unsigned int index, const unsigned int datum) {
	int page = bit_field_get_page_from_index(bf, index);
	int pagetype = bit_field_get_pagetype_from_index(bf, index);
	int size = bf->data[index];
	if (size == 1) {
		bf->data[index] = 0;
		assert(bf->data[index+1] == datum);
		bf->data[index+1] = 0;
		bit_field_set_invalidators(bf, page, 1);

		//clear pagetype if old page/s empty
	        if (pagetype <= bf->pagesize) {
        	        int is_empty = 1;
                	for (int i = page*(bf->pagesize+1)+1; i < page*(bf->pagesize+1)+1 + bf->pagesize; i += pagetype) {
                        	if (bf->data[i] != 0) {
                                	is_empty = 0;
	                                break;
        	                }
	                }
        	        if (is_empty == 1) {
                	        bit_field_set_pagetype(bf, page, 0);
	                }
        	} else {
	                bit_field_set_pagetype(bf, page, 0);
        	}

		return 0;
	}
	for (int i = 0; i < size; i++) {
		if (bf->data[index+1+i] == datum) {
			if (i == size-1) {
				bf->data[index+1+i] = 0;
			} else {
				bf->data[index+1+i] = bf->data[index+1+size-1];
				bf->data[index+1+size-1] = 0;
			}
			bf->data[index]--;

			int page_removed_item = bit_field_get_page_from_index(bf, index+1+i);
			int page_moved_item = bit_field_get_page_from_index(bf, index+1+size-1);
			bit_field_set_invalidators(bf, page, 1);
			if (page_removed_item != page) {
				bit_field_set_invalidators(bf, page_removed_item, 1);
			}
			if (page_moved_item != page && page_moved_item != page_removed_item) {
				bit_field_set_invalidators(bf, page_moved_item, 1);
			}
			return index;
		}
	}
	assert(index == 0); //should not be reached
	return index;
}

/* INTERNAL */
unsigned int bit_field_register_invalidator(struct bit_field *bf) {
	if (bf->invalidators_c == 0) {
		bf->invalidators = (unsigned char **) malloc(sizeof(unsigned char *));
	} else {
		bf->invalidators = (unsigned char **) realloc(bf->invalidators, (bf->invalidators_c+1)*sizeof(unsigned char *));
	}
	bf->invalidators[bf->invalidators_c++] = (unsigned char *) malloc((bf->pages+7)/8 * sizeof(unsigned char));
	memset(bf->invalidators[bf->invalidators_c-1], 1, (bf->pages+7)/8 * sizeof(unsigned char));
	return bf->invalidators_c-1;
}
/* END INTERNAL */

unsigned int bit_field_register_device(struct bit_field *bf, unsigned int device_id) {
#ifdef NVCC_PRESENT
	if (bf->devices_c == 0) {
		bf->device_data = (unsigned int **) malloc(sizeof(unsigned int *));
		bf->device_ids = (unsigned int *) malloc(sizeof(unsigned int));
	} else {
		bf->device_data = (unsigned int **) realloc(bf->device_data, (bf->devices_c+1)*sizeof(unsigned int *));
		bf->device_ids = (unsigned int *) realloc(bf->device_ids, (bf->devices_c+1)*sizeof(unsigned int));
	}
	bit_field_register_invalidator(bf);

	cudaError_t err = cudaSuccess;
	err = cudaSetDevice(device_id);
	bf->device_data[bf->devices_c] = NULL;
	err = cudaMalloc((void **)&bf->device_data[bf->devices_c], bf->pages*(bf->pagesize+1)*sizeof(unsigned int));
	if (err != cudaSuccess) {
                fprintf(stderr, "Error allocating device_data (error code %s)!\n", cudaGetErrorString(err));
                exit(EXIT_FAILURE);
        }
	bf->device_ids[bf->devices_c++] = device_id;
	return bf->devices_c-1;
#endif
	return 0;
}

void bit_field_update_device(const struct bit_field *bf, unsigned int device_id) {
#ifdef NVCC_PRESENT
	cudaError_t err = cudaSuccess;
	err = cudaSetDevice(device_id);
	for (int i = 0; i < bf->devices_c; i++) {
		if (bf->device_ids[i] == device_id) {
			int cp_size = 0;
			int cp_startpage = 0;
			int cp_started = 0;
			for (int j = 0; j < (bf->pages+7)/8; j++) {
//				printf("%c\r\n", (&(*bf->invalidators[i])[j]));
				if ((&(*bf->invalidators[i]))[j] > 0) {
					if (!cp_started) {
						cp_started = 1;
						cp_startpage = j;
					}
					if ((j*8)+7 >= bf->pages-1) {
						cp_size += (bf->pages - (j*8));
					} else {
						cp_size += 8;
					}
				} else {
					if (cp_started) {
						err = cudaMemcpy(&bf->device_data[i][(cp_startpage*8)*(bf->pagesize+1)], &bf->data[(cp_startpage*8)*(bf->pagesize+1)], cp_size*(bf->pagesize+1)*sizeof(unsigned int), cudaMemcpyHostToDevice);
	                                        if (err != cudaSuccess) {
        	                                        fprintf(stderr, "Error copying device_data (error code %s)!\n", cudaGetErrorString(err));
                	                                exit(EXIT_FAILURE);
                        	                }
						cp_started = 0;
						cp_size = 0;
					}
				}
			}
			if (cp_started) {
                               	err = cudaMemcpy(&bf->device_data[i][(cp_startpage*8)*(bf->pagesize+1)], &bf->data[(cp_startpage*8)*(bf->pagesize+1)], cp_size*(bf->pagesize+1)*sizeof(unsigned int), cudaMemcpyHostToDevice);
                                if (err != cudaSuccess) {
                                       	fprintf(stderr, "Error copying device_data (error code %s)!\n", cudaGetErrorString(err));
                                        exit(EXIT_FAILURE);
                                }
                        }
			memset(bf->invalidators[i], 0, (bf->pages+7)/8 * sizeof(unsigned char));
			break;
		}
	}
#endif
}

void bit_field_listen(struct bit_field *bf) {
	if (bf->sockets_c == 0) {
		bf->sockets = (int *) malloc(sizeof(int));
		bf->ip6addrs = (struct sockaddr_in6 *) malloc(sizeof(struct sockaddr_in6));
		bf->ip6groups = (struct ipv6_mreq *) malloc(sizeof(struct ipv6_mreq));

		bf->sockets_out = (int *) malloc(sizeof(int));
		bf->ip6addrs_out = (struct sockaddr_in6 *) malloc(sizeof(struct sockaddr_in6));

		bf->pagelocks = (pthread_mutex_t *) malloc(bf->pages*sizeof(pthread_mutex_t));
		for (int i = 0; i < bf->pages; i++) {
			pthread_mutex_init(&bf->pagelocks[i], NULL);
		}
	} else {
		bf->sockets = (int *) realloc(bf->sockets, (bf->sockets_c+1)*sizeof(int));
		bf->ip6addrs = (struct sockaddr_in6 *) realloc(bf->ip6addrs, (bf->sockets_c+1)*sizeof(struct sockaddr_in6));
		bf->ip6groups = (struct ipv6_mreq *) realloc(bf->ip6groups, (bf->sockets_c+1)*sizeof(struct ipv6_mreq));

		bf->sockets_out = (int *) realloc(bf->sockets_out, (bf->sockets_c+1)*sizeof(int));
		bf->ip6addrs_out = (struct sockaddr_in6 *) realloc(bf->ip6addrs_out, (bf->sockets_c+1)*sizeof(struct sockaddr_in6));
	}
	memset(&bf->ip6addrs[bf->sockets_c], 0, sizeof(struct sockaddr_in6));
	memset(&bf->ip6groups[bf->sockets_c], 0, sizeof(struct ipv6_mreq));

        bf->ip6addrs[bf->sockets_c].sin6_family = AF_INET6;
        bf->ip6addrs[bf->sockets_c].sin6_port = htons(60000);
        bf->ip6addrs[bf->sockets_c].sin6_addr = in6addr_any;
	int net_id = util_get_netiface_id();
        bf->ip6addrs[bf->sockets_c].sin6_scope_id = net_id;

        bf->sockets[bf->sockets_c] = socket(AF_INET6, SOCK_DGRAM, 0);
        if (bf->sockets[bf->sockets_c] < 0) {
                fprintf(stderr, "ERROR: Unable to create socket\n");
                return;
        }

	int enable = 1;
	if (setsockopt(bf->sockets[bf->sockets_c], SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
	  	fprintf(stderr, "ERROR: setsockopt(SO_REUSEADDR) failed");
		return;
	}

        if (bind(bf->sockets[bf->sockets_c], (struct sockaddr *)&(bf->ip6addrs[bf->sockets_c]), sizeof(struct sockaddr_in6)) < 0) {
                fprintf(stderr, "ERROR: Unable to bind socket\n");
                return;
        }

        int ret;
        bf->ip6groups[bf->sockets_c].ipv6mr_interface = net_id;
        ret = inet_pton(AF_INET6, "ff02::1337:1337:1337:1337", &bf->ip6groups[bf->sockets_c].ipv6mr_multiaddr);
        if (ret == 0) {
                fprintf(stderr, "ERROR: Unable to parse network address\n");
                return;
        } else if (ret == -1) {
                fprintf(stderr, "ERROR: Address family not supported\n");
                return;
        }
        ret = setsockopt(bf->sockets[bf->sockets_c], IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (const char *)&bf->ip6groups[bf->sockets_c], sizeof(struct ipv6_mreq));
        if (ret == -1) {
                fprintf(stderr, "ERROR: Unable to set socket options\n");
                return;
        }

        memset(&bf->ip6addrs_out[bf->sockets_c], 0, sizeof(struct sockaddr_in6));

        bf->ip6addrs_out[bf->sockets_c].sin6_family = bf->ip6addrs[0].sin6_family;
        bf->ip6addrs_out[bf->sockets_c].sin6_port = bf->ip6addrs[0].sin6_port;
//        int net_id = util_get_netiface_id();
        bf->ip6addrs_out[bf->sockets_c].sin6_scope_id = net_id;
        inet_pton(AF_INET6, "ff02::1337:1337:1337:1337", &bf->ip6addrs_out[bf->sockets_c].sin6_addr);

        int sock = socket(AF_INET6, SOCK_DGRAM, 0);
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
	        fprintf(stderr, "ERROR: setsockopt(SO_REUSEADDR) failed");
                return;
        }
	bf->sockets_out[bf->sockets_c] = sock;

	bf->sockets_c++;
}

void *bit_field_listenloop(void *bf_v) {
	struct bit_field *bf = (struct bit_field *) bf_v;
	int ret = -1;
	int header_size = sizeof(unsigned int);
	while (1) {//TODO: preallocate packet_buffers
		unsigned int *packet = (unsigned int *) malloc((bf->pagesize+1+1+1)*sizeof(unsigned int));
		ret = read(bf->sockets[0], packet, header_size);
		if (ret == header_size) {
			unsigned int packet_type = packet[0];
			if (packet_type == PT_PAGE) {
				int rbts = 0;
				while (rbts < bf->pagesize+1+1) {
					rbts += read(bf->sockets[0], &packet[rbts+1], bf->pagesize+1+1-rbts);
				}
			} else if (packet_type == PT_DATUM) {
				int rbts = 0;
				while (rbts < 2) {
					rbts += read(bf->sockets[0], &packet[rbts+1], 2-rbts);
				}
			} else {
				printf("unknown packet tpye\r\n");
			}

			if (bf->filters_c > 0) {
				for (int i = 0; i < bf->filters_c; i++) {
				        void (*f)(void *parent, unsigned int *) = (void (*)(void *parent, unsigned int *)) bf->filters[i];
				        f(bf->parents[i], packet);
				}
			} else {
				if (packet_type == PT_PAGE) {
					bit_field_lock_pages(bf, packet[1], 1);
					memcpy(&bf->data[packet[2]*(bf->pagesize+1)], &packet[2], (bf->pagesize+1)*sizeof(unsigned int));
					bit_field_unlock_pages(bf, packet[1], 1);
				} else if (packet_type == PT_DATUM) {
					bit_field_lock_pages(bf, bit_field_get_page_from_index(bf, packet[1]), 1);
					bit_field_update_data(bf, packet[1], packet[2], 0);
					bit_field_unlock_pages(bf, bit_field_get_page_from_index(bf, packet[1]), 1);
				} else {
					printf("unable to handle packet\r\n");
				}
				free(packet);
			}
		} else {
			fprintf(stderr, "ret: %i\r\n", ret);
		}
//		bit_field_dump(bf);
	}
	return NULL;
}

void bit_field_listenloop_start(struct bit_field *bf) {
	pthread_t *t = (pthread_t *) malloc(sizeof(pthread_t));
	pthread_create(t, NULL, &bit_field_listenloop, (void *) bf);
}

void bit_field_register_filter(struct bit_field *bf, void *parent, void *filter) {
	if (bf->filters_c == 0) {
		bf->filters = (void **) malloc(sizeof(void *));
		bf->parents = (void **) malloc(sizeof(void *));
	} else {
		bf->filters = (void **) realloc(bf->filters, (bf->filters_c+1)*sizeof(void *));
		bf->parents = (void **) realloc(bf->parents, (bf->filters_c+1)*sizeof(void *));
	}

	bf->filters[bf->filters_c] = filter;
	bf->parents[bf->filters_c] = parent;

	bf->filters_c++;
}

void bit_field_broadcast_page(struct bit_field *bf, unsigned int page) {
	bit_field_lock_pages(bf, page, 1);
	unsigned int *packet = (unsigned int *) malloc((bf->pagesize+1+1+1)*sizeof(unsigned int));
	unsigned int index = page*(bf->pagesize+1);
	packet[0] = PT_PAGE;
	packet[1] = page;
	memcpy(&packet[2], &bf->data[index], (bf->pagesize+1)*sizeof(unsigned int));
	for (int i = 0; i < bf->sockets_c; i++) {
		sendto(bf->sockets_out[i], packet, (bf->pagesize+1+1+1)*sizeof(unsigned int), 0, (struct sockaddr*)&bf->ip6addrs_out[i], sizeof(bf->ip6addrs_out[i]));
	}
	bit_field_unlock_pages(bf, page, 1);
}

void bit_field_dump(const struct bit_field *bf) {
	printf("dumping bitfield, pages: %i\r\n", bf->pages);
	for (int i = 0; i < bf->pages; i++) {
		printf("%i\t", bit_field_get_pagetype(bf, i));
		for (int j = 0; j < bf->pagesize; j++) {
			printf("%i\t", bit_field_get_value(bf, i, j));
		}
		printf("\r\n");
	}
	for (int i = 0; i < bf->invalidators_c; i++) {
		for (int j = 0; j < (bf->pages+7)/8; j++) {
			for (int k = 0; k < 8; k++) {
				if (((&(*bf->invalidators[i]))[j] >> k) & 0x1) {
					printf("1");
				} else {
					printf("0");
				}
			}
		}
		printf("\r\n");
	}
	printf("end dumping bitfield\r\n");
}
