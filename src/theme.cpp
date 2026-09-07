// Vetro Look, GPL-3.0-or-later. Palette, localisation and the icon set.
#include "ui.h"

bool reducedMotion=false;
int language=0;

Palette Theme(float light){
 Palette dark{},day{};
 dark.glass    =D2D1::ColorF(.14f,.16f,.19f,.28f);
 dark.glassEdge=D2D1::ColorF(1,1,1,.13f);
 dark.glassLift=D2D1::ColorF(1,1,1,.055f);
 dark.card     =D2D1::ColorF(1,1,1,.075f);
 dark.cardEdge =D2D1::ColorF(1,1,1,.07f);
 dark.text     =D2D1::ColorF(.955f,.965f,.985f,1.f);
 dark.dim      =D2D1::ColorF(.955f,.965f,.985f,.66f);
 dark.faint    =D2D1::ColorF(.955f,.965f,.985f,.40f);
 dark.accent   =D2D1::ColorF(.36f,.62f,1.f,1.f);
 dark.danger   =D2D1::ColorF(1.f,.44f,.40f,1.f);
 dark.hover    =D2D1::ColorF(1,1,1,.10f);
 dark.press    =D2D1::ColorF(1,1,1,.17f);
 dark.track    =D2D1::ColorF(1,1,1,.17f);
 dark.thumb    =D2D1::ColorF(1,1,1,.96f);
 dark.sunk     =D2D1::ColorF(0,0,0,.26f);
 dark.sep      =D2D1::ColorF(1,1,1,.09f);
 dark.plate    =D2D1::ColorF(0,0,0,.24f);

 day.glass     =D2D1::ColorF(.975f,.98f,1.f,.36f);
 day.glassEdge =D2D1::ColorF(1,1,1,.8f);
 day.glassLift =D2D1::ColorF(1,1,1,.34f);
 day.card      =D2D1::ColorF(1,1,1,.10f);
 day.cardEdge  =D2D1::ColorF(1,1,1,.16f);
 day.text      =D2D1::ColorF(.09f,.10f,.13f,1.f);
 day.dim       =D2D1::ColorF(.09f,.10f,.13f,.62f);
 day.faint     =D2D1::ColorF(.09f,.10f,.13f,.38f);
 day.accent    =D2D1::ColorF(.10f,.42f,.93f,1.f);
 day.danger    =D2D1::ColorF(.80f,.16f,.14f,1.f);
 day.hover     =D2D1::ColorF(0,0,0,.055f);
 day.press     =D2D1::ColorF(0,0,0,.10f);
 day.track     =D2D1::ColorF(0,0,0,.13f);
 day.thumb     =D2D1::ColorF(.14f,.15f,.19f,1.f);
 day.sunk      =D2D1::ColorF(0,0,0,.07f);
 day.sep       =D2D1::ColorF(0,0,0,.08f);
 day.plate     =D2D1::ColorF(0,0,0,.05f);

 float t=(std::max)(0.f,(std::min)(1.f,light));
 Palette p;
 p.glass=Mix(dark.glass,day.glass,t);       p.glassEdge=Mix(dark.glassEdge,day.glassEdge,t);
 p.glassLift=Mix(dark.glassLift,day.glassLift,t);
 p.card=Mix(dark.card,day.card,t);          p.cardEdge=Mix(dark.cardEdge,day.cardEdge,t);
 p.text=Mix(dark.text,day.text,t);          p.dim=Mix(dark.dim,day.dim,t);
 p.faint=Mix(dark.faint,day.faint,t);       p.accent=Mix(dark.accent,day.accent,t);
 p.danger=Mix(dark.danger,day.danger,t);    p.hover=Mix(dark.hover,day.hover,t);
 p.press=Mix(dark.press,day.press,t);       p.track=Mix(dark.track,day.track,t);
 p.thumb=Mix(dark.thumb,day.thumb,t);       p.sunk=Mix(dark.sunk,day.sunk,t);
 p.sep=Mix(dark.sep,day.sep,t);             p.plate=Mix(dark.plate,day.plate,t);
 return p;
}

