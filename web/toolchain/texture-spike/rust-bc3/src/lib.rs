//! 移植自 dm/ios/tools/gen_widestash_art.py::bc3。固定缓冲区，无分配器或宿主导入，每个线程拥有一个实例。
//! Port of dm/ios/tools/gen_widestash_art.py::bc3.
//! Fixed buffers, no allocator or host imports. One worker owns one instance.
const MAX_PIXELS: usize = 4 * 1024 * 1024;
const OUTPUT_CAP: usize = MAX_PIXELS + 65536;
static mut INPUT: [u8; MAX_PIXELS * 4] = [0; MAX_PIXELS * 4];
static mut OUTPUT: [u8; OUTPUT_CAP] = [0; OUTPUT_CAP];
#[no_mangle]
pub extern "C" fn bc3_input() -> *mut u8 {
    std::ptr::addr_of_mut!(INPUT).cast()
}
#[no_mangle]
pub extern "C" fn bc3_output() -> *mut u8 {
    std::ptr::addr_of_mut!(OUTPUT).cast()
}
fn q565(e: [f64; 3]) -> u16 {
    ((e[0] * 31.0 / 255.0).round_ties_even() as u16) << 11
        | ((e[1] * 63.0 / 255.0).round_ties_even() as u16) << 5
        | (e[2] * 31.0 / 255.0).round_ties_even() as u16
}
fn un565(c: u16) -> [f64; 3] {
    [
        f64::from(c >> 11 & 31) * 255.0 / 31.0,
        f64::from(c >> 5 & 63) * 255.0 / 63.0,
        f64::from(c & 31) * 255.0 / 31.0,
    ]
}
fn block(p: &[[f64; 4]; 16]) -> [u8; 16] {
    let mut mu = [0.0; 3];
    for pixel in p {
        for j in 0..3 {
            mu[j] += pixel[j];
        }
    }
    for value in &mut mu {
        *value /= 16.0;
    }
    let mut c = [[0.0; 3]; 16];
    let mut cov = [[0.0; 3]; 3];
    for i in 0..16 {
        for j in 0..3 {
            c[i][j] = p[i][j] - mu[j];
        }
    }
    for pixel in c {
        for i in 0..3 {
            for j in 0..3 {
                cov[i][j] += pixel[i] * pixel[j];
            }
        }
    }
    let mut v = [1.0; 3];
    for _ in 0..8 {
        let mut next = [0.0; 3];
        for i in 0..3 {
            next[i] = cov[i][0] * v[0] + cov[i][1] * v[1] + cov[i][2] * v[2];
        }
        let norm = (next[0] * next[0] + next[1] * next[1] + next[2] * next[2])
            .sqrt()
            .max(1e-9);
        for i in 0..3 {
            v[i] = next[i] / norm;
        }
    }
    let mut lo = 0;
    let mut hi = 0;
    let mut projection = [0.0; 16];
    for i in 0..16 {
        projection[i] = c[i][0] * v[0] + c[i][1] * v[1] + c[i][2] * v[2];
        if projection[i] < projection[lo] {
            lo = i;
        }
        if projection[i] > projection[hi] {
            hi = i;
        }
    }
    let a = q565([p[hi][0], p[hi][1], p[hi][2]]);
    let b = q565([p[lo][0], p[lo][1], p[lo][2]]);
    let c0 = a.max(b);
    let c1 = a.min(b);
    let p0 = un565(c0);
    let p1 = un565(c1);
    let mut pal = [[0.0; 3]; 4];
    pal[0] = p0;
    pal[1] = p1;
    for j in 0..3 {
        pal[2][j] = (2.0 * p0[j] + p1[j]) / 3.0;
        pal[3][j] = (p0[j] + 2.0 * p1[j]) / 3.0;
    }
    let mut a0: f64 = 0.0;
    let mut a1: f64 = 255.0;
    for pixel in p {
        a0 = a0.max(pixel[3]);
        a1 = a1.min(pixel[3]);
    }
    let mut ap = [0.0; 8];
    ap[0] = a0;
    ap[1] = a1;
    for k in 1..7 {
        ap[k + 1] = ((7 - k) as f64 * a0 + k as f64 * a1) / 7.0;
    }
    let mut cb = 0u32;
    let mut ab = 0u64;
    for (i, pixel) in p.iter().enumerate() {
        let mut ci = 0;
        let mut best = f64::INFINITY;
        if c0 != c1 {
            for (k, color) in pal.iter().enumerate() {
                let r = pixel[0] - color[0];
                let g = pixel[1] - color[1];
                let b = pixel[2] - color[2];
                let distance = r * r + g * g + b * b;
                if distance < best {
                    best = distance;
                    ci = k;
                }
            }
        }
        let mut ai = 0;
        best = f64::INFINITY;
        if a0 != a1 {
            for (k, alpha) in ap.iter().enumerate() {
                let distance = (pixel[3] - alpha).abs();
                if distance < best {
                    best = distance;
                    ai = k;
                }
            }
        }
        cb |= (ci as u32) << (2 * i);
        ab |= (ai as u64) << (3 * i);
    }
    let mut result = [0; 16];
    result[0] = a0 as u8;
    result[1] = a1 as u8;
    result[2..8].copy_from_slice(&ab.to_le_bytes()[..6]);
    result[8..10].copy_from_slice(&c0.to_le_bytes());
    result[10..12].copy_from_slice(&c1.to_le_bytes());
    result[12..16].copy_from_slice(&cb.to_le_bytes());
    result
}
#[no_mangle]
pub extern "C" fn bc3_encode(width: usize, height: usize, input_len: usize) -> usize {
    if width == 0
        || height == 0
        || width > 8192
        || height > 8192
        || width * height > MAX_PIXELS
        || input_len != width * height * 4
    {
        return 0;
    }
    let size = width.div_ceil(4) * height.div_ceil(4) * 16;
    if size > OUTPUT_CAP {
        return 0;
    }
    let mut offset = 0;
    for y in (0..height).step_by(4) {
        for x in (0..width).step_by(4) {
            let mut pixels = [[0.0; 4]; 16];
            for row in 0..4 {
                for col in 0..4 {
                    let src = ((y + row).min(height - 1) * width + (x + col).min(width - 1)) * 4;
                    for (channel, value) in pixels[row * 4 + col].iter_mut().enumerate() {
                        *value = unsafe { *bc3_input().add(src + channel) } as f64;
                    }
                }
            }
            let encoded = block(&pixels);
            unsafe {
                std::ptr::copy_nonoverlapping(encoded.as_ptr(), bc3_output().add(offset), 16);
            }
            offset += 16;
        }
    }
    size
}
#[no_mangle]
pub extern "C" fn bc3_clear(input_len: usize, output_len: usize) {
    unsafe {
        std::ptr::write_bytes(bc3_input(), 0, input_len.min(MAX_PIXELS * 4));
        std::ptr::write_bytes(bc3_output(), 0, output_len.min(OUTPUT_CAP));
    }
}
