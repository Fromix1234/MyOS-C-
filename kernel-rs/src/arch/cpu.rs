// ===========================================================================
//  arch::cpu — низкоуровневые инструкции x86
// ===========================================================================
//  Единственное место, где мы говорим с процессором «голыми» инструкциями.
//  Всё остальное ядро пользуется этими безопасными обёртками.
// ===========================================================================

/// Записать байт в порт ввода-вывода.
#[inline(always)]
pub unsafe fn outb(port: u16, value: u8) {
    core::arch::asm!(
        "out dx, al",
        in("dx") port,
        in("al") value,
        options(nomem, nostack, preserves_flags)
    );
}

/// Прочитать байт из порта ввода-вывода.
#[inline(always)]
pub unsafe fn inb(port: u16) -> u8 {
    let value: u8;
    core::arch::asm!(
        "in al, dx",
        in("dx") port,
        out("al") value,
        options(nomem, nostack, preserves_flags)
    );
    value
}

/// Остановить процессор до следующего прерывания.
#[inline(always)]
pub unsafe fn halt() {
    core::arch::asm!("hlt", options(nomem, nostack, preserves_flags));
}
