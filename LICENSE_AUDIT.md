# VetroLook — аудит лицензий и происхождения кода

Дата аудита: 7 сентября 2026. Это технический аудит исходного дерева, а не юридическое заключение.

## Метод

- Проверены все компилируемые файлы из `CMakeLists.txt`, вендорские каталоги и фактическая линковка.
- Текущие файлы сравнивались с локальными checkout'ами доноров по нормализованным последовательностям строк и ручной проверкой функций.
- Для QuickView найдено 440 совпадений восьмистрочных отпечатков, относящихся к двум EXIF-файлам. Для QuickLook найдено 0 непрерывных совпадений из шести строк в `explorer.cpp`; вывод ниже не основан на совпадении API или общей идее.

## Подтверждённые GPL-компоненты

### QuickView

- **VetroLook:** `third_party/exif.cpp`, `third_party/exif.h`.
- **Источник:** `QuickView/QuickView/exif.cpp`, `QuickView/QuickView/exif.h`, commit `3ab7d110ccbd9b5e3c4882c205abdbb0d3f1a62b`.
- **Доказательство:** `git diff --no-index --stat` для `exif.cpp` показывает только 4 изменённые строки: добавлен атрибуционный комментарий и `#include "pch.h"` заменён на `#include <cstring>`. Содержательная реализация `Rational`, `IFEntry`, JPEG/TIFF/EXIF-разбор и заголовок совпадают. Это не сходство API.
- **Объём:** 29,310 байт `.cpp` и 6,956 байт `.h`; практически полный файл донора, существенная самостоятельная часть программы.
- **Лицензия:** QuickView — GPL-3.0; сам исходник не несёт более узкой лицензии, а VetroLook корректно фиксирует происхождение в notices.
- **Последствие:** распространение комбинированной программы с этим компонентом должно соответствовать GPL-3.0-or-later; весь распространяемый VetroLook нельзя выдать только под MIT или proprietary/non-commercial условиями.

### QuickLook

- **VetroLook:** `src/explorer.cpp`.
- **Источник:** `QuickLook.Native/QuickLook.Native32/Shell32.cpp` и `HelperMethods.cpp`, commit `60eea46ae2efaf29c9c3a80c61a618ad774bea20`.
- **Доказательство:** файл VetroLook сам прямо атрибутирует адаптацию. Ручное сравнение подтверждает тот же специфический Explorer-поток: `IShellWindows` → `IServiceProvider` → `IShellBrowser` → `QueryActiveShellView` → `SVGIO_SELECTION` → `IDataObject`/`CF_HDROP`, включая сопоставление активной `ShellTabWindowClass`. При этом непрерывных шестистрочных совпадений не найдено, поэтому это не заявляется как дословная копия всего файла, а как адаптация, которую разумно считать GPL-покрытой до независимой clean-room реализации.
- **Объём:** 3,948 байт; ограниченный компонент интеграции с Explorer, не основной рендерер/UI.
- **Лицензия:** QuickLook GPL-3.0-or-later.
- **Последствие:** независимо от меньшего объёма, для выпуска следует сохранять GPL-режим и атрибуцию.

## Библиотеки, встроенные или слинкованные в текущий EXE

`CMakeLists.txt` статически линкует `turbojpeg-static`, `webpdecoder`, `webpdemux`, `vetro_raw` (полная компиляция LibRaw), `vetro_exr` (TinyEXR + miniz) и `avif` с импортированной статической `dav1d::dav1d`. `src/wuffs.cpp` компилирует единый файл `wuffs/release/c/wuffs-v0.4.c` через `#define WUFFS_IMPLEMENTATION`.

