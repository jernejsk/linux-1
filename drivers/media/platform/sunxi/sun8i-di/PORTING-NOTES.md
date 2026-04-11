# Porting Notes: H616 (DI300) Deinterlace Driver

## Executive Summary

The H616 (sun50iw9) has a **completely different deinterlace IP block (DI300)**
compared to the H3/H5/R40 block currently supported by `sun8i-di`. The register
map, feature set, and operational model are fundamentally different. This is
**not** an incremental compatible addition — it requires a new hardware
abstraction layer underneath the existing V4L2 M2M framework.

---

## 1. Hardware Comparison

### 1.1 Register Map

The old (sun8i) and new (DI300) IPs share **no register compatibility**:

| Aspect               | sun8i-di (H3)                | DI300 (H616)                     |
|----------------------|------------------------------|----------------------------------|
| Base address         | (varies by SoC)              | 0x01420000                       |
| Register space       | ~0x900 bytes                 | ~0x2D0 + 0x10000 VOF SRAM area  |
| Module enable        | 0x00 (MOD_ENABLE)            | No equivalent — uses START reg   |
| Start trigger        | 0x04 bit16 (FRM_CTRL.START)  | 0x010 (START reg, bit 0)         |
| Interrupt enable     | 0x60                         | 0x014 (INT_CTL)                  |
| Interrupt status     | 0x64                         | 0x018 (STATUS, w1c bit 0)        |
| IP version           | None                         | 0x01C                            |
| Func enable          | Ctrl reg at 0xA0             | 0x020 (func_en: dit/md/tnr/fmd)  |
| DMA control          | FRM_CTRL bits                | 0x024 (dma_ctl: per-channel gating) |
| Size register        | 0x100/0x200 (per-channel)    | 0x030 (single combined register) |
| Format               | 0x4C / 0x5C (separate in/out)| 0x034 (single register, all fmts)|
| Field order          | 0x2C (FIELD_CTRL)            | 0x038 (field_order, BFF bit)     |
| Input pitch          | 0x40-0x48                    | 0x040-0x05C (top/bot split)      |
| Output pitch         | 0xD4-0xDC                    | 0x060-0x07C                      |
| Input addresses      | 0x20-0x28 (single set)       | 0x090-0x0EC (per-field top/bot)  |
| Output addresses     | 0x50-0x58                    | 0x0F0-0x11C (tnr/dit0/dit1)     |
| Flag addresses       | 0xC0-0xC4                    | 0x120-0x128                      |
| Scaler               | 0x100-0x228 + coef tables    | **None** — no built-in scaler    |
| CSC                  | 0x70 (12 CSC coef regs)      | **None**                         |
| DIT (deinterlace)    | 0xA0-0xBC                    | 0x1A0-0x1CC                      |
| MD (motion detect)   | Part of DIT ctrl at 0xA0     | 0x180-0x194 (separate block)     |
| FMD (film detect)    | **None**                     | 0x1D0-0x234 (new block)          |
| TNR (temporal NR)    | **None**                     | 0x240-0x2AC (new block)          |
| VOF SRAM             | **None**                     | 0x10000-0x103FF                  |
| Software reset       | Via reset controller only     | 0x000 bit 31 (self-reset)        |

### 1.2 Feature Comparison

| Feature              | sun8i-di (H3)     | DI300 (H616)                    |
|----------------------|--------------------|---------------------------------|
| Deinterlace modes    | Passthrough, Weave, Bob, Mixed | Weave, Bob, Motion-adaptive |
| Output frames/field  | 2 (both fields)    | 1 or 2 (configurable)           |
| Scaler               | Yes (bilinear)     | **No**                          |
| CSC                  | Yes (bypass-able)  | **No**                          |
| Motion Detection     | Built into DIT     | Separate MD block with flag I/O |
| Film Mode Detection  | No                 | **Yes** (FMD block + SW alg)    |
| TNR                  | No                 | **Yes** (temporal noise reduction)|
| Field Order Detect   | No (user-specified)| **Yes** (FOD SW algorithm)      |
| Interlace Detection  | No                 | **Yes** (ITD SW algorithm)      |
| Video-On-Film        | No                 | **Yes** (VOF detection + SRAM)  |
| Input frames         | 1 current + 1 prev | Up to 3 (di/prev/cur)           |
| Supported formats    | NV12, NV21         | NV12, NV21, NV16, NV61, YU12, YV12, YU16, YV16 |
| Input addressing     | Linear (single)    | Per-field top/bottom split      |
| Max size             | 2048x1100          | 2048x2048 (11-bit fields)       |

