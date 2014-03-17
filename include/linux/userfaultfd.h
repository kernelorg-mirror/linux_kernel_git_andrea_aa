/*
 *  include/linux/userfaultfd.h
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *  Copyright (C) 2014  Red Hat, Inc.
 *
 */

#ifndef _LINUX_USERFAULTFD_H
#define _LINUX_USERFAULTFD_H

#include <linux/fcntl.h>

/*
 * CAREFUL: Check include/uapi/asm-generic/fcntl.h when defining
 * new flags, since they might collide with O_* ones. We want
 * to re-use O_* flags that couldn't possibly have a meaning
 * from userfaultfd, in order to leave a free define-space for
 * shared O_* flags.
 */
#define UFFD_CLOEXEC O_CLOEXEC
#define UFFD_NONBLOCK O_NONBLOCK

#define UFFD_SHARED_FCNTL_FLAGS (O_CLOEXEC | O_NONBLOCK)
#define UFFD_FLAGS_SET (EFD_SHARED_FCNTL_FLAGS)

#ifdef CONFIG_USERFAULTFD

int handle_userfault(struct vm_area_struct *vma, unsigned long address,
		     unsigned int flags);

#else /* CONFIG_USERFAULTFD */

static int handle_userfault(struct vm_area_struct *vma, unsigned long address,
			    unsigned int flags)
{
	return VM_FAULT_SIGBUS;
}

#endif

#endif /* _LINUX_USERFAULTFD_H */
