// ===========================================================================
//  disk — ATA PIO, разделы MBR, MyFS, установка системы
// ===========================================================================
//  В защищённом режиме BIOS (int 0x13) недоступен, поэтому говорим с диском
//  напрямую через порты ATA-контроллера 0x1F0..0x1F7 (PIO, primary master).
//
//  MyFS: суперблок (+0), таблица файлов (+1), данные (+2). Файлы хранятся
//  целиком подряд — для учебной ОС достаточно, метаданные отдельно от данных.
//  Первые PART_RESERVED секторов любого раздела — зона загрузки (VBR,
//  stage2, ядро); MyFS начинается после неё, поэтому install и format
//  не мешают друг другу.
// ===========================================================================

use crate::abi::MyosPart;
use crate::{arch, log};

pub const PART_RESERVED: u32 = 192;
pub const FS_FILES_MAX: usize = 8;
const DISK_SECTORS: u32 = 65536; // образ 32 МБ

const ATA_IO: u16 = 0x1F0;
const ATA_CNT: u16 = 0x1F2;
const ATA_LBA0: u16 = 0x1F3;
const ATA_LBA1: u16 = 0x1F4;
const ATA_LBA2: u16 = 0x1F5;
const ATA_DRV: u16 = 0x1F6;
const ATA_CMD: u16 = 0x1F7;
const ATA_ALT: u16 = 0x3F6;

// --- Буферы секторов (глобальные — не держим 512 байт на стеке) -------------
static mut DISK_BUF: [u8; 512] = [0; 512];
static mut SEC_BUF: [u8; 512] = [0; 512];

unsafe fn dbuf() -> *mut u8 {
    core::ptr::addr_of_mut!(DISK_BUF) as *mut u8
}
unsafe fn sbuf() -> *mut u8 {
    core::ptr::addr_of_mut!(SEC_BUF) as *mut u8
}
unsafe fn rd16(p: *const u8, off: usize) -> u16 {
    (p.add(off) as *const u16).read_unaligned()
}
unsafe fn rd32(p: *const u8, off: usize) -> u32 {
    (p.add(off) as *const u32).read_unaligned()
}
unsafe fn wr16(p: *mut u8, off: usize, v: u16) {
    (p.add(off) as *mut u16).write_unaligned(v)
}
unsafe fn wr32(p: *mut u8, off: usize, v: u32) {
    (p.add(off) as *mut u32).write_unaligned(v)
}

fn pr(s: &[u8]) {
    log::puts(s);
}
fn pc(c: u8) {
    log::putc(c);
}
fn pnum(v: u32) {
    log::print_dec(v);
}
fn phex(v: u8) {
    log::print_hex8(v);
}

// --- ATA PIO -----------------------------------------------------------------
unsafe fn wait_bsy() {
    for _ in 0..1_000_000 {
        if arch::inb(ATA_CMD) & 0x80 == 0 {
            return;
        }
    }
}
unsafe fn wait_drq() {
    for _ in 0..1_000_000 {
        if arch::inb(ATA_CMD) & 0x08 != 0 {
            return;
        }
    }
}
unsafe fn delay() {
    for _ in 0..4 {
        let _ = arch::inb(ATA_ALT);
    }
}

pub unsafe fn read_sector(lba: u32, buf: *mut u8) {
    wait_bsy();
    arch::outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F) as u8);
    delay();
    arch::outb(ATA_CNT, 1);
    arch::outb(ATA_LBA0, (lba & 0xFF) as u8);
    arch::outb(ATA_LBA1, ((lba >> 8) & 0xFF) as u8);
    arch::outb(ATA_LBA2, ((lba >> 16) & 0xFF) as u8);
    arch::outb(ATA_CMD, 0x20); // READ SECTORS
    wait_bsy();
    wait_drq();
    for i in 0..256 {
        let w = arch::inw(ATA_IO);
        (buf.add(i * 2) as *mut u16).write_unaligned(w);
    }
}

