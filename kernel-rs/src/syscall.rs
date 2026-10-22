// ===========================================================================
//  syscall — системные вызовы: единственная дверь из ring 3 в ring 0
// ===========================================================================
//  Договор (common/myos.h): eax = номер, ebx/ecx/edx/esi/edi = аргументы,
//  ответ в eax. Каждый указатель проверяется user_ok — программа не может
//  попросить ядро «почитать» память ядра (как copy_from_user в Linux).
// ===========================================================================

use crate::abi::*;
use crate::disk::{self, PART_RESERVED, SHELL_SNAP, DEMO_SNAP};
use crate::input;
use crate::{log, task, video};

// --- Номера вызовов (зеркало common/myos.h) ---------------------------------
const SYS_EXIT: u32 = 0;
const SYS_KEY: u32 = 1;
const SYS_TIME: u32 = 2;
const SYS_TICKS: u32 = 3;
const SYS_PID: u32 = 4;
const SYS_FB: u32 = 5;
const SYS_PRINT: u32 = 6;
const SYS_LOG_READ: u32 = 7;
const SYS_FS_LIST: u32 = 8;
const SYS_FS_READ: u32 = 9;
const SYS_FS_READAT: u32 = 10;
const SYS_FS_WRITE: u32 = 11;
const SYS_FS_DELETE: u32 = 12;
const SYS_FS_RENAME: u32 = 13;
const SYS_FS_FORMAT: u32 = 14;
const SYS_PART_LIST: u32 = 15;
const SYS_PART_MK: u32 = 16;
const SYS_DISK_WTEST: u32 = 17;
const SYS_INSTALL_PLAN: u32 = 18;
const SYS_INSTALL: u32 = 19;
const SYS_BOOTPART: u32 = 20;
const SYS_SPAWN: u32 = 21;
const SYS_ALIVE: u32 = 22;
const SYS_INFO: u32 = 23;
const SYS_INPUT: u32 = 24;
const SYS_FS_LS: u32 = 25;
const SYS_FS_CAT: u32 = 26;
const SYS_PART_LS: u32 = 27;
const SYS_YIELD: u32 = 28;

const ERR_SYSFIL: u32 = (-9i32) as u32;
const EV_KEY: i32 = 1;
const EV_MOUSE: i32 = 2;

// --- Проверка «адрес лежит в памяти программ» (4..8 МБ) --------------------
fn user_ok(addr: u32, len: u32) -> bool {
    if addr < 0x0040_0000 {
        return false;
    }
    if len > 0x0040_0000 {
        return false;
    }
    if addr + len > 0x0080_0000 {
        return false;
    }
    true
}

/// Скопировать строку из памяти программы в буфер ядра (до max-1 символов).
unsafe fn copy_str_from_user(dst: &mut [u8], src: u32, max: usize) -> i32 {
    if !user_ok(src, 1) {
        return -1;
    }
    let mut n = 0usize;
    while n < max - 1 {
        if !user_ok(src + n as u32, 1) {
            return -1;
        }
        let c = core::ptr::read_volatile((src + n as u32) as *const u8);
        if c == 0 {
            break;
        }
        dst[n] = c;
        n += 1;
    }
    dst[n] = 0;
    n as i32
}

unsafe fn cstr_len_user(src: u32, max: u32) -> i32 {
    if !user_ok(src, 1) {
        return -1;
    }
    let mut n = 0u32;
    while n < max {
        if !user_ok(src + n, 1) {
            return -1;
        }
        if core::ptr::read_volatile((src + n) as *const u8) == 0 {
            return n as i32;
        }
        n += 1;
    }
    -1
}

/// Мостик из ассемблера: кадр лежит в порядке из arch::isr_syscall.
#[no_mangle]
pub unsafe extern "C" fn syscall_dispatch(regs: *mut u32) {
    let num = regs.add(11).read(); // EAX — номер вызова
    let a1 = regs.add(8).read(); // EBX
    let a2 = regs.add(10).read(); // ECX
    let a3 = regs.add(9).read(); // EDX
    let a4 = regs.add(5).read(); // ESI
    let a5 = regs.add(4).read(); // EDI
    regs.add(11).write(dispatch(num, a1, a2, a3, a4, a5));
}

