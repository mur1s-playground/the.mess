#include "Mess.hpp"

#include <stdio.h>

void mess_init(struct mess *m) {
	bit_field_init(&m->bf, 256, 256);

	bit_field_register_filter(&m->bf, m, (void *) &mess_filter);
	bit_field_listen(&m->bf);
	bit_field_listenloop_start(&m->bf);
}

void mess_request_page(struct mess *m) {

}

void mess_filter(struct mess *m, unsigned int *packet) {
	printf("%i, %i\r\n", packet[0], packet[1]);
}