pub unsafe fn write_sector(lba: u32, buf: *const u8) {
    wait_bsy();
    arch::outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F) as u8);
    delay();
    arch::outb(ATA_CNT, 1);
    arch::outb(ATA_LBA0, (lba & 0xFF) as u8);
    arch::outb(ATA_LBA1, ((lba >> 8) & 0xFF) as u8);
    arch::outb(ATA_LBA2, ((lba >> 16) & 0xFF) as u8);
    arch::outb(ATA_CMD, 0x30); // WRITE SECTORS
    wait_bsy();
    wait_drq();
    for i in 0..256 {
        let w = (buf.add(i * 2) as *const u16).read_unaligned();
        arch::outw(ATA_IO, w);
    }
    arch::outb(ATA_CMD, 0xE7); // FLUSH CACHE
    wait_bsy();
}

// --- MBR ---------------------------------------------------------------------
/// Найти раздел N: (LBA, размер в секторах). Читает сектор 0 в disk_buf.
pub unsafe fn find_part(n: i32) -> Option<(u32, u32)> {
    if n < 1 || n > 4 {
        return None;
    }
    read_sector(0, dbuf());
    let d = dbuf();
    if *d.add(510) != 0x55 || *d.add(511) != 0xAA {
        return None;
    }
    let e = d.add(446 + (n as usize - 1) * 16);
    if *e.add(4) == 0 {
        return None;
    }
    let lba = rd32(e, 8);
    let size = rd32(e, 12);
    Some((lba, size))
}

unsafe fn mbr_ok() -> bool {
    let d = dbuf();
    *d.add(510) == 0x55 && *d.add(511) == 0xAA
}

// --- MyFS: суперблок и таблица ----------------------------------------------
fn is_myfs() -> bool {
    unsafe {
        let d = dbuf();
        *d == b'M' && *d.add(1) == b'Y' && *d.add(2) == b'F' && *d.add(3) == b'S'
    }
}

#[derive(Clone, Copy)]
pub struct FileInfo {
    pub name: [u8; 56],
    pub start: u32,
    pub size: u32,
}

static mut FILE_LIST: [FileInfo; FS_FILES_MAX] = [FileInfo {
    name: [0; 56],
    start: 0,
    size: 0,
}; FS_FILES_MAX];

pub fn file_at(i: usize) -> FileInfo {
    unsafe { core::ptr::read(core::ptr::addr_of!(FILE_LIST).cast::<FileInfo>().add(i)) }
}

unsafe fn entry_ptr(i: usize) -> *const u8 {
    sbuf().add(i * 64)
}

unsafe fn entry_empty(i: usize) -> bool {
    *entry_ptr(i) == 0
}

unsafe fn entry_start(i: usize) -> u32 {
    rd32(entry_ptr(i), 56)
}

unsafe fn entry_size(i: usize) -> u32 {
    rd32(entry_ptr(i), 60)
}

unsafe fn entry_name_eq(i: usize, name: &[u8]) -> bool {
    let p = entry_ptr(i);
    let mut k = 0usize;
    while k < 56 {
        let a = *p.add(k);
        let b = if k < name.len() { name[k] } else { 0 };
        if a != b {
            return false;
        }
        if a == 0 {
            return true;
        }
        k += 1;
    }
    false
}

unsafe fn print_entry_name(i: usize) {
    let p = entry_ptr(i);
    for k in 0..56 {
        let b = *p.add(k);
        if b == 0 {
            break;
        }
        pc(b);
    }
}

/// Загрузить таблицу файлов раздела n в file_list.
/// Возврат: сколько файлов (>0), 0 — пусто, -1 — нет раздела, -2 — не MyFS.
pub unsafe fn fs_load_table(n: i32) -> i32 {
    let (plba, _psize) = match find_part(n) {
        Some(v) => v,
        None => return -1,
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        return -2;
    }
    let table = rd32(dbuf(), 10);
    read_sector(table, sbuf());
    let list = core::ptr::addr_of_mut!(FILE_LIST) as *mut FileInfo;
    let mut cnt = 0usize;
    for i in 0..FS_FILES_MAX {
        if entry_empty(i) {
            continue;
        }
        let dst = (list.add(cnt) as *mut u8).cast::<u8>();
        core::ptr::copy_nonoverlapping(entry_ptr(i), dst, 56);
        (*list.add(cnt)).start = entry_start(i);
        (*list.add(cnt)).size = entry_size(i);
        cnt += 1;
    }
    cnt as i32
}