unsafe fn dispatch(num: u32, a1: u32, a2: u32, a3: u32, a4: u32, a5: u32) -> u32 {
    match num {
        SYS_EXIT => {
            log::puts(b"ring3: back in the kernel (ring 0).\n");
            task::exit(task::pid());
            0
        }
        SYS_KEY => {
            if task::pid() != task::input_owner() {
                return u32::MAX;
            }
            input::poll_key() as u32
        }
        SYS_TIME => arch_day_secs(),
        SYS_TICKS => crate::arch::millis(),
        SYS_PID => task::pid() as u32,
        SYS_FB => {
            if !user_ok(a1, core::mem::size_of::<MyosFb>() as u32) {
                return u32::MAX;
            }
            let p = a1 as *mut MyosFb;
            (*p).addr = video::fb_addr();
            (*p).w = video::width();
            (*p).h = video::height();
            (*p).bpp = video_bpp();
            (*p).pitch = video_pitch();
            (*p).rpos = video_rpos();
            (*p).gpos = video_gpos();
            (*p).bpos = video_bpos();
            0
        }
        SYS_PRINT => {
            let mut buf = [0u8; 256];
            let n = copy_str_from_user(&mut buf, a1, 256);
            if n < 0 {
                return u32::MAX;
            }
            log::puts(&buf[..n as usize]);
            n as u32
        }
        SYS_LOG_READ => {
            if !user_ok(a1, 1) {
                return u32::MAX;
            }
            let mut max = a2;
            let room = 0x0080_0000u32.wrapping_sub(a1);
            if max > room {
                max = room;
            }
            if max > 4096 {
                max = 4096;
            }
            log::read(a1 as *mut u8, max)
        }
        SYS_FS_LIST => {
            let mut max = a3 as i32;
            if max > disk::FS_FILES_MAX as i32 {
                max = disk::FS_FILES_MAX as i32;
            }
            if max <= 0
                || !user_ok(a2, max as u32 * core::mem::size_of::<MyosFile>() as u32)
            {
                return u32::MAX;
            }
            let cnt = disk::fs_load_table(a1 as i32);
            if cnt < 0 {
                return cnt as u32;
            }
            let mut n = cnt;
            if n > max {
                n = max;
            }
            let out = a2 as *mut MyosFile;
            for i in 0..n as usize {
                let f = disk::file_at(i);
                let dst = &mut (*out.add(i)).name;
                dst.fill(0);
                let mut k = 0usize;
                while k < 55 && f.name[k] != 0 {
                    dst[k] = f.name[k];
                    k += 1;
                }
                (*out.add(i)).start = f.start;
                (*out.add(i)).size = f.size;
            }
            n as u32
        }
        SYS_FS_READ => {
            let mut nm = [0u8; 64];
            if copy_str_from_user(&mut nm, a2, 64) < 0 {
                return u32::MAX;
            }
            if !user_ok(a3, 1) {
                return u32::MAX;
            }
            let mut max = a4;
            let room = 0x0080_0000u32.wrapping_sub(a3);
            if max > room {
                max = room;
            }
            disk::fs_read_by_name(a1 as i32, cstr(&nm), a3 as *mut u8, max) as u32
        }
        SYS_FS_READAT => {
            if !user_ok(a3, 1) {
                return u32::MAX;
            }
            let mut max = a4;
            let room = 0x0080_0000u32.wrapping_sub(a3);
            if max > room {
                max = room;
            }
            disk::fs_read_at(a1, a2, a3 as *mut u8, max) as u32
        }
        SYS_FS_WRITE => {
            let mut nm = [0u8; 64];
            if copy_str_from_user(&mut nm, a2, 64) < 0 {
                return u32::MAX;
            }
            let quiet = a5 != 0;
            if (a4 as i32) < 0 {
                let len = cstr_len_user(a3, 0x10_0000);
                if len < 0 {
                    return u32::MAX;
                }
                return disk::fs_write_buf(a1 as i32, cstr(&nm), a3 as *const u8, len as u32, quiet)
                    as u32;
            }
            if !user_ok(a3, a4) {
                return u32::MAX;
            }
            disk::fs_write_buf(a1 as i32, cstr(&nm), a3 as *const u8, a4, quiet) as u32
        }
        SYS_FS_DELETE => {
            let mut nm = [0u8; 64];
            if copy_str_from_user(&mut nm, a2, 64) < 0 {
                return u32::MAX;
            }
            if disk::is_sys_file(cstr(&nm)) {
                return ERR_SYSFIL;
            }
            disk::fs_delete(a1 as i32, cstr(&nm)) as u32
        }
        SYS_FS_RENAME => {
            let mut old = [0u8; 64];
            let mut new = [0u8; 64];
            if copy_str_from_user(&mut old, a2, 64) < 0 {
                return u32::MAX;
            }
            if copy_str_from_user(&mut new, a3, 64) < 0 {
                return u32::MAX;
            }
            if disk::is_sys_file(cstr(&old)) || disk::is_sys_file(cstr(&new)) {
                return ERR_SYSFIL;
            }
            disk::fs_rename(a1 as i32, cstr(&old), cstr(&new)) as u32
        }
        SYS_FS_FORMAT => {
            let r = disk::fs_format(a1 as i32, a2 != 0);
            if r != 0 {
                return r as u32;
            }
            if a1 as i32 == read_sys_part() {
                disk::restore_sys_files(a1 as i32);
            }
            0
        }
        SYS_PART_LIST => {
            if !user_ok(a1, 4 * core::mem::size_of::<MyosPart>() as u32) {
                return u32::MAX;
            }
            disk::part_list(a1 as *mut MyosPart) as u32
        }
        SYS_PART_MK => {
            disk::cmd_mkpart(a1 as i32, a2 as i32);
            0
        }
        SYS_DISK_WTEST => {
            disk::cmd_wtest();
            0
        }
        SYS_INSTALL_PLAN => {
            if !user_ok(a2, core::mem::size_of::<MyosInstall>() as u32) {
                return u32::MAX;
            }
            let out = a2 as *mut MyosInstall;
            (*out).lba = 0;
            (*out).sectors = 0;
            (*out).kern_sectors = disk::kernel_sectors();
            match disk::install_check(a1 as i32) {
                None => {
                    (*out).code = -1;
                    u32::MAX
                }
                Some((plba, psize)) => {
                    (*out).lba = plba;
                    (*out).sectors = psize;
                    (*out).code = 0;
                    0
                }
            }
        }
        SYS_INSTALL => {
            let (plba, psize) = match disk::install_check(a1 as i32) {
                Some(v) => v,
                None => return u32::MAX,
            };
            if a2 & 2 == 0 {
                disk::install_plan(a1 as i32, plba, psize);
            }
            if a2 & 1 == 0 {
                log::puts(b"Type: install ");
                log::putc(b'0' + a1 as u8);
                log::puts(b" yes\n");
                return 0;
            }
            disk::install_do(a1 as i32, plba, a2 & 2 == 0);
            disk::ensure_sys_files(a1 as i32);
            0
        }
        SYS_BOOTPART => {
            if !user_ok(a1, core::mem::size_of::<MyosBoot>() as u32) {
                return u32::MAX;
            }
            let (idx, lba) = disk::boot_partition();
            let b = a1 as *mut MyosBoot;
            (*b).index = idx;
            (*b).lba = lba;
            idx as u32
        }
        SYS_SPAWN => {
            let mut nm = [0u8; 64];
            if copy_str_from_user(&mut nm, a1, 64) < 0 {
                return u32::MAX;
            }
            if a2 < 0x0040_0000 || a2 > 0x0080_0000 - 0x1_0000 {
                return u32::MAX;
            }
            let sp = read_sys_part();
            if sp == 0 {
                return u32::MAX;
            }
            // Зона программы — чистая (как mmap со свежими страницами).
            let mut i = 0u32;
            while i < 0x10_0000 {
                core::ptr::write_volatile((a2 + i) as *mut u32, 0);
                i += 4;
            }
            let len = disk::fs_read_by_name(sp, cstr(&nm), a2 as *mut u8, 0x10_0000);
            if len <= 0 {
                return u32::MAX;
            }
            log::puts(b"ring3: entering user mode...\n");
            let pid = task::create(cstr(&nm), a2, a2 + 0xFFFF0, task::pid());
            if pid < 0 {
                return u32::MAX;
            }
            task::set_input_owner(pid);
            pid as u32
        }
        SYS_ALIVE => {
            if task::alive(a1 as i32) {
                1
            } else {
                0
            }
        }
        SYS_INFO => {
            if a1 == 0 {
                disk::kernel_sectors()
            } else if a1 == 1 {
                PART_RESERVED
            } else {
                0
            }
        }
        SYS_INPUT => {
            let mut max = a2;
            if max > 64 {
                max = 64;
            }
            if max == 0 {
                return 0;
            }
            if !user_ok(a1, max * core::mem::size_of::<MyosEvent>() as u32) {
                return u32::MAX;
            }
            let out = a1 as *mut MyosEvent;
            let owner = task::pid() == task::input_owner();
            let mut cnt = 0u32;
            while cnt < max {
                if owner {
                    let sc = input::poll_key();
                    if sc >= 0 {
                        (*out.add(cnt as usize)).type_ = EV_KEY;
                        (*out.add(cnt as usize)).d1 = sc;
                        (*out.add(cnt as usize)).d2 = 0;
                        (*out.add(cnt as usize)).d3 = 0;
                        cnt += 1;
                        continue;
                    }
                }
                if let Some((dx, dy, btn)) = input::mouse_poll() {
                    (*out.add(cnt as usize)).type_ = EV_MOUSE;
                    (*out.add(cnt as usize)).d1 = dx;
                    (*out.add(cnt as usize)).d2 = dy;
                    (*out.add(cnt as usize)).d3 = btn;
                    cnt += 1;
                    continue;
                }
                break;
            }
            cnt
        }
        SYS_FS_LS => {
            disk::fs_ls(a1 as i32);
            0
        }
        SYS_FS_CAT => {
            let mut nm = [0u8; 64];
            if copy_str_from_user(&mut nm, a2, 64) < 0 {
                return u32::MAX;
            }
            disk::fs_cat(a1 as i32, cstr(&nm));
            0
        }
        SYS_PART_LS => {
            disk::cmd_part();
            0
        }
        SYS_YIELD => {
            task::schedule_now();
            0
        }
        _ => u32::MAX,
    }
}

fn cstr(buf: &[u8]) -> &[u8] {
    let mut n = 0;
    while n < buf.len() && buf[n] != 0 {
        n += 1;
    }
    &buf[..n]
}

fn read_sys_part() -> i32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(disk::SYS_PART)) }
}

fn arch_day_secs() -> u32 {
    crate::arch::day_secs()
}

// Аксессоры видеорежима (скрывают статики video от syscall).
fn video_bpp() -> i32 {
    crate::video::bpp()
}
fn video_pitch() -> i32 {
    crate::video::pitch()
}
fn video_rpos() -> i32 {
    crate::video::rpos()
}
fn video_gpos() -> i32 {
    crate::video::gpos()
}
fn video_bpos() -> i32 {
    crate::video::bpos()
}

// Эта функция не нужна на уровне ABI, но подчёркивает владение снимками.
#[allow(dead_code)]
fn snapshots() -> (u32, u32) {
    (SHELL_SNAP, DEMO_SNAP)
}
