//! 对生产 Pillow 10.2.0 整数图像操作的有界直接移植。不替代 Pillow，不含编解码、字体或通用形状 API。每个实例由单个工作线程拥有，固定缓冲区不可重入。
//! Bounded direct ports of the production Pillow 10.2.0 integer image operations.
//! Not a Pillow replacement: no codec, font, or general shape API.
//! A single worker owns each instance; the fixed buffers are not reentrant.
const MAX_PIXELS: usize = 4_194_304;
const MAX_SIDE: u32 = 8192;
const MAX_BYTES: usize = MAX_PIXELS * 4;
static mut INPUT: [u8; MAX_BYTES] = [0; MAX_BYTES];
static mut DESTINATION: [u8; MAX_BYTES] = [0; MAX_BYTES];
static mut MASK: [u8; MAX_PIXELS] = [0; MAX_PIXELS];
static mut OUTPUT: [u8; MAX_BYTES] = [0; MAX_BYTES];
static mut BBOX: [u32; 4] = [0; 4];

#[no_mangle]
pub extern "C" fn image_input() -> *mut u8 {
    std::ptr::addr_of_mut!(INPUT).cast()
}
#[no_mangle]
pub extern "C" fn image_destination() -> *mut u8 {
    std::ptr::addr_of_mut!(DESTINATION).cast()
}
#[no_mangle]
pub extern "C" fn image_mask() -> *mut u8 {
    std::ptr::addr_of_mut!(MASK).cast()
}
#[no_mangle]
pub extern "C" fn image_output() -> *mut u8 {
    std::ptr::addr_of_mut!(OUTPUT).cast()
}
#[no_mangle]
pub extern "C" fn image_bbox_result() -> *mut u32 {
    std::ptr::addr_of_mut!(BBOX).cast()
}
fn geometry(width: u32, height: u32, channels: u32) -> Option<usize> {
    if width == 0
        || height == 0
        || width > MAX_SIDE
        || height > MAX_SIDE
        || !matches!(channels, 1 | 3 | 4)
    {
        return None;
    }
    let pixels = width as usize * height as usize;
    (pixels <= MAX_PIXELS).then_some(pixels * channels as usize)
}
// 使用 Pillow 的 SHIFTFORDIV255，不使用浮点混合或预乘 Canvas。
// Pillow's SHIFTFORDIV255, not floating point blending or premultiplied Canvas.
fn shift_div255(n: u32) -> u32 {
    ((n >> 8) + n) >> 8
}
fn blend(dst: u8, src: u8, alpha: u8) -> u8 {
    shift_div255(
        u32::from(dst) * (255 - u32::from(alpha)) + u32::from(src) * u32::from(alpha) + 128,
    ) as u8
}

/// 整数裁剪，源范围外全部置零，包括隐藏 RGB。
/// Integer crop. Areas outside the source are zero, including hidden RGB.
#[no_mangle]
pub extern "C" fn image_crop(
    w: u32,
    h: u32,
    channels: u32,
    left: i32,
    top: i32,
    out_w: u32,
    out_h: u32,
) -> i32 {
    let (Some(_), Some(len)) = (geometry(w, h, channels), geometry(out_w, out_h, channels)) else {
        return -1;
    };
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), (w * h * channels) as usize);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        output.fill(0);
        let c = channels as usize;
        for y in 0..out_h {
            let sy = i64::from(top) + i64::from(y);
            if !(0..i64::from(h)).contains(&sy) {
                continue;
            }
            for x in 0..out_w {
                let sx = i64::from(left) + i64::from(x);
                if !(0..i64::from(w)).contains(&sx) {
                    continue;
                }
                let from = (sy as usize * w as usize + sx as usize) * c;
                let to = (y as usize * out_w as usize + x as usize) * c;
                output[to..to + c].copy_from_slice(&input[from..from + c]);
            }
        }
    }
    len as i32
}