/// Прочитать size байт с LBA start в буфер dst (не больше max).
pub unsafe fn fs_read_at(start: u32, mut size: u32, dst: *mut u8, max: u32) -> i32 {
    if size > max {
        size = max;
    }
    let mut got = 0u32;
    let mut lba = start;
    while got < size {
        read_sector(lba, dbuf());
        let mut chunk = size - got;
        if chunk > 512 {
            chunk = 512;
        }
        core::ptr::copy_nonoverlapping(dbuf(), dst.add(got as usize), chunk as usize);
        got += chunk;
        lba += 1;
    }
    got as i32
}

/// Прочитать файл по имени из раздела n в буфер dst. Возврат: байт или -1.
pub unsafe fn fs_read_by_name(n: i32, name: &[u8], dst: *mut u8, max: u32) -> i32 {
    let cnt = fs_load_table(n);
    if cnt < 0 {
        return -1;
    }
    for i in 0..cnt as usize {
        let f = file_at(i);
        let mut k = 0usize;
        let mut eq = true;
        while k < 56 && f.name[k] != 0 {
            if k >= name.len() || f.name[k] != name[k] {
                eq = false;
                break;
            }
            k += 1;
        }
        if eq && k == name.len() {
            return fs_read_at(f.start, f.size, dst, max);
        }
    }
    -1
}

// --- MyFS: операции ----------------------------------------------------------
pub unsafe fn fs_format(n: i32, quiet: bool) -> i32 {
    let (plba, psize) = match find_part(n) {
        Some(v) => v,
        None => {
            if !quiet {
                pr(b"No such partition\n");
            }
            return -1;
        }
    };
    core::ptr::write_bytes(dbuf(), 0, 512);
    let d = dbuf();
    *d = b'M';
    *d.add(1) = b'Y';
    *d.add(2) = b'F';
    *d.add(3) = b'S';
    wr16(d, 4, 1);
    wr32(d, 6, psize);
    wr32(d, 10, plba + PART_RESERVED + 1);
    wr32(d, 14, plba + PART_RESERVED + 2);
    wr32(d, 18, plba + PART_RESERVED + 2);
    wr16(d, 22, 0);
    wr16(d, 24, FS_FILES_MAX as u16);
    write_sector(plba + PART_RESERVED, d);

    core::ptr::write_bytes(dbuf(), 0, 512);
    write_sector(plba + PART_RESERVED + 1, dbuf());
    if !quiet {
        pr(b"Formatted P");
        pc(b'0' + n as u8);
        pr(b" as MyFS\n");
    }
    0
}

pub unsafe fn fs_ls(n: i32) {
    let (plba, _psize) = match find_part(n) {
        Some(v) => v,
        None => {
            pr(b"No such partition\n");
            return;
        }
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        pr(b"Not MyFS (format it first)\n");
        return;
    }
    let table = rd32(dbuf(), 10);
    read_sector(table, sbuf());
    pr(b"MyFS on P");
    pc(b'0' + n as u8);
    pr(b":\n");
    let mut shown = 0;
    for i in 0..FS_FILES_MAX {
        if entry_empty(i) {
            continue;
        }
        pr(b"  ");
        print_entry_name(i);
        pr(b"  ");
        pnum(entry_size(i));
        pr(b" bytes\n");
        shown += 1;
    }
    if shown == 0 {
        pr(b"  (empty)\n");
    }
}

