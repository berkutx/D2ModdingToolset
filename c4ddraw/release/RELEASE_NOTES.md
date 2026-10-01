C4dll-R __VER__ — окно на всю рабочую область и улучшенный автоподбор разрешения.

В «Видео > Режим экрана» добавлено «Окно на всю рабочую область (панель задач видна)». Игра разворачивается штатным способом Windows: заголовок и меню сохраняются, панель задач остаётся доступна, прежние узкие отступы по краям исчезают. Кнопка справа сверху и двойной щелчок по заголовку включают тот же режим даже при resizable=false. Восстановление возвращает прежнее окно, а F4 — выбранный оконный режим после полного экрана.

Авто подбирает разрешение для монитора игры и запоминает его для следующего оконного запуска. Если монитор отключён, используется доступный. В меню показаны выбранный размер и необходимость перезапуска. При разворачивании рекомендация включить Авто появляется, если подходящее разрешение уменьшит увеличение изображения; учитываются и штатные 800x600, 1024x768, 1280x1024. Игра сама не закрывается: сохранитесь и перезапустите её вручную.

Исправлено «Центральное окно > На всю высоту»: фиксированные меню и панели используют доступную высоту внутри окна. Увеличение сохраняет пропорции и при необходимости обрезает боковой декор; ширина интерфейса ограничивает увеличение на узком экране. Координаты мыши и видимая область для плагинов учитывают этот масштаб. Стратегическая карта сохраняет прежнее поведение. Повышение разрешения само по себе не уменьшит центральное меню, настроенное на всю высоту.

Перед обновлением закройте игру и сохраните старые DLL и плагины. Из `__ZIP__` обновите `C4dll-R.dll`, оба плагина и папку `Shaders`, сохранив свои INI. Сброс настроек не требуется. Управление окном описано в `WINDOW-WORKAREA-RU.txt`, настройка захвата OBS — в `TWITCH-STREAMER-RU.md`.

### English

Work-area mode uses native Windows maximization while keeping the caption, menu and taskbar. The caption maximize button and double-click select the same mode even with resizable=false. Restore returns to the previous window; F4 returns from fullscreen to the selected windowed mode.

Automatic resolution follows the game's monitor and remembers it for the next windowed launch, with a fallback if that monitor is disconnected. The menu shows the selected resolution and whether a restart is needed. A useful resolution change can prompt a restart recommendation, including supported original game resolutions. Save your game and restart manually; the prompt does not close it.

The existing fixed-window “fill height” preset now uses the available client height. Uniform enlargement trims side decoration while preserving the central UI; its width limits enlargement in narrow windows. Input and plugin visible-area geometry follow the same scaling. The strategic map retains its behavior. A higher canvas resolution does not shrink a central menu configured to fill the height.

Close the game before updating the wrapper, both plugins and shaders from `__ZIP__`. Preserve your INI files; no settings reset is required. See `WINDOW-WORKAREA-RU.txt` for window controls and `TWITCH-STREAMER-RU.md` for OBS capture setup.

### Предыдущая версия: 2.2.0


В C4dll-R 2.2.0 добавлен фильтр Lanczos + Bicubic: смесь резкого Lanczos и более мягкого Bicubic в равных долях. Он доступен в меню «Видео > Фильтр» и выбран по умолчанию для новой настройки OpenGL и после сброса. При обновлении существующий выбор фильтра сохраняется. Отдельные Lanczos и Bicubic остаются в меню.

В меню «Игра > MNS/SMNS > Прокрутка карты» независимо включаются перетаскивание левой кнопкой, перетаскивание средней кнопкой и прокрутка у края экрана. Изменения применяются сразу, прежние настройки сохраняются. Нажатие средней кнопки без движения не выбирает объект.

Добавлено периодическое повторное сетевое уведомление через штатный цикл игры: оно позволяет возобновить обработку очереди после потери уведомления. Во вложенных окнах запрос откладывается, устаревшие запросы отменяются. Служебный таймер врапера перенесён на отдельное скрытое окно в том же потоке, чтобы его ID не пересекался с игровыми таймерами. MSS не изменяется. Исправление проверено сборкой и автоматическими проверками; подтверждения в живом сетевом матче пока нет.

В архиве `C4dll-R-v2.2.0.zip` находятся `C4dll-R.dll`, `Mods/timer.c4p`, `Mods/twitchstat.c4p` и все девять фильтров. Перед обновлением закройте все клиенты игры, обновите DLL, оба плагина и папку `Shaders` из одного архива, сохранив свои `ddraw.ini`, `C4menu.ini` и `C4plugins.ini`. Сброс настроек не требуется.

### English

C4dll-R 2.2.0 adds Lanczos + Bicubic, an equal blend of the two filters. It is the default for a new OpenGL configuration and after a reset; existing filter choices are preserved.

The “Game > MNS/SMNS > Map Scroll” submenu has independent Left Mouse Button, Middle Mouse Button and Edge Detection checkboxes. Changes apply immediately and existing settings are preserved. A middle click without dragging does not select an object.

Periodic network notifications now pass through the game's normal event loop to resume queue processing after a lost notification. Nested windows defer the request, and stale requests are cancelled. Wrapper housekeeping uses a separate hidden window on the same thread to prevent timer ID collisions with the game. MSS is unchanged. Build and automated checks passed; a live multiplayer match has not yet validated this fix.

Close all game clients before updating. Install `C4dll-R.dll`, both plugins and `Shaders` from `C4dll-R-v2.2.0.zip` together, preserving your `ddraw.ini`, `C4menu.ini` and `C4plugins.ini`. No settings reset is needed.

### Предыдущая версия: 2.1.0

Добавлен короткий сетевой журнал для проверки передачи хода между хостом и джойнером.

- При включённой диагностике C4trace записывает отправку и получение BeginTurn, EndTurn и TurnInfo, обработку TurnInfo и отключение игрока. Раз в пять секунд сохраняется сводка сетевых вызовов и работы интерфейса. Содержимое остальных пакетов не сохраняется.
- Диагностика по умолчанию выключена. Включение через «Производительность > Технические настройки > Диагностика сети и задержек (рестарт)...» сохраняет настройку и закрывает этот клиент. Сначала сохраните игру, затем запустите её снова вручную. Для сравнения включите журнал до матча у обоих игроков и сохраните оба файла `C4trace-*.csv` рядом с `Discipl2.exe`.
- Подробная трасса по-прежнему доступна для отдельного исследования: дополнительно задайте `C4DLL_NETTRACE_DETAIL=1` перед запуском. Обычный переключатель включает короткий режим. Инструкция по сбору и чтению журналов — в `NETWORK_TRACE.md`.

Сохранены Twitch Stat, настройки «Игра > MNS/SMNS» и предыдущие исправления таймера и палитры. Настройка трансляции описана в `TWITCH-STREAMER-RU.md`.
