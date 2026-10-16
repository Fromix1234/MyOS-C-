// ===========================================================================
//  KERNEL k2: ATA PIO disk driver
// ===========================================================================
//  MyOS v2.0 module. Split mechanically from kernel.cpp v1.2
//  (git a81d4f4) and then reworked: see README section 11.

// ===========================================================================
//  ДИСК: чтение секторов по ATA PIO (первичный канал, master)
// ===========================================================================
//  В защищённом режиме BIOS (int 0x13) недоступен, поэтому говорим с диском
//  напрямую через порты ATA-контроллера 0x1F0..0x1F7.
//  PIO = программный ввод-вывод: выбираем сектор, даём команду, ждём готовности
//  и читаем 256 слов (512 байт = ровно один сектор).
const unsigned short ATA_IO   = 0x1F0;   // данные (чтение — по 16 бит)
const unsigned short ATA_CNT  = 0x1F2;   // сколько секторов читать
const unsigned short ATA_LBA0 = 0x1F3;   // LBA, байт 0
const unsigned short ATA_LBA1 = 0x1F4;   // LBA, байт 1
const unsigned short ATA_LBA2 = 0x1F5;   // LBA, байт 2
const unsigned short ATA_DRV  = 0x1F6;   // выбор диска + старшие биты LBA
const unsigned short ATA_CMD  = 0x1F7;   // команда (запись) / статус (чтение)
const unsigned short ATA_ALT  = 0x3F6;   // альтернативный статус (для задержки)
const unsigned int DISK_SECTORS = 65536; // размер нашего образа: 32 МБ = 65536 секторов

// Сколько секторов занимает само ядро на диске. Число ВШИВАЕТСЯ при сборке
// (build.ps1 компилирует дважды и подставляет -DKERNEL_SECTORS=...).
// Нужно команде install: она копирует ядро из начала диска в раздел.
#ifndef KERNEL_SECTORS
#define KERNEL_SECTORS 48      // запасной вариант, если собрали без параметра
#endif

// Ждать, пока диск не «занят» (BSY = 0)
void ata_wait_bsy() {
    for (int t = 0; t < 1000000; t++)
        if (!(inb(ATA_CMD) & 0x80)) return;   // бит 7 = BSY (занят)
}
// Ждать, пока появились данные (DRQ = 1)
void ata_wait_drq() {
    for (int t = 0; t < 1000000; t++)
        if (inb(ATA_CMD) & 0x08) return;      // бит 3 = DRQ (данные готовы)
}
// Маленькая пауза: ATA хочет ~400 нс между записью регистров.
// Чтение альтернативного статуса занимает ~100 нс, читаем 4 раза.
void ata_delay() {
    for (int i = 0; i < 4; i++) inb(ATA_ALT);
}

// Прочитать ОДИН сектор (512 байт) по 28-битному адресу LBA в буфер
void ata_read_sector(unsigned int lba, unsigned char* buf) {
    ata_wait_bsy();
    outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F)); // master + старшие 4 бита LBA
    ata_delay();
    outb(ATA_CNT,  1);                          // читаем ровно 1 сектор
    outb(ATA_LBA0, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_CMD,  0x20);                       // команда READ SECTORS
    ata_wait_bsy();
    ata_wait_drq();
    unsigned short* w = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) w[i] = inw(ATA_IO);  // 256 слов = 512 байт
}

// Записать ОДИН сектор (512 байт) по адресу LBA ИЗ буфера
void ata_write_sector(unsigned int lba, unsigned char* buf) {
    ata_wait_bsy();
    outb(ATA_DRV, 0xE0 | ((lba >> 24) & 0x0F)); // master + старшие 4 бита LBA
    ata_delay();
    outb(ATA_CNT,  1);                          // пишем ровно 1 сектор
    outb(ATA_LBA0, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_CMD,  0x30);                       // команда WRITE SECTORS
    ata_wait_bsy();
    ata_wait_drq();
    unsigned short* w = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) outw(ATA_IO, w[i]);  // 256 слов = 512 байт
    outb(ATA_CMD, 0xE7);                        // FLUSH CACHE: сбросить кэш на диск
    ata_wait_bsy();
}

// Буфер под один сектор диска (глобальный — не держим 512 байт на стеке)
unsigned char disk_buf[512];