/// Записать ДВОИЧНЫЕ данные (ровно len байт) в файл name раздела N.
pub unsafe fn fs_write_buf(
    n: i32,
    name: &[u8],
    data: *const u8,
    len: u32,
    quiet: bool,
) -> i32 {
    let (plba, psize) = match find_part(n) {
        Some(v) => v,
        None => {
            if !quiet {
                pr(b"No such partition\n");
            }
            return -1;
        }
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        if !quiet {
            pr(b"Not MyFS (format it first)\n");
        }
        return -2;
    }
    let table = rd32(dbuf(), 10);
    let dptr = rd32(dbuf(), 18);
    read_sector(table, sbuf());

    let mut free_idx: i32 = -1;
    for i in 0..FS_FILES_MAX {
        if entry_empty(i) {
            if free_idx < 0 {
                free_idx = i as i32;
            }
            continue;
        }
        if entry_name_eq(i, name) {
            if !quiet {
                pr(b"File exists\n");
            }
            return -3;
        }
    }
    if free_idx < 0 {
        if !quiet {
            pr(b"File table full\n");
        }
        return -4;
    }
    let need = (len + 511) / 512;
    if dptr + need > plba + psize {
        if !quiet {
            pr(b"No space on partition\n");
        }
        return -5;
    }

    // Суперблок обновляем сразу, пока disk_buf держит его в руках.
    wr32(dbuf(), 18, dptr + need);
    wr16(dbuf(), 22, rd16(dbuf(), 22).wrapping_add(1));
    write_sector(plba + PART_RESERVED, dbuf());

    // Данные: каждые 512 байт -> один сектор.
    let mut s = 0u32;
    while s < need {
        core::ptr::write_bytes(dbuf(), 0, 512);
        let base = s * 512;
        let mut i = 0u32;
        while i < 512 && base + i < len {
            *dbuf().add(i as usize) = *data.add((base + i) as usize);
            i += 1;
        }
        write_sector(dptr + s, dbuf());
        s += 1;
    }

    // Запись в таблице файлов.
    let f = sbuf().add(free_idx as usize * 64);
    core::ptr::write_bytes(f, 0, 56);
    let mut k = 0usize;
    while k < name.len() && k < 55 {
        *f.add(k) = name[k];
        k += 1;
    }
    wr32(f, 56, dptr);
    wr32(f, 60, len);
    write_sector(table, sbuf());

    if !quiet {
        pr(b"Written ");
        pr(name);
        pr(b", ");
        pnum(len);
        pr(b" bytes\n");
    }
    0
}

/// Удалить файл; данные «уплотняются» (файлы после него сдвигаются вниз).
pub unsafe fn fs_delete(n: i32, name: &[u8]) -> i32 {
    let (plba, _psize) = match find_part(n) {
        Some(v) => v,
        None => return -1,
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        return -2;
    }
    let table = rd32(dbuf(), 10);
    let dptr = rd32(dbuf(), 18);
    let fcnt = rd16(dbuf(), 22);
    read_sector(table, sbuf());

    let mut vi: i32 = -1;
    for i in 0..FS_FILES_MAX {
        if !entry_empty(i) && entry_name_eq(i, name) {
            vi = i as i32;
            break;
        }
    }
    if vi < 0 {
        return -3;
    }
    let vi = vi as usize;
    let vstart = entry_start(vi);
    let vsec = (entry_size(vi) + 511) / 512;

    // Сдвигаем данные всех файлов, лежавших после удалённого.
    for i in 0..FS_FILES_MAX {
        if i == vi || entry_empty(i) {
            continue;
        }
        let fstart = entry_start(i);
        if fstart <= vstart {
            continue;
        }
        if vsec > 0 {
            let fs = (entry_size(i) + 511) / 512;
            let mut s = 0u32;
            while s < fs {
                read_sector(fstart + s, dbuf());
                write_sector(fstart + s - vsec, dbuf());
                s += 1;
            }
            wr32(entry_ptr(i) as *mut u8, 56, fstart - vsec);
        }
    }

    // Запись удалена.
    core::ptr::write_bytes(sbuf().add(vi * 64), 0, 64);
    write_sector(table, sbuf());

    // Суперблок читаем заново после движений.
    read_sector(plba + PART_RESERVED, dbuf());
    wr32(dbuf(), 18, dptr - vsec);
    wr16(dbuf(), 22, fcnt.wrapping_sub(1));
    write_sector(plba + PART_RESERVED, dbuf());
    0
}

