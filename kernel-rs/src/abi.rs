// ===========================================================================
//  abi — структуры «стыка» ядра и оболочки (зеркало common/myos.h)
// ===========================================================================
//  Раскладка обязана совпадать с C++-заголовком до байта: оболочка и ядро —
//  разные сборки, и они общаются только этими структурами через int 0x80.
// ===========================================================================

#[repr(C)]
pub struct MyosFb {
    pub addr: u32,
    pub w: i32,
    pub h: i32,
    pub bpp: i32,
    pub pitch: i32,
    pub rpos: i32,
    pub gpos: i32,
    pub bpos: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MyosPart {
    pub lba: u32,
    pub sectors: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct MyosMkpart {
    pub start: u32,
    pub free_mb: u32,
    pub code: i32,
}

#[repr(C)]
pub struct MyosInstall {
    pub lba: u32,
    pub sectors: u32,
    pub kern_sectors: u32,
    pub code: i32,
}

#[repr(C)]
pub struct MyosBoot {
    pub index: i32,
    pub lba: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MyosFile {
    pub name: [u8; 56],
    pub start: u32,
    pub size: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MyosEvent {
    pub type_: i32,
    pub d1: i32,
    pub d2: i32,
    pub d3: i32,
}
