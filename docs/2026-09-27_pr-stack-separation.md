# Разделение production-исправлений между PR8, PR10 и PR11

Проверено 27 сентября 2026. Перенос выполнен обычными коммитами и merge,
без rebase, force-push, удаления функций или изменения PR7.

## Где искать изменение

| PR | Назначение | Перенесённые изменения |
| --- | --- | --- |
| [8](https://github.com/berkutx/D2ModdingToolset/pull/8) | Подготовленные матчи, без движка ОХ и игрового драйвера | Согласие джойнера на вход, компактное измеряемое подтверждение, пустой optional `MOD_POTION`, отсутствующий optional `turn.lua`, отмена неудачной Detours-транзакции |
| [10](https://github.com/berkutx/D2ModdingToolset/pull/10) | Production ОХ поверх PR8 | Диагностика первого отказа; прежние startup-исправления сохранены; тестовый драйвер не перенесён |
| [11](https://github.com/berkutx/D2ModdingToolset/pull/11) | Игровой тестовый драйвер поверх PR10 | Включает нижние ветки обычным merge; игровая реализация сохранена |

Исходные исправления: join `3bb1416e` (эквивалент `b9119066`), compact
`81e5d5cf`, potion `09e02e9c`, optional turn script из `29967fe2`, Detours
cleanup из `be849872`, first-fault diagnostics `7f1603b3`.

Основные коммиты переноса: PR8 `28750c32` и `9ebb5ce2`; PR10 merge
`5279f0cb` и диагностика `7856acbf`; PR11 merge `b0a39ae4`.
Документация и последующие forward merges могут продвинуть вершины.

## Что специально не переносилось в PR8

- `Connect`, ранние `Refresh`, `BeginTurn`, `JoinGame`, `UpdateObj` уже
  исправлены в production PR10. Их повторный перенос из PR11 не нужен.
- `JoinByFilter_Callback` и чтение optional room OH property обеспечивают
  привязку комнаты к ОХ до native handshake. Это часть ОХ, несмотря на
  расположение в общем `netcustomservice.cpp`.
- `D2_TESTDRV`, локальный тестовый транспорт, автоматизация начальных окон,
  боёв и получения состояния мира остаются исключительно в PR11.
- Модовый `z_unit_effect.lua` не относится к исходникам MSS и не добавлен в PR.

Перенесённая диагностика не меняет допуск пакетов: сохранены `Unhandled`,
проверки поколения pregame, собственного snapshot и apply-fence. Startup-тест
адаптирован к callback с диагностикой. Ошибка диагностического callback не
подавляет существующий terminal/Abort; тесты проверяют первый отказ,
повторный вход, исключение callback и освобождение захваченных данных.

## Проверки

- PR8 Release Win32/v143, без ОХ/драйвера: 0 ошибок, 4 предупреждения C4018.
  DLL SHA-256: `25884877c998f371fa1736fd3e447cf45f065ddf21ab6550c1975e7df6f46fb0`.
- PR10 Release Win32/v143, ОХ включён, драйвер отсутствует: 0 ошибок,
  4 предупреждения C4018. DLL SHA-256:
  `72f9cf8f65282397373fef14d1108333d47251c70c1a42bc6e45ac70e0ed48ea`.
- Prepared protocol/lifecycle/settings, 113 text checks и 6165 cross-wire
  checks прошли; potion Debug 8/8 и Release 8/8; optional turn script и
  четыре отрицательных контроля прошли.
- ОХ: 15/15 control transcripts; port, wire, apply-fence, notification policy,
  bounded diagnostic header и actual lobby startup Debug 19/19, Release 19/19.
- `git diff b32fba56 b0a39ae4 -- mss32` пуст: весь native-код и проект PR11
  побайтово совпадают с прежней вершиной. Это проверка сохранности переноса,
  не новый игровой прогон.

Для воспроизведения использовать `tests/run-prepared-match-protocol.ps1`,
`tests/run-item-potion-fields.ps1`, `tests/run-turn-script-optional.ps1` и
`tests/run-simturns.ps1` в соответствующей ветке, с x86 MSVC environment.
Общие регрессии подключены к CI PR8 и наследуются верхними ветками;
OH-only и harness-only шаги сохранены без повторного запуска общих тестов
в том же build job.

Новой игровой приёмки, новой установки DLL и полной 18-case кампании в этой
задаче не было. Результаты предыдущих реальных двухклиентных проверок следует
читать отдельно от сборок и переносов веток.