pub unsafe fn fs_rename(n: i32, oldname: &[u8], newname: &[u8]) -> i32 {
    let (plba, _psize) = match find_part(n) {
        Some(v) => v,
        None => return -1,
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        return -2;
    }
    let table = rd32(dbuf(), 10);
    read_sector(table, sbuf());

    let mut found: i32 = -1;
    for i in 0..FS_FILES_MAX {
        if entry_empty(i) {
            continue;
        }
        if entry_name_eq(i, newname) && !entry_name_eq(i, oldname) {
            return -4;
        }
        if entry_name_eq(i, oldname) {
            found = i as i32;
        }
    }
    if found < 0 {
        return -3;
    }
    let f = sbuf().add(found as usize * 64);
    core::ptr::write_bytes(f, 0, 56);
    let mut k = 0usize;
    while k < newname.len() && k < 55 {
        *f.add(k) = newname[k];
        k += 1;
    }
    write_sector(table, sbuf());
    0
}

pub unsafe fn fs_cat(n: i32, name: &[u8]) {
    let (plba, _psize) = match find_part(n) {
        Some(v) => v,
        None => {
            pr(b"No such partition\n");
            return;
        }
    };
    read_sector(plba + PART_RESERVED, dbuf());
    if !is_myfs() {
        pr(b"Not MyFS (format it first)\n");
        return;
    }
    let table = rd32(dbuf(), 10);
    read_sector(table, sbuf());

    let mut found: i32 = -1;
    for i in 0..FS_FILES_MAX {
        if !entry_empty(i) && entry_name_eq(i, name) {
            found = i as i32;
            break;
        }
    }
    if found < 0 {
        pr(b"No such file\n");
        return;
    }
    let mut left = entry_size(found as usize);
    let mut lba = entry_start(found as usize);
    while left > 0 {
        read_sector(lba, dbuf());
        let chunk = if left < 512 { left } else { 512 };
        for i in 0..chunk as usize {
            pc(*dbuf().add(i));
        }
        left -= chunk;
        lba += 1;
    }
    pc(b'\n');
}

// --- Разделы -----------------------------------------------------------------
/// Таблица разделов для мастера установки (SYS_PART_LIST). Возврат: занятых.
pub unsafe fn part_list(out: *mut MyosPart) -> i32 {
    read_sector(0, dbuf());
    if !mbr_ok() {
        pr(b"No MBR signature (disk is not ATA?)\n");
        return 0;
    }
    let ks = kernel_sectors();
    let mut cnt = 0;
    for i in 0..4usize {
        let e = dbuf().add(446 + i * 16);
        let dst = out.add(i);
        (*dst).lba = 0;
        (*dst).sectors = 0;
        (*dst).flags = 0;
        if *e.add(4) == 0 {
            continue;
        }
        let plba = rd32(e, 8);
        let psize = rd32(e, 12);
        let mut fl = 1u32; // слот занят
        if *e == 0x80 {
            fl |= 8; // активный
        }
        if psize >= PART_RESERVED + 3 && ks + 3 <= PART_RESERVED {
            fl |= 4; // можно ставить
        }
        // Проверяем MyFS, не портя диск: читаем суперблок в sec_buf.
        *sbuf() = 0;
        read_sector(plba + PART_RESERVED, sbuf());
        if *sbuf() == b'M'
            && *sbuf().add(1) == b'Y'
            && *sbuf().add(2) == b'F'
            && *sbuf().add(3) == b'S'
        {
            fl |= 2;
        }
        (*dst).lba = plba;
        (*dst).sectors = psize;
        (*dst).flags = fl;
        cnt += 1;
    }
    cnt
}

