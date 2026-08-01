**C4dll-R __VER__** — самодостаточный DirectDraw-врапер для Disciples II на базе
cnc-ddraw.

**C4dll-R __VER__** — self-contained DirectDraw wrapper for Disciples II, built
on cnc-ddraw.

**Установка / Install:** скачайте / download `__ZIP__` и прочитайте / read
`INSTALL.txt`.

## Коротко / Summary

После 1.4 врапер получил единое RU/EN-меню, автоматический выбор между тремя штатными разрешениями и настоящим Hor+ под монитор, декоративные поля, широкий бой и облака, исправленные полноэкранные режимы, масштабирование и прокрутку карты, а также стабильный PvP-таймер и настройки производительности.

Since 1.4 the wrapper gained a unified RU/EN menu, monitor-adaptive selection between the three stock resolutions and true Hor+, decorative framing, widescreen battles and clouds, fixed fullscreen/scaling/map scrolling, plus a stabilized PvP timer and performance settings.

## Тестировщикам — коротко

Главный риск — новая цепочка «разрешение игры → размер окна/экрана → масштаб».
Проверьте чистую установку и обновление поверх 1.4 с сохранёнными INI. Особое
внимание:

- запуск любого широкого режима при включённой эмуляции 16-битного экрана:
  выбранный Hor+ должен присутствовать в списке DirectDraw, игра не должна
  показывать ошибку инициализации, а `ddraw.ini` не должен переписываться;
- единое меню «Разрешение»: «Авто под монитор», три штатных пункта со `★`, затем
  все десять проверенных широких режимов и внизу размер окна/вывода. На экранах
  4:3/5:4 авто должно выбрать штатный DisplaySize, на 3:2 и шире — Hor+; после
  полного рестарта карта, UI, курсор и клики должны совпадать;
- на Akella/GOG/Steam проверьте распознавание layout’а: разрешение игры и
  «Широкий бой» активны при совпавшей строке исходной таблицы DisciplesGL;
  прочие пункты `(MNS/SMNS)` остаются серыми. На неизвестном exe все адресные
  функции должны быть серыми;
- В «Видео → Разрешение → Только окно/стрим...» проверьте «Автоматически» (`0×0`), сохранение
  произвольного размера после перезапуска и немедленное применение. Размер игры
  не должен меняться; уменьшенное окно должно фильтроваться без рассинхронизации
  мыши, а borderless — по-прежнему занимать рабочий стол;
- При активном широком разрешении игры «Декоративный фон вокруг классических
  экранов» включён по умолчанию: фиксированные экраны должны окружаться фоном и
  рамкой DisciplesGL. В штатном разрешении пункт должен быть серым, поскольку
  внутри игрового кадра нет свободной области; отдельные поля вывода ему не
  принадлежат. Встроенный PNG-скриншот сохраняет оформленный игровой кадр;
- многократные `F4` и `Alt+Enter` между окном, borderless и exclusive: меню есть
  только в окне, не мигает, и всегда можно вернуться;
- автоматический «Широкий бой» на поддерживаемом exe: отдельного пункта меню в
  1.5 нет; со следующего боя обе панели, кнопки, предметы и зоны клика работают;
- обычный клик, перетаскивание карты и краевой скролл работают вместе, в том
  числе когда курсор вышел за край окна; карта начинает двигаться с первого
  изменившегося игрового пикселя и без стартового рывка к центру тайла;
- ускорение разных атак и многочастных эффектов заканчивается вместе с анимацией
  и возвращается к обычной скорости примерно за 300 мс;
- облака корректно обрабатывают отсутствующий, неверный и точный
  `Imgs\IsoClouds.ff`; настройка «1 CPU» применяется только после полного
  перезапуска.
- на чистой установке проверьте все восемь OpenGL-фильтров из комплектной папки
  `Shaders`, особенно multipass FSR и xBRZ.

## Все изменения после 1.4 — RU

- В «Видео» оставлен один popup «Разрешение», как у оригинального врапера. Он
  содержит автоподбор под монитор и отделяет три штатных режима со `★` от десяти
  проверенных широких режимов и размера окна/вывода. Авто берёт штатный
  DisplaySize для 4:3/5:4 и Hor+ для 3:2 и шире, отдавая приоритет целому
  масштабу; ручной выбор не переопределяется. Широкий кадр расширяет обзор, а не
  растягивает вывод; на неизвестном exe неподдержанные игровые строки серые.