- **LibRaw:** LGPL-2.1 или CDDL-1.0 по выбору; текущие notices выбирают LGPL-2.1. Статическое связывание не делает основной код GPL, но при выпуске вне GPL требует, среди прочего, предоставить исходники/объекты или иной предусмотренный LGPL способ relink. В данном GPL-релизе всё равно нужны соответствующие исходники и notices.
- **libjpeg-turbo:** IJG + BSD-3-Clause; не copyleft. Требуются copyright/license notices.
- **Wuffs:** MIT OR Apache-2.0; встроенный исходник, не GPL. Сохранить MIT/Apache текст и NOTICE, если применим.
- **libwebp:** BSD-3-Clause; статически слинкована, notices обязательны, copyleft нет.
- **libavif:** BSD-2-Clause; статически слинкована. Использует dav1d.
- **dav1d:** BSD-2-Clause; статически слинкована через libavif.
- **TinyEXR:** BSD-3-Clause; статически слинкована вместе с bundled miniz и собственными notices. Сохранить все указанные лицензии вложенных частей.
- **libjxl:** BSD-3-Clause исходники есть в исходной рабочей папке, но не добавлены в `CMakeLists.txt`, не включены и не линкуются; в текущий VetroLook не входит.

Windows DLL (`d2d1`, `dwrite`, `windowscodecs`, `shell32` и т. п.) — системные компоненты Windows, не копируемые исходники VetroLook; из этого дерева нет доказательства, что они задают лицензию проекта.

## Выводы

1. **Текущий VetroLook является распространяемой GPL-комбинацией.** Наиболее сильное доказательство — практически дословный EXIF-код QuickView GPL-3.0; QuickLook добавляет отдельный GPL-риск. Это не вывод только из общей идеи Quick Look или из использования Windows Shell API.
2. **Лицензировать весь текущий проект только под MIT нельзя.** MIT можно было бы дать лишь собственной независимой части как дополнительное разрешение, но не всему комбинированному выпуску с GPL-кодом.
3. **Собственная non-commercial лицензия для текущего полного выпуска несовместима с GPL-полученными частями.** Дополнительные ограничения нельзя накладывать на GPL-права получателя.
4. **Чтобы снять GPL-обязательства:** полностью удалить `third_party/exif.cpp/.h` и заменить независимой реализацией/подходящей permissive библиотекой; переписать `src/explorer.cpp` по независимой спецификации Windows Shell, не сверяясь с QuickLook, либо удалить эту функцию; удалить донорские GPL-attribution/notices только после удаления кода и проверки. Затем повторно проверить все файлы и выбрать разрешительные альтернативы для статического LibRaw (LGPL всё ещё потребует выполнения своих условий) либо обеспечить LGPL-compliant dynamic/relinkable distribution. Одной смены заголовков или имени файлов недостаточно.

| Source | Evidence | License | Copied/Linked/Inspired | Impact on VetroLook | Action needed |
|---|---|---|---|---|---|
| QuickView | `third_party/exif.cpp` differs from donor by 4 lines; 440 eight-line fingerprints across EXIF files | GPL-3.0 | Copied/adapted | GPL applies to distributed combined work | Keep GPL/source/notices, or replace both EXIF files cleanly |
| QuickLook | Explicit attribution plus matching specialised Explorer selection workflow; no six-line verbatim run | GPL-3.0-or-later | Adapted | GPL risk/obligation for combined release | Keep GPL/source/notices, or clean-room rewrite/remove |
| LibRaw | `file(GLOB_RECURSE ... LibRaw/src/*.cpp)` → static `vetro_raw` | LGPL-2.1 option or CDDL-1.0 | Statically linked | Not GPL by itself; LGPL distribution conditions apply | Ship source/notices and relinking path; review before non-GPL distribution |
| libjpeg-turbo | Static `turbojpeg-static` | IJG + BSD-3-Clause | Statically linked | Notices only | Preserve notices/license text |
| Wuffs | Direct inclusion `wuffs-v0.4.c` | MIT OR Apache-2.0 | Embedded source | Notices/license obligations, no GPL | Preserve license/NOTICE |
| libwebp | Static `webpdecoder`, `webpdemux` | BSD-3-Clause | Statically linked | Notices only | Preserve notices/license text |
| libavif | Static `avif` | BSD-2-Clause | Statically linked | Notices only | Preserve notices/license text |
| dav1d | Imported static `libdav1d.a` | BSD-2-Clause | Statically linked | Notices only | Preserve notices/license text |
| TinyEXR/miniz | Static `vetro_exr` | BSD-3-Clause plus bundled notices | Embedded/static | Notices only | Preserve full TinyEXR/miniz notices |
| libjxl | No target/include/link reference | BSD-3-Clause | Not used | No current distribution impact | Do not claim JPEG XL support until a decoder is built and linked |