pub unsafe fn cmd_part() {
    read_sector(0, dbuf());
    if !mbr_ok() {
        pr(b"No MBR signature (disk is not ATA?)\n");
        return;
    }
    pr(b"MBR partitions:\n");
    for i in 0..4usize {
        let e = dbuf().add(446 + i * 16);
        if *e.add(4) == 0 {
            continue;
        }
        let lba = rd32(e, 8);
        let sec = rd32(e, 12);
        pr(b"  P");
        pc(b'1' + i as u8);
        pr(b" type=");
        phex(*e.add(4));
        pr(b" lba=");
        pnum(lba);
        pr(b" size=");
        pnum(sec / 2048); // 2048 секторов = 1 МБ
        pr(b"MB\n");
    }
}

pub unsafe fn cmd_wtest() {
    const LBA: u32 = 60000; // свободный сектор после разделов
    pr(b"Write test @ LBA ");
    pnum(LBA);
    pr(b"...\n");
    for i in 0..512 {
        *dbuf().add(i) = (i as u8).wrapping_mul(7).wrapping_add(13);
    }
    write_sector(LBA, dbuf());
    core::ptr::write_bytes(dbuf(), 0, 512);
    read_sector(LBA, dbuf());
    let mut bad: i32 = -1;
    for i in 0..512 {
        let want = (i as u8).wrapping_mul(7).wrapping_add(13);
        if *dbuf().add(i) != want {
            bad = i as i32;
            break;
        }
    }
    if bad < 0 {
        pr(b"  OK - 512 bytes match\n");
    } else {
        pr(b"  FAIL at byte ");
        pnum(bad as u32);
        pr(b"\n");
    }
}

/// Создать (или изменить) раздел N размером MB мегабайт.
/// Пишем ТОЛЬКО байты 446..509 сектора 0 — код загрузчика не трогаем.
pub unsafe fn cmd_mkpart(n: i32, mb: i32) {
    if n < 1 || n > 4 {
        pr(b"Usage: mkpart N MB  (N=1..4)\n");
        return;
    }
    if mb < 1 || mb > 32 {
        pr(b"Size must be 1..32 MB\n");
        return;
    }
    read_sector(0, dbuf());
    if !mbr_ok() {
        pr(b"No MBR signature\n");
        return;
    }
    let e = dbuf().add(446 + (n as usize - 1) * 16);
    let start: u32;
    if *e.add(4) != 0 {
        start = rd32(e, 8);
    } else {
        let mut s = 2048u32;
        for i in 0..4usize {
            let o = dbuf().add(446 + i * 16);
            if *o.add(4) == 0 {
                continue;
            }
            let olba = rd32(o, 8);
            let osec = rd32(o, 12);
            if olba + osec > s {
                s = olba + osec;
            }
        }
        start = ((s + 2047) / 2048) * 2048;
    }
    let size = (mb as u32) * 2048;
    if start + size > DISK_SECTORS {
        pr(b"No space: only ");
        pnum((DISK_SECTORS - start) / 2048);
        pr(b"MB free\n");
        return;
    }
    if *e.add(4) == 0 {
        *e = 0; // новый раздел — не загрузочный
    }
    *e.add(1) = 0;
    *e.add(2) = 0;
    *e.add(3) = 0;
    *e.add(4) = 0x83;
    *e.add(5) = 0;
    *e.add(6) = 0;
    *e.add(7) = 0;
    wr32(e, 8, start);
    wr32(e, 12, size);
    write_sector(0, dbuf());
    pr(b"Created P");
    pc(b'0' + n as u8);
    pr(b": lba=");
    pnum(start);
    pr(b" size=");
    pnum(mb as u32);
    pr(b"MB\n");
    cmd_part();
}

// --- Установка системы в раздел ---------------------------------------------
//  install копирует систему из НАЧАЛА диска в служебную зону раздела:
//    сектор 1 (VBR) -> +0, сектора 2-3 (stage2) -> +1..2, ядро -> +3...
//  В копию VBR впечатывается LBA раздела (поле +504). Без подтверждения
//  ничего не пишем — сначала план.
extern "C" {
    #[link_name = "_image_end"]
    static IMAGE_END: u8;
}