### 1.3 Clock / Reset / DT

| Resource  | sun8i-di          | DI300 (H616)                      |
|-----------|-------------------|-----------------------------------|
| Clocks    | bus, mod, ram     | clk_di (mod), clk_bus_di          |
| Resets    | single unnamed    | rst_bus_di                        |
| Base addr | SoC-dependent     | 0x01420000                        |
| IRQ       | SoC-dependent     | SPI 89                            |
| Reg size  | varies            | 0x40000                           |

Note: H616 has **no ram clock** — only mod and bus clocks.

---

## 2. Porting Steps

### Step 1: Define DI300 register map header

Create `sun50i-di.h` (or extend `sun8i-di.h` with an ifdef section) with all
DI300 register offsets and bitfield definitions. Use proper `#define` macros
with shifts and masks (upstream kernel style), **not** the vendor's
union-of-bitfield approach (which is endianness-fragile and non-upstream).

Source: `di300_reg.h` — translate all `union` bitfield structs to `#define`
macros.

Key register groups to define:
- **TOP**: reset(0x00), func_vsn(0x0C), start(0x10), int_ctl(0x14),
  status(0x18), ip_version(0x1C), func_en(0x20), dma_ctl(0x24),
  rdma/wdma_cmd_ctl(0x28-0x2C), size(0x30), fmt(0x34), forder(0x38)
- **Pitch**: in_f01_pitch(0x40-0x4C), in_f2_pitch(0x50-0x5C),
  out_tnr_pitch(0x60-0x6C), out_dit_pitch(0x70-0x7C), flag_pitch(0x80)
- **Addresses**: in_f0(0x90-0xAC), in_f1(0xB0-0xCC), in_f2(0xD0-0xEC),
  out_tnr(0xF0-0xFC), out_dit0(0x100-0x10C), out_dit1(0x110-0x11C),
  flags(0x120-0x128)
- **MD**: md_para(0x180), md_crop(0x190-0x194)
- **DIT**: dit_setting(0x1A0), chroma params(0x1A4-0x1AC),
  intra/inter intp(0x1B0-0x1B4), crop/demo(0x1C0-0x1CC)
- **FMD**: diff thresholds(0x1D0-0x1D8), field histograms(0x1E0-0x1FC),
  feature thresholds(0x200-0x218), field_hist results(0x21C-0x220),
  global(0x224), crop(0x230-0x234)
- **TNR**: strength(0x240), dark thresholds(0x244-0x254),
  feather detect(0x258), dt_filter(0x25C), weight(0x260),
  abnormal detect(0x264), sum registers(0x268-0x28C),
  dither/random(0x290-0x298), md_result(0x29C), crop/demo(0x2A0-0x2AC)

### Step 2: Create SoC-specific data structure

Add a `struct deinterlace_variant` (or similar) to distinguish H3-class vs
DI300-class hardware:

```c
struct deinterlace_variant {
    bool has_scaler;
    bool has_tnr;
    bool has_fmd;
    unsigned int max_width;
    unsigned int max_height;
    const u32 *supported_formats;
    unsigned int num_formats;
    /* ops pointers for HW-specific init/run/irq */
    void (*init)(struct deinterlace_dev *dev);
    void (*setup_frame)(struct deinterlace_ctx *ctx);
    irqreturn_t (*irq_handler)(struct deinterlace_dev *dev);
};
```

Wire this through `of_device_id.data`.

### Step 3: Adapt context structure for DI300 needs

The DI300 needs more per-context state than the sun8i version:

```c
/* Additional fields for DI300 contexts */
struct deinterlace_ctx {
    /* ... existing fields ... */

    /* DI300: motion detection flag buffers (2, ping-pong) */
    void *md_flag_buf[2];
    dma_addr_t md_flag_buf_dma[2];
    unsigned int md_flag_dir;  /* toggles 0/1 each frame */

    /* DI300: previous TWO source buffers (3-frame motion adaptive) */
    struct vb2_v4l2_buffer *prev2;  /* frame before prev */

    /* DI300: field order tracking */
    bool bff;  /* bottom-field-first detected */
};
```

For the initial driver, **omit** the vendor's complex software algorithm state
(`di_dev_cdata`, `di_dev_proc_result`, all `__alg_hist` structs). The FMD/ITD/VOF/TNR
algorithms are entirely software and can be added incrementally later. Start with
basic motion-adaptive deinterlacing (DIT + MD only).