struct Phrase{const wchar_t* ru;const wchar_t* en;};
static const Phrase phrases[]={
 {L"Меню",L"Menu"},
 {L"Дополнительные действия",L"More actions"},
 {L"Отправить",L"Send"},
 {L"Сохранить",L"Save"},
 {L"Сохранить как",L"Save as"},
 {L"Печать",L"Print"},
 {L"Удалить",L"Delete"},
 {L"Тема",L"Theme"},
 {L"Тёмная",L"Dark"},
 {L"Светлая",L"Light"},
 {L"Язык",L"Language"},

 {L"Edit",L"Edit"},
 {L"Инструменты редактирования",L"Editing tools"},
 {L"Обрезать",L"Crop"},
 {L"Повернуть",L"Rotate"},
 {L"Рисовать",L"Draw"},
 {L"Стрелка",L"Arrow"},
 {L"Выделить",L"Select"},

 {L"Информация",L"Information"},
 {L"ФАЙЛ",L"FILE"},
 {L"КАМЕРА",L"CAMERA"},
 {L"ГИСТОГРАММА",L"HISTOGRAM"},
 {L"ЭКСПОЗИЦИЯ",L"EXPOSURE"},
 {L"МЕСТО СЪЁМКИ",L"LOCATION"},

 {L"Имя файла",L"Filename"},
 {L"Формат",L"Format"},
 {L"Размер",L"Dimensions"},
 {L"Мегапиксели",L"Megapixels"},
 {L"Объём",L"File size"},
 {L"Создан",L"Created"},
 {L"Изменён",L"Modified"},
 {L"Цветовой профиль",L"Color profile"},
 {L"Глубина цвета",L"Bit depth"},

 {L"Камера",L"Camera"},
 {L"Объектив",L"Lens"},
 {L"Фокусное",L"Focal length"},
 {L"Диафрагма",L"Aperture"},
 {L"Выдержка",L"Shutter"},
 {L"ISO",L"ISO"},
 {L"Экспокоррекция",L"Exposure comp."},
 {L"Баланс белого",L"White balance"},
 {L"Вспышка",L"Flash"},
 {L"Замер",L"Metering"},

 {L"Света",L"Highlights"},
 {L"Тени",L"Shadows"},
 {L"Пересветы",L"Highlight clipping"},
 {L"Провалы",L"Shadow clipping"},
 {L"Широта",L"Latitude"},
 {L"Долгота",L"Longitude"},

 {L"Свободно",L"Free"},
 {L"Применить",L"Apply"},
 {L"Отмена",L"Cancel"},
 {L"Отменить",L"Undo"},
 {L"Цвет",L"Color"},
 {L"Толщина",L"Thickness"},
 {L"Переместить в корзину?",L"Move to Recycle Bin?"},
 {L"Файл перемещён в корзину",L"Moved to Recycle Bin"},
 {L"Сохранено",L"Saved"},
 {L"Изменений нет",L"No changes"},
 {L"Скопировано",L"Copied"},
 {L"Сначала сохраните изменения",L"Save your changes first"},

 {L"Сохранение…",L"Saving…"},
 {L"Открытие…",L"Opening…"},
 {L"Чтение метаданных…",L"Reading metadata…"},
 {L"Ближе к каждой детали.",L"A little closer to every detail."},
 {L"Перетащите изображение сюда или нажмите Ctrl + O",L"Drop an image here, or press Ctrl + O"},
 {L"Пробел в Проводнике открывает быстрый просмотр",L"Space in Explorer opens a quick preview"},
 {L"Недоступно",L"Unavailable"},
 {L"Влево",L"Left"},
 {L"Вправо",L"Right"},

 {L"Назад",L"Back"},
 {L"Свернуть",L"Minimise"},
 {L"Развернуть",L"Maximise"},
 {L"Восстановить",L"Restore"},
 {L"Закрыть",L"Close"},
 {L"Сведения",L"Info"},
 {L"Копировать",L"Copy"},
 {L"В избранное",L"Favourite"},
 {L"Вписать",L"Fit"},
 {L"1:1",L"1:1"},
 {L"Ещё",L"More"},

 {L"Открывать по умолчанию",L"Make default viewer"},
 {L"Готово - выберите Vetro Look в параметрах Windows",L"Done - pick Vetro Look in Windows settings"},
 {L"Не удалось зарегистрировать приложение",L"Could not register the application"},
 {L"Просмотр по пробелу",L"Space to preview"},
 {L"Вкл",L"On"},
 {L"Выкл",L"Off"},
 {L"Путь к файлу",L"File path"},
 {L"Открыть на карте",L"Open in Maps"},

 {L"Поиск по папкам…",L"Search folders…"},
 {L"Поиск по фото…",L"Search photos…"},
 {L"Имя",L"Name"},
 {L"Дата",L"Date"},
 {L"Размер",L"Size"},
 {L"Фильтры",L"Filters"},
 {L"Форматы",L"Formats"},
 {L"Размер папки",L"Folder size"},
 {L"Тип",L"Kind"},
 {L"RAW",L"RAW"},
 {L"Обычные",L"Regular"},
 {L"Цветовой профиль",L"Color profile"},
 {L"Любой",L"Any"},
 {L"Без профиля",L"No profile"},
 {L"Сбросить",L"Clear"},
 {L"Применить",L"Apply"},
 {L"Индексирование…",L"Indexing…"},
 {L"Найдено",L"Found"},
 {L"папка",L"folder"},
 {L"папки",L"folders"},
 {L"папок",L"folders"},
 {L"фото",L"photos"},
 {L"Библиотека обновлена",L"Library updated"},
 {L"Обновление библиотеки…",L"Updating library…"},
 {L"Папок с фото не найдено",L"No photo folders found"},
 {L"В этой папке нет фото",L"No photos in this folder"},
 {L"Ваша фотоколлекция",L"Your photo collection"},
 {L"Пересканировать",L"Rescan"},
};
static_assert(sizeof(phrases)/sizeof(phrases[0])==S_COUNT,"localisation table is out of step with Str");
const wchar_t* T(Str s){
 if(s<0||s>=S_COUNT)return L"";
 return language?phrases[s].en:phrases[s].ru;
}

