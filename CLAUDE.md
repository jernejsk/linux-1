# Linux Kernel Development

## Coding standards
- Follow Documentation/process/coding-style.rst strictly
- Use `devm_*` resource-managed APIs for all device drivers
- Prefer `dev_err()`/`dev_info()` over `printk()` in drivers
- Always check return values; use `PTR_ERR()`/`IS_ERR()` for pointer errors
- Use `__iomem` for MMIO pointers, `__user` for userspace pointers
- Avoid `udelay()` > 10µs in interrupt context; use `msleep()` in process context
- Use `spin_lock_irqsave()` in interrupt context, `mutex_lock()` in process context

## Build (AArch64 cross-compile)
- Full build:    `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)`
- Menuconfig:   `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig`
- Single module: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=drivers/mydriver`
- compile_commands.json: `python3 scripts/clang-tools/gen_compile_commands.py`
