#ifndef MESS_HPP
#define MESS_HPP

#include "BitField.hpp"

struct mess {
	struct bit_field bf;
};

void mess_init(struct mess *m);

void mess_filter(struct mess *m, unsigned int *packet);

#endif /* MESS_HPP */
