/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2024, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2024, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#pragma once
#include <linux/kernel.h>

#define HANDLE_OFFSET_SIZE 32

typedef unsigned long (*copy_from_user_fn_t)(void *, const void *,
					     unsigned long);

static inline void *__alaska_translate(void *vaddr, copy_from_user_fn_t copy)
{
	uint64_t addr;
	void *handle_table_addr;
	uint64_t hte;
	uint64_t result;

	addr = (uint64_t)vaddr;
	// Handle check
	if ((addr >> 62) != 0b10)
		return vaddr;

	handle_table_addr = (void *)(addr >> (HANDLE_OFFSET_SIZE - 3));
	// EEP
	copy((void *)&hte, (const void __user *)handle_table_addr, sizeof(hte));

	result = (uint64_t)((uint64_t)hte +
			    (addr & ((1LU << HANDLE_OFFSET_SIZE) - 1)));

	// printk(KERN_INFO "alaska_translate(%llx) -> %llx\n", addr, result);

	return (void *)result;
}

#define alaska_translate(x) \
	(__typeof__(x))__alaska_translate((void *)(x), copy_from_user)