### Step 4: Implement DI300 hardware init

Port `di_dev_apply_fixed_para()` → `deinterlace_di300_init()`.

This sets up all the one-time register values:
- `func_en`: enable DIT + MD (skip TNR/FMD initially)
- `size`: (width-1) | ((height-1) << 16)
- `dma_ctl`: gate relevant DMA channels + mclk_gate
- `flag_pitch`: computed from width — `ALIGN(width*2, 256) / 8`
- `md_para`: fixed value 0x21360c04
- `dit_setting`: mode bits based on MOTION mode, enable diag_intp
- `dit_chr_para0/1`: fixed values 0x30058000 / 0x04300000
- `dit_intra_para`: fixed value 0x514240ac
- `dit_inter_para`: initial value 0x22000000
- Crop registers: set to full frame (0 to width-1, 0 to height-1)

### Step 5: Implement DI300 frame setup (device_run)

Port `di_dev_set_top_para()` + `di_dev_set_fb()` → `deinterlace_di300_device_run()`.

Key differences from sun8i:
1. **Input addressing is per-field**: Each input frame needs **two** addresses
   (top field and bottom field). Bottom = top + ystride. The HW reads
   alternating lines from top/bot address sets.
2. **Pitch is 2× line stride** for input (because pitch = distance between two
   lines of the same field = 2 × distance between adjacent lines).
3. **Output pitch is 1× stride** (progressive output).
4. **Field order (BFF)** must be set in `forder` register.
5. **Motion flag buffers** ping-pong: on each frame, swap which is read vs
   written.
6. **Format register** is a single reg covering all inputs and outputs.

Frame processing flow:
```
1. Set field order (forder.bff)
2. Set MD flag buffer addresses (ping-pong toggle)
3. Set input FB addresses (top/bot split, for each input frame)
4. Set input pitches (2× stride for interlaced)
5. Set output FB addresses
6. Set output pitches (1× stride)
7. Set format register
8. Set crop windows
9. Write start bit (start.start = 1)
```

### Step 6: Implement DI300 IRQ handler

Port `di_irq_handler()` → `deinterlace_di300_irq()`.

DI300 IRQ flow:
1. Read `status` register
2. Check `finish_flag` (bit 0)
3. Clear by writing 1 to bit 0 (w1c)
4. Complete the M2M job

Note: DI300 does **not** have the writeback error status bit that sun8i has.
Instead check `status.busy` — if still busy after finish IRQ, something is
wrong.

### Step 7: Adapt format handling

DI300 supports more formats than sun8i. For initial bring-up, keep NV12/NV21
only (matching existing driver). The format conversion function
`di_dev_convert_fmt()` maps:
- YU12/YV12 → 0 (planar YUV420)
- NV12/NV21 → 1 (UV-combined YUV420)
- YU16/YV16 → 2 (planar YUV422)
- NV16/NV61 → 3 (UV-combined YUV422)

The UV sequence (uvseq bit) selects UV vs VU ordering.

### Step 8: Adapt clock/reset handling

DI300 on H616 uses:
- `clk_di` (module clock) — set to 300 MHz (parent: PLL_PERIPH0_2X)
- `clk_bus_di` (bus/AHB gate clock)
- `rst_bus_di` (bus reset)

There is **no ram clock**. Make `ram_clk` optional (or use variant data to
skip it).

Power-on sequence: enable mod clock → enable bus clock → deassert reset.
Power-off sequence: assert reset → disable bus clock → disable mod clock.

### Step 9: Adapt buffer requirements

sun8i needs 2 flag buffers per context (width × height / 4 each).

DI300 needs 2 MD flag buffers per context sized as:
- `w_bit = width * 2`
- `w_stride = ALIGN(w_bit, 256) / 8`  (32-byte alignment on bit-count)
- `size = height * w_stride`

These are significantly larger than the sun8i flag buffers.

DI300 30Hz mode needs only 2 input FBs; 60Hz mode needs 3 input FBs.
For initial implementation, target 30Hz mode (1 output frame per 2 input fields).
This matches the existing sun8i behavior.

### Step 10: Remove scaler from DI300 path

The existing sun8i driver programs scaler registers (CH0/CH1 sizes, scale
factors, filter coefficients). DI300 has **no scaler** — remove all scaler
setup from the DI300 code path. Output size must equal input size.

In the V4L2 layer, for DI300 variant:
- `try_fmt_vid_cap` should force output size = input size
- Remove `framesizes` stepwise (or report only discrete = input size)
- Remove scale factor programming