/// Pillow 的左右翻转（轴 0）或上下翻转（轴 1）。
/// Pillow FLIP_LEFT_RIGHT (axis 0) or FLIP_TOP_BOTTOM (axis 1).
#[no_mangle]
pub extern "C" fn image_flip(w: u32, h: u32, channels: u32, axis: u32) -> i32 {
    let Some(len) = geometry(w, h, channels) else {
        return -1;
    };
    if axis > 1 {
        return -1;
    }
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        let c = channels as usize;
        for y in 0..h {
            for x in 0..w {
                let sx = if axis == 0 { w - 1 - x } else { x };
                let sy = if axis == 1 { h - 1 - y } else { y };
                let from = ((sy * w + sx) * channels) as usize;
                let to = ((y * w + x) * channels) as usize;
                output[to..to + c].copy_from_slice(&input[from..from + c]);
            }
        }
    }
    len as i32
}

/// 将 INPUT 粘贴到 DESTINATION；遮罩 0 表示无，1 使用 L MASK，2 使用源 alpha。带遮罩粘贴还会混合 alpha 本身，与源覆盖合成不同。
/// Paste source INPUT over DESTINATION; mask 0 none, 1 L MASK, 2 source alpha.
/// Alpha is itself blended for masked paste, unlike source-over compositing.
#[no_mangle]
pub extern "C" fn image_paste(
    sw: u32,
    sh: u32,
    dw: u32,
    dh: u32,
    channels: u32,
    left: i32,
    top: i32,
    mask_kind: u32,
) -> i32 {
    let (Some(src_len), Some(len)) = (geometry(sw, sh, channels), geometry(dw, dh, channels))
    else {
        return -1;
    };
    if mask_kind > 2 || (mask_kind == 2 && channels != 4) {
        return -1;
    }
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), src_len);
        let destination = std::slice::from_raw_parts(image_destination(), len);
        let mask = std::slice::from_raw_parts(image_mask(), (sw * sh) as usize);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        output.copy_from_slice(destination);
        for sy in 0..sh {
            let dy = i64::from(top) + i64::from(sy);
            if !(0..i64::from(dh)).contains(&dy) {
                continue;
            }
            for sx in 0..sw {
                let dx = i64::from(left) + i64::from(sx);
                if !(0..i64::from(dw)).contains(&dx) {
                    continue;
                }
                let from = ((sy * sw + sx) * channels) as usize;
                let to = (dy as usize * dw as usize + dx as usize) * channels as usize;
                let a = match mask_kind {
                    1 => mask[(sy * sw + sx) as usize],
                    2 => input[from + 3],
                    _ => 255,
                };
                for k in 0..channels as usize {
                    output[to + k] = blend(output[to + k], input[from + k], a);
                }
            }
        }
    }
    len as i32
}

/// RGBA 源覆盖目标，使用 Pillow 精确的 7 位系数。
/// RGBA source over destination, using Pillow's exact 7-bit coefficients.
#[no_mangle]
pub extern "C" fn image_alpha_composite(
    sw: u32,
    sh: u32,
    dw: u32,
    dh: u32,
    left: i32,
    top: i32,
) -> i32 {
    let (Some(src_len), Some(len)) = (geometry(sw, sh, 4), geometry(dw, dh, 4)) else {
        return -1;
    };
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), src_len);
        let destination = std::slice::from_raw_parts(image_destination(), len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        output.copy_from_slice(destination);
        for sy in 0..sh {
            let dy = i64::from(top) + i64::from(sy);
            if !(0..i64::from(dh)).contains(&dy) {
                continue;
            }
            for sx in 0..sw {
                let dx = i64::from(left) + i64::from(sx);
                if !(0..i64::from(dw)).contains(&dx) {
                    continue;
                }
                let from = ((sy * sw + sx) * 4) as usize;
                let to = (dy as usize * dw as usize + dx as usize) * 4;
                let sa = u32::from(input[from + 3]);
                if sa == 0 {
                    continue;
                }
                let da = u32::from(output[to + 3]);
                let outa255 = sa * 255 + da * (255 - sa);
                let coef1 = sa * 255 * 255 * 128 / outa255;
                let coef2 = 255 * 128 - coef1;
                for k in 0..3 {
                    let value =
                        u32::from(input[from + k]) * coef1 + u32::from(output[to + k]) * coef2;
                    output[to + k] = (shift_div255(value + (128 << 7)) >> 7) as u8;
                }
                output[to + 3] = shift_div255(outa255 + 128) as u8;
            }
        }
    }
    len as i32
}

