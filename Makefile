all: kernel progs

progs: syscall-client

NULL =

kernel_asm_files = \
	atomic.S \
	early-entry.S \
	high-entry.S \
	vector.S \
	$(NULL)

kernel_c_files = \
	arch.c \
	assert.c \
	early-mmu.c \
	init.c \
	large-object-cache.c \
	message.c \
	mmu.c \
	object-cache.c \
	once.c \
	small-object-cache.c \
	stdlib.c \
	thread.c \
	tree-map.c \
	vm.c \
	$(NULL)

kernel_c_dep_files = $(patsubst %.c, .%.c.depends, $(kernel_c_files))

-include $(kernel_c_dep_files)

kernel_objs = \
	$(patsubst %.c, %.ko, $(kernel_c_files)) \
	$(patsubst %.S, %.ko, $(kernel_asm_files)) \
	$(NULL)

kernel_asm_temps = $(patsubst %.c, %.s, $(kernel_c_files))
kernel_preproc_temps = $(patsubst %.c, %.i, $(kernel_c_files))

CROSS_COMPILE = arm-none-eabi

KERNEL_ASFLAGS += $(ASFLAGS) -Wall -Werror -g
KERNEL_CFLAGS += $(CFLAGS) -Wall -Werror -save-temps -g -march=armv6
KERNEL_LDFLAGS += $(LDFLAGS) -Wl,-T,kernel.ldscript

KERNEL_CC = $(CROSS_COMPILE)-gcc
KERNEL_AS = $(CROSS_COMPILE)-as
KERNEL_LD = $(CROSS_COMPILE)-gcc

kernel: kernel.ldscript

syscall_client_c_files = \
	crt.c \
	syscall-client.c \
	syscall.c \
	$(NULL)

syscall_client_c_dep_files = \
	$(patsubst %.c, .%.c.depends, $(syscall_client_c_files)) \
	$(NULL)

-include $(syscall_client_c_dep_files)

syscall_client_objs = \
	$(patsubst %.c, %.ko, $(syscall_client_c_files)) \
	$(NULL)

syscall_client_asm_temps = $(patsubst %.c, %.s, $(syscall_client_c_files))

syscall_client_preproc_temps = $(patsubst %.c, %.i, $(syscall_client_c_files))

syscall-client: $(syscall_client_objs)
	$(KERNEL_LD) -nostdlib -o $@ $+ 

%.ko: %.c
	@# Update dependencies
	@$(KERNEL_CC) $(KERNEL_CFLAGS) -M -MT $@ -o .$<.depends $<
	@# Build object
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c -o $@ $<

%.ko: %.S
	$(KERNEL_CC) $(KERNEL_ASFLAGS) -c -o $@ $<

kernel: $(kernel_objs)
	$(KERNEL_LD) $(KERNEL_LDFLAGS) -nostdlib -o $@ $(kernel_objs)

clean:
	rm -f \
		$(kernel_objs) \
		$(kernel_c_dep_files) \
		$(kernel_asm_temps) \
		$(kernel_preproc_temps) \
		kernel \
		$(syscall_client_objs) \
		$(syscall_client_c_dep_files) \
		$(syscall_client_asm_temps) \
		$(syscall_client_preproc_temps) \
		syscall-client \
		$(NULL)

debug: kernel
	$(CROSS_COMPILE)-gdb kernel --eval="target remote :1234"

run: kernel
	qemu-system-arm -s -S -kernel kernel -cpu arm1136

.PHONY: clean debug run
