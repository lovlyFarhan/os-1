break dabt_handler
break failed_assert

add-symbol-file build/echo              0x10000
add-symbol-file build/syscall-client    0x20000
add-symbol-file build/uio               0x30000
add-symbol-file build/pl011             0x40000