/// Сколько секторов занимает ядро на диске (по концу его образа).
pub fn kernel_sectors() -> u32 {
    let end = unsafe { core::ptr::addr_of!(IMAGE_END) as usize as u32 };
    let len = end.saturating_sub(0x10000);
    (len + 511) / 512 + 1
}

/// Проверить раздел для установки; при отказе печатает причину.
pub unsafe fn install_check(n: i32) -> Option<(u32, u32)> {
    let (plba, psize) = match find_part(n) {
        Some(v) => v,
        None => {
            pr(b"No such partition\n");
            return None;
        }
    };
    if psize < PART_RESERVED + 3 {
        pr(b"Partition too small\n");
        return None;
    }
    if kernel_sectors() + 3 > PART_RESERVED {
        pr(b"Kernel too big for boot area\n");
        return None;
    }
    Some((plba, psize))
}

pub unsafe fn install_plan(n: i32, plba: u32, psize: u32) {
    pr(b"Install to P");
    pc(b'0' + n as u8);
    pr(b": lba=");
    pnum(plba);
    pr(b" size=");
    pnum(psize / 2048);
    pr(b"MB\n");
    pr(b"Boot area: first ");
    pnum(PART_RESERVED);
    pr(b" sec\n");
    pr(b"  boot 1 + stage2 2 + kernel ");
    pnum(kernel_sectors());
    pr(b"\n");
    pr(b"MyFS files stay untouched.\n");
}

pub unsafe fn install_do(n: i32, plba: u32, talk: bool) {
    if talk {
        pr(b"Installing to P");
        pc(b'0' + n as u8);
        pr(b"...\n");
    }

    // 1. VBR: сектор 1 диска + впечатать LBA раздела (поле +504).
    read_sector(1, dbuf());
    *dbuf().add(504) = (plba & 0xFF) as u8;
    *dbuf().add(505) = ((plba >> 8) & 0xFF) as u8;
    *dbuf().add(506) = ((plba >> 16) & 0xFF) as u8;
    *dbuf().add(507) = ((plba >> 24) & 0xFF) as u8;
    write_sector(plba, dbuf());
    if talk {
        pr(b"  VBR -> ");
        pnum(plba);
        pr(b"\n");
    }

    // 2. stage2 (2 сектора): сектора 2-3 диска -> +1..+2 раздела.
    read_sector(2, dbuf());
    write_sector(plba + 1, dbuf());
    read_sector(3, dbuf());
    write_sector(plba + 2, dbuf());
    if talk {
        pr(b"  stage2 2 sec -> ");
        pnum(plba + 1);
        pr(b"\n");
    }

    // 3. Ядро: сектора 4+ диска -> +3.. раздела (посекторно).
    let ks = kernel_sectors();
    let mut i = 0u32;
    while i < ks {
        read_sector(4 + i, dbuf());
        write_sector(plba + 3 + i, dbuf());
        i += 1;
    }
    if talk {
        pr(b"  kernel ");
        pnum(ks);
        pr(b" sec -> ");
        pnum(plba + 3);
        pr(b"\n");
    }

    // 4. Раздел N становится АКТИВНЫМ (флаг 0x80), остальные снимаем.
    read_sector(0, dbuf());
    for i in 0..4usize {
        let e = dbuf().add(446 + i * 16);
        if *e.add(4) == 0 {
            continue;
        }
        *e = if i == (n as usize - 1) { 0x80 } else { 0x00 };
    }
    write_sector(0, dbuf());

    if talk {
        pr(b"P");
        pc(b'0' + n as u8);
        pr(b" marked active. Done.\n");
    }
}

pub unsafe fn cmd_install(n: i32, confirm: bool) {
    let (plba, psize) = match install_check(n) {
        Some(v) => v,
        None => return,
    };
    install_plan(n, plba, psize);
    if !confirm {
        pr(b"Type: install ");
        pc(b'0' + n as u8);
        pr(b" yes\n");
        return;
    }
    install_do(n, plba, true);
}