- Универсальное меню врапера теперь доступно и на других сборках игры. Широкое
  разрешение и «Широкий бой» выбирают одну из девяти проверенных игровых раскладок по PE `ProductVersion`
  и контрольной инструкции; остальные адресные функции явно помечены
  `(MNS/SMNS)` и заблокированы вне этой мод-сборки.
- `GameCanvasMode=0` однозначно выбирает штатный DisplaySize, `1` — ручной Hor+,
  `2` — автоподбор; при отсутствующем ключе на поддерживаемом layout’е включается
  авто. Все логические размеры применяются только после полного перезапуска.
- Исправлен запуск широкого режима при эмуляции 16-битного экрана: служебная
  геометрия согласуется с выбранным игровым кадром только внутри процесса, без
  перезаписи `ddraw.ini`.
- Разрешение игры отделено от масштабирования. Длинный список пресетов вывода
  заменён одним диалогом «Разрешение → Окно/вывод...»: «Автоматически» сохраняет
  `0×0`, а пользовательская ширина 320–8192 и высота 240–8192 записываются в
  эффективную секцию `ddraw.ini` и применяются сразу. Размер меняет только вывод,
  не игровой кадр; borderless всегда берёт рабочий стол. Смена разрешения игры
  показывает отдельное сообщение о полном перезапуске.
- Добавлен включённый по умолчанию декоративный фон DisciplesGL вокруг
  фиксированных экранов внутри широкого игрового кадра. В штатном разрешении
  пункт серый: отдельные letterbox/pillarbox-поля вывода остаются частью
  рендерера. Встроенный PNG-скриншот сохраняет оформленный игровой кадр.
- Добавлен автоматический «Широкий бой» (включён по умолчанию): раскладка шириной
  990 пикселей показывает обе панели отрядов с корректными кнопками, предметами
  и зонами клика. Отдельного пункта меню в 1.5 нет; раскладка выбирается при
  открытии следующего боя.
- Добавлены облака карты через проверенный внешний `Imgs\IsoClouds.ff`. Пункт
  меню управляет штатной настройкой игры `Disciple.ini [Settings] IsoBirds`,
  требует рестарта и не ставит игровые хуки при неуспешной проверке.
- Исправлены полноэкранные режимы: меню скрыто в borderless и exclusive,
  устранены мерцание и рассинхронизация геометрии. `F4` надёжно возвращает в
  окно/обратно, `Alt+Enter` синхронизируется с меню.
- Перетаскивание карты включено по умолчанию для новой настройки. Штатный
  краевой скролл работает одновременно и подавляется только во время настоящего
  drag; прокрутка не останавливается при выходе курсора за край активного окна.
  Точная точка захвата сохраняется при нажатии, поэтому первый изменившийся
  игровой пиксель сразу сдвигает карту без перепривязки и начального рывка.
- Ускорение атаки заканчивается по реальному завершению последней визуальной
  части и возвращается к обычной скорости за 300 мс. Для несовпавшей сигнатуры
  остаётся безопасный временной fallback.
- «1 CPU» теперь строго применяется со следующего запуска: меню не делает
  live-reload affinity и не создаёт смешанное состояние потоков. Опция остаётся
  включённой по умолчанию.
- Настройки рендерера читаются и записываются в реально активной секции
  `ddraw.ini`; новый диалог сохраняет там размер окна, а live-reload не теряет
  его и режим, выбранный горячей клавишей. Новый конфиг следует разрешению игры
  вместо фиксированных 800×600; известные старые размеры безопасно мигрируют.
- В релизный ZIP добавлены все восемь выбираемых OpenGL-шейдеров и необходимые
  multipass-файлы. Исправлен ключ таймера `TableDuration_0`; плагин больше не
  встраивает шрифт с ограничением personal-use и использует системный fallback.
- Дефолты таймера настроены для PvP: принудительный режим, пауза анимаций и
  боёв игрок-против-игрока, завершение хода по истечении без retreat. Таймер
  переносится по `Ctrl+Alt+ЛКМ`. Автозавершение больше не показывает вопрос и
  не переносит отложенное нажатие в следующий положительный ход.
- Сборка меню переведена на UTF-8 с проверкой русской строки в DLL. В сборку
  добавлены широкий canvas, WideBattle, облака и проверяемый по хешу ресурс
  боевого диалога, а также встроенные ресурсы декоративного фона; обновлены
  README/INSTALL, добавлены GPL-3.0 `LICENSE` и `THIRD_PARTY_NOTICES.txt`.