// Icon geometry, authored inside a 24 x 24 box.
const wchar_t* IcBack     =L"M15 5 L8.2 12 L15 19";
const wchar_t* IcMinus    =L"M5.5 12 L18.5 12";
const wchar_t* IcPlus     =L"M12 5.5 L12 18.5 M5.5 12 L18.5 12";
const wchar_t* IcInfo     =L"M12 3.2 C16.85 3.2 20.8 7.15 20.8 12 C20.8 16.85 16.85 20.8 12 20.8 C7.15 20.8 3.2 16.85 3.2 12 C3.2 7.15 7.15 3.2 12 3.2 Z M12 10.6 L12 16.4 M12 7.3 L12 8.4";
const wchar_t* IcCopy     =L"M9 8.4 L19.6 8.4 L19.6 20 L9 20 Z M15.4 8.4 L15.4 4 L4.4 4 L4.4 15.2 L9 15.2";
const wchar_t* IcCheck    =L"M5 12.6 L10 17.6 L19 6.6";
const wchar_t* IcHeart    =L"M12 20.6 C12 20.6 3.4 15.1 3.4 9.3 C3.4 6.4 5.7 4 8.6 4 C10.3 4 11.5 5 12 6.1 C12.5 5 13.7 4 15.4 4 C18.3 4 20.6 6.4 20.6 9.3 C20.6 15.1 12 20.6 12 20.6 Z";
const wchar_t* IcRotate   =L"M12 3.6 C16.65 3.6 20.4 7.35 20.4 12 C20.4 16.65 16.65 20.4 12 20.4 C7.35 20.4 3.6 16.65 3.6 12 C3.6 9.55 4.65 7.35 6.35 5.8 M9.2 1.1 L12.5 3.6 L9.2 6.1";
const wchar_t* IcExpand   =L"M4.5 9.5 L4.5 4.5 L9.5 4.5 M14.5 4.5 L19.5 4.5 L19.5 9.5 M19.5 14.5 L19.5 19.5 L14.5 19.5 M9.5 19.5 L4.5 19.5 L4.5 14.5";
const wchar_t* IcCompress =L"M9.5 4.5 L9.5 9.5 L4.5 9.5 M14.5 4.5 L14.5 9.5 L19.5 9.5 M19.5 14.5 L14.5 14.5 L14.5 19.5 M4.5 14.5 L9.5 14.5 L9.5 19.5";
const wchar_t* IcDots     =L"M5.7 10.3 C6.64 10.3 7.4 11.06 7.4 12 C7.4 12.94 6.64 13.7 5.7 13.7 C4.76 13.7 4 12.94 4 12 C4 11.06 4.76 10.3 5.7 10.3 Z M12 10.3 C12.94 10.3 13.7 11.06 13.7 12 C13.7 12.94 12.94 13.7 12 13.7 C11.06 13.7 10.3 12.94 10.3 12 C10.3 11.06 11.06 10.3 12 10.3 Z M18.3 10.3 C19.24 10.3 20 11.06 20 12 C20 12.94 19.24 13.7 18.3 13.7 C17.36 13.7 16.6 12.94 16.6 12 C16.6 11.06 17.36 10.3 18.3 10.3 Z";
const wchar_t* IcMin      =L"M5.5 12.5 L18.5 12.5";
const wchar_t* IcMax      =L"M5.5 5.5 L18.5 5.5 L18.5 18.5 L5.5 18.5 Z";
const wchar_t* IcRestore  =L"M8 8 L8 4.6 L19.4 4.6 L19.4 16 L16 16 M4.6 8 L16 8 L16 19.4 L4.6 19.4 Z";
const wchar_t* IcClose    =L"M6.2 6.2 L17.8 17.8 M17.8 6.2 L6.2 17.8";
const wchar_t* IcShare    =L"M12 3 L12 14.6 M8 7 L12 3 L16 7 M5.4 12.6 L5.4 20.4 L18.6 20.4 L18.6 12.6";
const wchar_t* IcSave     =L"M12 3.4 L12 15 M8 11 L12 15 L16 11 M5.4 19.6 L18.6 19.6";
const wchar_t* IcSaveAs   =L"M6 3.2 L13.8 3.2 L19 8.4 L19 20.8 L6 20.8 Z M13.8 3.2 L13.8 8.4 L19 8.4 M9.2 15 L15.4 15";
const wchar_t* IcPrint    =L"M7 9 L7 3.4 L17 3.4 L17 9 M7 9 L4.6 9 L4.6 16.6 L7 16.6 M17 9 L19.4 9 L19.4 16.6 L17 16.6 M7 13.6 L17 13.6 L17 20.6 L7 20.6 Z";
const wchar_t* IcTrash    =L"M4.2 6.2 L19.8 6.2 M9 6.2 L9 3.8 L15 3.8 L15 6.2 M6 6.2 L7 20.6 L17 20.6 L18 6.2 M10 10 L10 17 M14 10 L14 17";
const wchar_t* IcMoon     =L"M19.6 14.4 C18.5 14.9 17.3 15.2 16.1 15.2 C11.9 15.2 8.5 11.8 8.5 7.6 C8.5 6.4 8.8 5.2 9.3 4.2 C5.9 5.3 3.4 8.5 3.4 12.3 C3.4 17.1 7.3 21 12.1 21 C15.9 21 19.1 18.5 20.2 15.1 Z";
const wchar_t* IcSun      =L"M12 7.6 C14.43 7.6 16.4 9.57 16.4 12 C16.4 14.43 14.43 16.4 12 16.4 C9.57 16.4 7.6 14.43 7.6 12 C7.6 9.57 9.57 7.6 12 7.6 Z M12 2.4 L12 4.6 M12 19.4 L12 21.6 M2.4 12 L4.6 12 M19.4 12 L21.6 12 M5.2 5.2 L6.8 6.8 M17.2 17.2 L18.8 18.8 M18.8 5.2 L17.2 6.8 M6.8 17.2 L5.2 18.8";
const wchar_t* IcGlobe    =L"M12 3.2 C16.85 3.2 20.8 7.15 20.8 12 C20.8 16.85 16.85 20.8 12 20.8 C7.15 20.8 3.2 16.85 3.2 12 C3.2 7.15 7.15 3.2 12 3.2 Z M3.4 12 L20.6 12 M12 3.2 C14.4 5.6 15.4 8.8 15.4 12 C15.4 15.2 14.4 18.4 12 20.8 C9.6 18.4 8.6 15.2 8.6 12 C8.6 8.8 9.6 5.6 12 3.2 Z";
const wchar_t* IcCrop     =L"M6.4 2.4 L6.4 17.6 L21.6 17.6 M2.4 6.4 L17.6 6.4 L17.6 21.6";
const wchar_t* IcPen      =L"M4 20 L4.6 15.6 L15.8 4.4 L19.6 8.2 L8.4 19.4 Z M14 6.2 L17.8 10";
const wchar_t* IcArrow    =L"M5 19 L18.6 5.4 M10.4 5.4 L18.6 5.4 L18.6 13.6";
const wchar_t* IcMarquee  =L"M4.4 4.4 L19.6 4.4 L19.6 19.6 L4.4 19.6 Z";
const wchar_t* IcChevron  =L"M9.6 5.4 L16.2 12 L9.6 18.6";
const wchar_t* IcChevronL =L"M14.4 5.4 L7.8 12 L14.4 18.6";
const wchar_t* IcDefault  =L"M3.6 4.8 L20.4 4.8 L20.4 19.2 L3.6 19.2 Z M3.6 8.6 L20.4 8.6 M8.4 14 L11.1 16.7 L16 11.4";
const wchar_t* IcSpace    =L"M3.2 8.4 L20.8 8.4 L20.8 15.6 L3.2 15.6 Z M7.2 11.2 L7.2 12.8 M16.8 11.2 L16.8 12.8";
const wchar_t* IcSearch   =L"M10.8 4.4 C14.89 4.4 18.2 7.71 18.2 11.8 C18.2 15.89 14.89 19.2 10.8 19.2 C6.71 19.2 3.4 15.89 3.4 11.8 C3.4 7.71 6.71 4.4 10.8 4.4 Z M16.2 17.2 L21 22";
const wchar_t* IcSortLines=L"M4.2 6.8 L19.8 6.8 M4.2 12 L14.4 12 M4.2 17.2 L9.6 17.2";
const wchar_t* IcFilter   =L"M3.6 5 L20.4 5 L14 13.2 L14 19.4 L10 19.4 L10 13.2 Z";
const wchar_t* IcFolderIc =L"M3.6 8 C3.6 6.8 4.6 5.8 5.8 5.8 L9.6 5.8 L11.6 7.8 L18.2 7.8 C19.4 7.8 20.4 8.8 20.4 10 L20.4 17 C20.4 18.2 19.4 19.2 18.2 19.2 L5.8 19.2 C4.6 19.2 3.6 18.2 3.6 17 Z";
const wchar_t* IcGridPhoto=L"M4 4.4 L10.2 4.4 L10.2 10.6 L4 10.6 Z M13.8 4.4 L20 4.4 L20 10.6 L13.8 10.6 Z M4 13.8 L10.2 13.8 L10.2 20 L4 20 Z M13.8 13.8 L20 13.8 L20 20 L13.8 20 Z";
const wchar_t* IcCheckbox =L"M5 12.4 L9.6 17 L19 6.6";