### Step 11: DT binding

Create a new compatible string: `"allwinner,sun50i-h616-deinterlace"`

DT node example:
```dts
deinterlace: deinterlace@1420000 {
    compatible = "allwinner,sun50i-h616-deinterlace";
    reg = <0x01420000 0x40000>;
    interrupts = <GIC_SPI 89 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&ccu CLK_DI>, <&ccu CLK_BUS_DI>;
    clock-names = "mod", "bus";
    resets = <&ccu RST_BUS_DI>;
};
```

Note: no `ram` clock.

### Step 12: Wire it all together with of_device_id

```c
static const struct deinterlace_variant sun8i_h3_variant = {
    .has_scaler = true,
    .has_tnr = false,
    .has_fmd = false,
    .max_width = 2048,
    .max_height = 1100,
    /* ... */
};

static const struct deinterlace_variant sun50i_h616_variant = {
    .has_scaler = false,
    .has_tnr = false,   /* initially; can enable later */
    .has_fmd = false,   /* initially; can enable later */
    .max_width = 2048,
    .max_height = 2048,
    /* ... */
};

static const struct of_device_id deinterlace_dt_match[] = {
    { .compatible = "allwinner,sun8i-h3-deinterlace",
      .data = &sun8i_h3_variant },
    { .compatible = "allwinner,sun50i-h616-deinterlace",
      .data = &sun50i_h616_variant },
    { /* sentinel */ }
};
```

---

## 3. Phased Implementation Plan

### Phase 1: Basic 30Hz motion-adaptive DI (MVP)
- New register header for DI300
- Variant infrastructure to select sun8i vs DI300 code paths
- DI300 init, device_run, IRQ handler
- DIT + MD only (no FMD/TNR/VOF)
- 30Hz mode: 2 input frames → 1 output frame
- NV12/NV21 formats only
- No scaler (output = input size)
- DT binding + DTS for H616

### Phase 2: 60Hz mode (2 output frames per field pair)
- 3 input frames required (di + prev + cur)
- 2 output frames per processing cycle
- Adapt V4L2 buffer management for 3-in/2-out

### Phase 3: FMD (Film Mode Detection)
- Port the software FMD algorithm from `di300_alg.c`
- Read FMD hardware histogram registers after each frame
- Feed results back to DIT `dit_inter_para` register

### Phase 4: TNR (Temporal Noise Reduction)
- Enable TNR block
- Additional output buffer for TNR
- Port TNR adaptive gain control algorithm
- Only supports 3-plane planar formats (YU12/YV12)

### Phase 5: Additional formats
- Add YU12, YV12, NV16, NV61, YU16, YV16 support
- Handle planar vs semi-planar addressing differences

### Phase 6: FOD/ITD/VOF algorithms
- Port field order detection
- Port interlace detection (progressive content in interlaced stream)
- Port video-on-film detection (requires VOF SRAM save/restore)

---

## 4. Key Gotchas / Non-obvious Details

1. **Input pitch doubling**: DI300 input pitches must be 2× the line stride
   because the HW reads fields (every other line). The vendor driver does
   `fb->buf.ystride << 1`.

2. **Top/bottom field addressing**: Each input frame has separate top and
   bottom field addresses. Bottom = top + one line stride (not doubled).

3. **MD flag buffer sizing**: Uses bit-level addressing — `width * 2` bits per
   line, 32-byte aligned stride. Not byte-per-pixel.

4. **Self-reset register**: DI300 has a software reset at register 0x000 bit 31.
   Assert then deassert. Used for error recovery.

5. **VOF SRAM access**: The BIST mode must be enabled (`bist_ctl.bist_mode_en = 1`)
   before reading/writing VOF SRAM at offsets 0x10000+. Must disable after.
   Only needed for 60Hz mode with FMD.

6. **No CSC/bypass**: DI300 has no CSC — input must already be in YUV. No bypass
   register needed.

7. **mclk_gate**: `dma_ctl` bit 31 must be set to gate the memory clock. Always
   set this.

8. **Format register is shared**: Unlike sun8i which has separate in/out format
   registers, DI300 has a single `fmt` register where different bit fields
   control each input/output port independently.

9. **Crop registers**: All crop registers require 4-byte alignment on all
   coordinates. The vendor driver checks and rejects non-aligned crops.

10. **No writeback stride control**: Unlike sun8i which has a
    `WB_LINE_STRIDE_CTRL` enable bit, DI300 always uses the output pitch
    registers directly.