// --- Загрузочный раздел и снимки файлов системы ------------------------------
pub static mut SYS_PART: i32 = 0;
pub const SHELL_SNAP: u32 = 0x100000;
pub const DEMO_SNAP: u32 = 0x200000;
pub static mut SHELL_SNAP_LEN: u32 = 0;
pub static mut DEMO_SNAP_LEN: u32 = 0;

/// Из какого раздела загрузились? Возврат: (1..4 | 0 | -1, LBA из записки).
pub unsafe fn boot_partition() -> (i32, u32) {
    if core::ptr::read_volatile(0x0502 as *const u16) != 0x4D59 {
        return (-1, 0);
    }
    let bootlba = core::ptr::read_volatile(0x0500 as *const u16) as u32;
    read_sector(0, dbuf());
    for i in 0..4usize {
        let e = dbuf().add(446 + i * 16);
        if *e.add(4) == 0 {
            continue;
        }
        if rd32(e, 8) == bootlba {
            return (i as i32 + 1, bootlba);
        }
    }
    (0, bootlba)
}

pub fn is_sys_file(name: &[u8]) -> bool {
    name == b"shell.bin" || name == b"demo.bin"
}

/// Есть ли на разделе n файл name?
pub unsafe fn part_has_file(n: i32, name: &[u8]) -> bool {
    let cnt = fs_load_table(n);
    if cnt <= 0 {
        return false;
    }
    for i in 0..cnt as usize {
        let f = file_at(i);
        let mut k = 0usize;
        while k < 56 && f.name[k] != 0 {
            if k >= name.len() || f.name[k] != name[k] {
                break;
            }
            k += 1;
        }
        if k == name.len() && (k >= 56 || f.name[k] == 0) {
            return true;
        }
    }
    false
}

/// Найти раздел, на котором есть файл name (перебор P1..P4). 0 — не нашли.
pub unsafe fn find_part_with_file(name: &[u8]) -> i32 {
    for n in 1..=4 {
        if part_has_file(n, name) {
            return n;
        }
    }
    0
}

/// Если раздел n — MyFS, положить на него файлы системы из снимков ОЗУ.
pub unsafe fn ensure_sys_files(n: i32) {
    let cnt = fs_load_table(n);
    if cnt < 0 {
        return;
    }
    let mut has_shell = false;
    let mut has_demo = false;
    for i in 0..cnt as usize {
        let f = file_at(i);
        let mut len = 0;
        while len < 56 && f.name[len] != 0 {
            len += 1;
        }
        let name = &f.name[..len];
        if name == b"shell.bin" {
            has_shell = true;
        } else if name == b"demo.bin" {
            has_demo = true;
        }
    }
    if !has_shell {
        let len = core::ptr::read_volatile(core::ptr::addr_of!(SHELL_SNAP_LEN));
        if len > 0 {
            fs_write_buf(n, b"shell.bin", SHELL_SNAP as *const u8, len, true);
        }
    }
    if !has_demo {
        let len = core::ptr::read_volatile(core::ptr::addr_of!(DEMO_SNAP_LEN));
        if len > 0 {
            fs_write_buf(n, b"demo.bin", DEMO_SNAP as *const u8, len, true);
        }
    }
}

/// Вернуть на раздел n файлы системы после format (если это раздел системы).
pub unsafe fn restore_sys_files(n: i32) {
    let len = core::ptr::read_volatile(core::ptr::addr_of!(SHELL_SNAP_LEN));
    if len > 0 {
        fs_write_buf(n, b"shell.bin", SHELL_SNAP as *const u8, len, true);
    }
    let dlen = core::ptr::read_volatile(core::ptr::addr_of!(DEMO_SNAP_LEN));
    if dlen > 0 {
        fs_write_buf(n, b"demo.bin", DEMO_SNAP as *const u8, dlen, true);
    }
}