/// 美术内核使用的 L/RGB/RGBA 转换；丢弃 alpha，不做合成。
/// L/RGB/RGBA conversions used by art kernels. Alpha is discarded, not composited.
#[no_mangle]
pub extern "C" fn image_convert(w: u32, h: u32, from_channels: u32, to_channels: u32) -> i32 {
    let (Some(src_len), Some(len)) = (geometry(w, h, from_channels), geometry(w, h, to_channels))
    else {
        return -1;
    };
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), src_len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        for (src, dst) in input
            .chunks_exact(from_channels as usize)
            .zip(output.chunks_exact_mut(to_channels as usize))
        {
            let (r, g, b) = if from_channels == 1 {
                (src[0], src[0], src[0])
            } else {
                (src[0], src[1], src[2])
            };
            if to_channels == 1 {
                dst[0] =
                    ((u32::from(r) * 19595 + u32::from(g) * 38470 + u32::from(b) * 7471 + 0x8000)
                        >> 16) as u8;
            } else {
                dst[0] = r;
                dst[1] = g;
                dst[2] = b;
                if to_channels == 4 {
                    dst[3] = if from_channels == 4 { src[3] } else { 255 };
                }
            }
        }
    }
    len as i32
}

/// 提取一个通道，或将 MASK[0..256] 作为 L 查找表（通道 4）。
/// Extract one channel, or apply MASK[0..256] as an L lookup table (channel 4).
#[no_mangle]
pub extern "C" fn image_channel(w: u32, h: u32, channels: u32, channel: u32) -> i32 {
    let Some(src_len) = geometry(w, h, channels) else {
        return -1;
    };
    if channel >= channels && !(channel == 4 && channels == 1) {
        return -1;
    }
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), src_len);
        let output = std::slice::from_raw_parts_mut(image_output(), (w * h) as usize);
        let lut = std::slice::from_raw_parts(image_mask(), 256);
        for (src, dst) in input.chunks_exact(channels as usize).zip(output.iter_mut()) {
            *dst = if channel == 4 {
                lut[src[0] as usize]
            } else {
                src[channel as usize]
            };
        }
    }
    (w * h) as i32
}

#[no_mangle]
pub extern "C" fn image_put_alpha(w: u32, h: u32) -> i32 {
    let Some(len) = geometry(w, h, 4) else {
        return -1;
    };
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        let mask = std::slice::from_raw_parts(image_mask(), (w * h) as usize);
        output.copy_from_slice(input);
        for (p, a) in output.as_chunks_mut::<4>().0.iter_mut().zip(mask) {
            p[3] = *a;
        }
    }
    len as i32
}

/// RGBA 仅按 alpha 求边界（Pillow 10.2 默认）；其他模式按非零 L 或 RGB。空集返回 0，有结果返回 1，无效返回 -1；右/下界为排他边界。
/// Alpha-only RGBA bbox (the Pillow 10.2 default); nonzero L or RGB bbox otherwise.
/// Returns 0 for empty, 1 for present, -1 for invalid. Result is exclusive right/bottom.
#[no_mangle]
pub extern "C" fn image_bbox(w: u32, h: u32, channels: u32, alpha_only: u32) -> i32 {
    let Some(len) = geometry(w, h, channels) else {
        return -1;
    };
    if alpha_only > 1 {
        return -1;
    }
    let mut bbox = [w, h, 0, 0];
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), len);
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * channels) as usize;
                let nonzero = if channels == 4 && alpha_only == 1 {
                    input[i + 3] != 0
                } else {
                    input[i..i + channels as usize].iter().any(|v| *v != 0)
                };
                if nonzero {
                    bbox[0] = bbox[0].min(x);
                    bbox[1] = bbox[1].min(y);
                    bbox[2] = bbox[2].max(x + 1);
                    bbox[3] = bbox[3].max(y + 1);
                }
            }
        }
        std::ptr::copy_nonoverlapping(bbox.as_ptr(), image_bbox_result(), 4);
    }
    i32::from(bbox[2] != 0)
}

