<div align="center">

# 🖥️ MyOS
### *The Future of Minimalist Operating Systems*

![MyOS Banner](assets/banner.jpg)

**Design by CEO FROMIX**

---

> [!WARNING]
> **PROJECT STATUS: PRE-RELEASE**
> *The system is currently in active development. Expect frequent updates and experimental features.*

</div>

## 🚀 Overview
**MyOS** — это амбициозный проект по созданию операционной системы с нуля. От низкоуровневого загрузчика до современного графического интерфейса с эффектом «жидкого стекла». 

**Цель проекта:** Доказать, что мощная и функциональная ОС может быть компактной, быстрой и эстетически совершенной.

---

## 🛠️ Technical Architecture
MyOS построена на принципе разделения ядра и оболочки (Ring 0 $\rightarrow$ Ring 3), что обеспечивает стабильность и модульность.

| Компонент | Технология | Описание |
| :--- | :--- | :--- |
| **Bootloader** | NASM | MBR $\rightarrow$ VBR $\rightarrow$ Stage2 (VESA Init) |
| **Kernel (v3.0)** | Rust / C++ | Управление памятью, системные вызовы, драйверы |
| **Shell (GUI)** | C++ | Оконный менеджер, композитор, приложения |
| **FileSystem** | MyFS | Собственная оптимизированная файловая система |

---

## ✨ Key Features
- 🧊 **Liquid Glass UI**: Полупрозрачные окна и элементы интерфейса.
- 🖱️ **Advanced Input**: Поддержка IntelliMouse, скролл-колеса и высокая точность.
- 💻 **MyC Compiler**: Встроенный компилятор в настоящий x86 машинный код.
- 🛠️ **Disk Management**: Графический мастер установки и управления разделами.

---

## 👥 Developers & Community
**"You can be on the list of developers."**

Мы открыты для сотрудничества с теми, кто горит идеей низкоуровневого программирования. Если вы хотите оставить свой след в истории MyOS — присоединяйтесь к нам!

- **CEO & Lead Architect:** [FROMIX](https://github.com/Fromix1234)
- **Contributors:** *Your name could be here...*

---

## 📦 Quick Start (Oracle VM VirtualBox)
Для запуска проекта используйте следующие шаги:

### 1. Сборка образа
Запустите скрипт в PowerShell:
```powershell
cd MyOS-C-
powershell -ExecutionPolicy Bypass -File build.ps1 -rust
```
*Скрипт создаст файл `disk.img` в корне проекта.*

### 2. Запуск в VirtualBox
1. Создайте новую виртуальную машину (**Type**: Other, **Version**: Other/32-bit).
2. В разделе **Storage (Носители)** добавьте контроллер IDE.
3. Добавьте существующий жесткий диск $\rightarrow$ выберите ваш `disk.img`.
4. В настройках **System $\rightarrow$ Motherboard** убедитесь, что включена опция **Enable EFI** (выключена, так как MyOS использует BIOS/Legacy).
5. Запустите машину.

---

<div align="center">
  <sub>© 2026 MyOS Project. All rights reserved. Designed by FROMIX.</sub>
</div>