## All changes since 1.4 — EN

- Video now has one Resolution popup, like the original wrapper. It offers
  monitor-adaptive selection and separates the three `★` stock modes, ten
  validated widescreen modes and window/output size. Auto chooses stock
  DisplaySize on 4:3/5:4 and Hor+ on 3:2 or wider, prioritizing integer scaling;
  manual choices remain authoritative. Widescreen expands view rather than
  stretching output; unsupported game rows are gray on unknown executables.
- The universal wrapper menu is now available on other game builds. Widescreen
  resolution and Widescreen Battle select one of nine validated game layouts by PE `ProductVersion`
  plus a code probe; the remaining address-dependent features are explicitly
  labelled `(MNS/SMNS)` and disabled outside that mod build.
- `GameCanvasMode=0` selects stock DisplaySize, `1` selects manual Hor+, and `2`
  selects monitor-adaptive mode; a missing key defaults to Auto on a supported
  layout. Every logical game size is applied only after a full restart.
- Fixed widescreen startup with 16-bit screen emulation: C4dll-R advertises the
  selected Hor+ mode in DirectDraw and aligns the service geometry for the
  current process only, without rewriting `ddraw.ini`.
- Game resolution is separated from scaling. The long output-preset list was
  replaced with one **Resolution > Window/output...** dialog. Automatic stores `0x0`;
  a custom width of 320–8192 and height of 240–8192 are written to the effective
  `ddraw.ini` section and applied live. This changes output only, never the game
  canvas; borderless always uses the desktop. Game-resolution changes show a
  separate explicit full-restart message.
- Added the DisciplesGL decorative background around fixed-size screens inside
  an active widescreen game canvas, enabled by default. The item is gray in a
  stock native canvas because separate output letter/pillar bars belong to the
  renderer. The built-in PNG screenshot saves the decorated game frame.
- Added automatic Widescreen Battle (enabled by default): a fixed 990-pixel
  layout displays both unit panels with matching buttons, items and hit areas.
  There is no menu switch in 1.5; the layout is selected when the next battle opens.
- Added map clouds through a validated external `Imgs\IsoClouds.ff`. The menu
  controls the game's native `Disciple.ini [Settings] IsoBirds` setting, is
  restart-only, and applies no game hooks when validation fails.
- Fixed fullscreen handling: the menu is hidden in borderless and exclusive,
  and the old flicker/geometry desynchronization is gone. `F4` reliably returns
  to/from a window and `Alt+Enter` stays synchronized with the menu.
- Map drag-scroll now defaults to on for a new setting. Native edge scrolling
  remains available and is suppressed only during a real drag; it no longer
  stops when the cursor crosses the active window edge. The exact button-down
  point is retained, so the first changed game pixel pans immediately without
  a tile-centre re-anchor or initial jump.
- Attack acceleration now ends on the actual completion of the final visual
  part and returns to normal over 300 ms. A safe timed fallback remains for a
  mismatched executable signature.
- The 1 CPU option is now strictly next-start only: the menu no longer
  live-reloads affinity or creates mixed thread state. It remains enabled by
  default.
- Renderer settings now use the effective `ddraw.ini` section; the new dialog
  saves the window size there, and live reload preserves it together with a
  hotkey-selected mode. New configuration follows the game resolution instead
  of forcing 800×600, and known legacy sizes migrate safely.
- The release ZIP now contains all eight selectable OpenGL shaders and their
  required multipass files. The timer uses the correct `TableDuration_0` key;
  it no longer embeds a personal-use-only font and uses the system fallback.
- Timer defaults now target PvP: Force mode, animation and player-vs-player
  combat pauses, and end-turn on expiry without retreat. Reposition it with
  `Ctrl+Alt+LMB`. Automatic expiry no longer opens the confirmation question or
  carries a queued click into the next positive-time turn.
- Menu sources are built as UTF-8 and the DLL is checked for valid Russian text.
  The build now includes the widescreen canvas, Widescreen Battle, map clouds and
  a hash-validated battle-dialog resource plus the embedded decorative assets;
  README/INSTALL were updated, and the GPL-3.0 `LICENSE` plus
  `THIRD_PARTY_NOTICES.txt` were added.
