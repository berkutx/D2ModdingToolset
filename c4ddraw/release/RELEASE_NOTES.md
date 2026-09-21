В C4dll-R __VER__ добавлен фильтр Lanczos + Bicubic: смесь резкого Lanczos и более мягкого Bicubic в равных долях. Он доступен в меню «Видео > Фильтр» и выбран по умолчанию для новой настройки OpenGL и после сброса. При обновлении существующий выбор фильтра сохраняется. Отдельные Lanczos и Bicubic остаются в меню.

В меню «Игра > MNS/SMNS > Прокрутка карты» независимо включаются перетаскивание левой кнопкой, перетаскивание средней кнопкой и прокрутка у края экрана. Изменения применяются сразу, прежние настройки сохраняются. Нажатие средней кнопки без движения не выбирает объект.

Добавлено периодическое повторное сетевое уведомление через штатный цикл игры: оно позволяет возобновить обработку очереди после потери уведомления. Во вложенных окнах запрос откладывается, устаревшие запросы отменяются. Служебный таймер врапера перенесён на отдельное скрытое окно в том же потоке, чтобы его ID не пересекался с игровыми таймерами. MSS не изменяется. Исправление проверено сборкой и автоматическими проверками; подтверждения в живом сетевом матче пока нет.

В архиве `__ZIP__` находятся `C4dll-R.dll`, `Mods/timer.c4p`, `Mods/twitchstat.c4p` и все девять фильтров. Перед обновлением закройте все клиенты игры, обновите DLL, оба плагина и папку `Shaders` из одного архива, сохранив свои `ddraw.ini`, `C4menu.ini` и `C4plugins.ini`. Сброс настроек не требуется.

### English

C4dll-R __VER__ adds Lanczos + Bicubic, an equal blend of the two filters. It is the default for a new OpenGL configuration and after a reset; existing filter choices are preserved.

The “Game > MNS/SMNS > Map Scroll” submenu has independent Left Mouse Button, Middle Mouse Button and Edge Detection checkboxes. Changes apply immediately and existing settings are preserved. A middle click without dragging does not select an object.

Periodic network notifications now pass through the game's normal event loop to resume queue processing after a lost notification. Nested windows defer the request, and stale requests are cancelled. Wrapper housekeeping uses a separate hidden window on the same thread to prevent timer ID collisions with the game. MSS is unchanged. Build and automated checks passed; a live multiplayer match has not yet validated this fix.

Close all game clients before updating. Install `C4dll-R.dll`, both plugins and `Shaders` from `__ZIP__` together, preserving your `ddraw.ini`, `C4menu.ini` and `C4plugins.ini`. No settings reset is needed.

### Предыдущая версия: 2.1.0

Добавлен короткий сетевой журнал для проверки передачи хода между хостом и джойнером.

- При включённой диагностике C4trace записывает отправку и получение BeginTurn, EndTurn и TurnInfo, обработку TurnInfo и отключение игрока. Раз в пять секунд сохраняется сводка сетевых вызовов и работы интерфейса. Содержимое остальных пакетов не сохраняется.
- Диагностика по умолчанию выключена. Включение через «Производительность > Технические настройки > Диагностика сети и задержек (рестарт)...» сохраняет настройку и закрывает этот клиент. Сначала сохраните игру, затем запустите её снова вручную. Для сравнения включите журнал до матча у обоих игроков и сохраните оба файла `C4trace-*.csv` рядом с `Discipl2.exe`.
- Подробная трасса по-прежнему доступна для отдельного исследования: дополнительно задайте `C4DLL_NETTRACE_DETAIL=1` перед запуском. Обычный переключатель включает короткий режим. Инструкция по сбору и чтению журналов — в `NETWORK_TRACE.md`.

Сохранены Twitch Stat, настройки «Игра > MNS/SMNS» и предыдущие исправления таймера и палитры. Настройка трансляции описана в `TWITCH-STREAMER-RU.md`.