/// 生产只用 L MaxFilter(5)，边缘扩展与 Pillow RankFilter 一致。
/// Production uses only L MaxFilter(5). Edge extension matches Pillow RankFilter.
#[no_mangle]
pub extern "C" fn image_max_filter(w: u32, h: u32, size: u32) -> i32 {
    let Some(len) = geometry(w, h, 1) else {
        return -1;
    };
    if !matches!(size, 3 | 5) {
        return -1;
    }
    let radius = i64::from(size / 2);
    unsafe {
        let input = std::slice::from_raw_parts(image_input(), len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        for y in 0..h {
            for x in 0..w {
                let mut maximum = 0;
                for ky in -radius..=radius {
                    for kx in -radius..=radius {
                        let sy = (i64::from(y) + ky).clamp(0, i64::from(h) - 1) as usize;
                        let sx = (i64::from(x) + kx).clamp(0, i64::from(w) - 1) as usize;
                        maximum = maximum.max(input[sy * w as usize + sx]);
                    }
                }
                output[(y * w + x) as usize] = maximum;
            }
        }
    }
    len as i32
}

// 边界遵循 Pillow Resample.c，每次只计算一个轴，不动态分配。
// Bounds follow Pillow Resample.c. One axis is computed at a time: no dynamic allocation.
const MAX_COEFFS: usize = MAX_SIDE as usize * 9;
static mut COEFFS: [i32; MAX_COEFFS] = [0; MAX_COEFFS];
static mut BOUNDS: [[usize; 3]; MAX_SIDE as usize] = [[0; 3]; MAX_SIDE as usize];
static mut WEIGHTS: [f64; MAX_SIDE as usize] = [0.0; MAX_SIDE as usize];
fn sinc(x: f64) -> f64 {
    if x == 0.0 {
        1.0
    } else {
        let x = x * std::f64::consts::PI;
        x.sin() / x
    }
}
fn coefficients(input: u32, output: u32) -> bool {
    let scale = f64::from(input) / f64::from(output);
    let filter_scale = scale.max(1.0);
    let support = 3.0 * filter_scale;
    let reciprocal = 1.0 / filter_scale;
    let mut offset = 0;
    unsafe {
        let weights = std::slice::from_raw_parts_mut(
            std::ptr::addr_of_mut!(WEIGHTS).cast::<f64>(),
            MAX_SIDE as usize,
        );
        let coeffs = std::slice::from_raw_parts_mut(
            std::ptr::addr_of_mut!(COEFFS).cast::<i32>(),
            MAX_COEFFS,
        );
        let bounds = std::slice::from_raw_parts_mut(
            std::ptr::addr_of_mut!(BOUNDS).cast::<[usize; 3]>(),
            output as usize,
        );
        for (x, bound) in bounds.iter_mut().enumerate() {
            let center = (x as f64 + 0.5) * scale;
            // C 类型转换向零截断，包括负的左边界。
            // C casts truncate toward zero, including negative left edges.
            let min = ((center - support + 0.5) as i32).max(0) as usize;
            let max = ((center + support + 0.5) as u32).min(input) as usize;
            let count = max - min;
            if offset + count > MAX_COEFFS {
                return false;
            }
            let mut sum = 0.0;
            for (k, weight) in weights.iter_mut().take(count).enumerate() {
                let delta = (k as f64 + min as f64 - center + 0.5) * reciprocal;
                *weight = if (-3.0..3.0).contains(&delta) {
                    sinc(delta) * sinc(delta / 3.0)
                } else {
                    0.0
                };
                sum += *weight;
            }
            for k in 0..count {
                let weight = if sum == 0.0 {
                    weights[k]
                } else {
                    weights[k] / sum
                };
                coeffs[offset + k] =
                    (weight * 4_194_304.0 + if weight < 0.0 { -0.5 } else { 0.5 }) as i32;
            }
            *bound = [min, count, offset];
            offset += count;
        }
    }
    true
}

/// 仅支持整图 Lanczos，不支持小数框或 reducing_gap。RGBA 使用 Pillow RGBa 转换及分别取整的两次 22 位处理，而非通用浮点滤波。验证后将 INPUT 作为临时存储改写为预乘 RGBA。
/// Full-image Lanczos only; no fractional box/reducing_gap. RGBA uses Pillow RGBa
/// conversion and two separately rounded 22-bit passes, not generic float filtering.
/// INPUT is scratch-mutated to premultiplied RGBA after validation.
#[no_mangle]
pub extern "C" fn image_resize_lanczos(
    w: u32,
    h: u32,
    channels: u32,
    out_w: u32,
    out_h: u32,
) -> i32 {
    let (Some(src_len), Some(len), Some(intermediate_len)) = (
        geometry(w, h, channels),
        geometry(out_w, out_h, channels),
        geometry(out_w, h, channels),
    ) else {
        return -1;
    };
    unsafe {
        let input = std::slice::from_raw_parts_mut(image_input(), src_len);
        let intermediate = std::slice::from_raw_parts_mut(image_destination(), intermediate_len);
        let output = std::slice::from_raw_parts_mut(image_output(), len);
        if w == out_w && h == out_h {
            output.copy_from_slice(input);
            return len as i32;
        }
        if channels == 4 {
            for pixel in input.as_chunks_mut::<4>().0 {
                let a = u32::from(pixel[3]);
                for value in &mut pixel[..3] {
                    *value = shift_div255(u32::from(*value) * a + 128) as u8;
                }
            }
        }
        let c = channels as usize;
        if w == out_w {
            intermediate.copy_from_slice(input);
        } else {
            if !coefficients(w, out_w) {
                return -1;
            }
            let coeffs =
                std::slice::from_raw_parts(std::ptr::addr_of!(COEFFS).cast::<i32>(), MAX_COEFFS);
            let bounds = std::slice::from_raw_parts(
                std::ptr::addr_of!(BOUNDS).cast::<[usize; 3]>(),
                out_w as usize,
            );
            for y in 0..h as usize {
                for (x, &[start, count, offset]) in bounds.iter().enumerate() {
                    for channel in 0..c {
                        let mut sum = 1_i32 << 21;
                        for k in 0..count {
                            sum += i32::from(input[(y * w as usize + start + k) * c + channel])
                                * coeffs[offset + k];
                        }
                        intermediate[(y * out_w as usize + x) * c + channel] =
                            (sum >> 22).clamp(0, 255) as u8;
                    }
                }
            }
        }
        if h == out_h {
            output.copy_from_slice(intermediate);
        } else {
            if !coefficients(h, out_h) {
                return -1;
            }
            let coeffs =
                std::slice::from_raw_parts(std::ptr::addr_of!(COEFFS).cast::<i32>(), MAX_COEFFS);
            let bounds = std::slice::from_raw_parts(
                std::ptr::addr_of!(BOUNDS).cast::<[usize; 3]>(),
                out_h as usize,
            );
            for (y, &[start, count, offset]) in bounds.iter().enumerate() {
                for x in 0..out_w as usize {
                    for channel in 0..c {
                        let mut sum = 1_i32 << 21;
                        for k in 0..count {
                            sum += i32::from(
                                intermediate[((start + k) * out_w as usize + x) * c + channel],
                            ) * coeffs[offset + k];
                        }
                        output[(y * out_w as usize + x) * c + channel] =
                            (sum >> 22).clamp(0, 255) as u8;
                    }
                }
            }
        }
        if channels == 4 {
            for pixel in output.as_chunks_mut::<4>().0 {
                let a = u32::from(pixel[3]);
                if a != 0 && a != 255 {
                    for value in &mut pixel[..3] {
                        *value = (255 * u32::from(*value) / a).min(255) as u8;
                    }
                }
            }
        }
    }
    len as i32
}
