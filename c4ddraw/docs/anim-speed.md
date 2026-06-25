# Animation speed controls (C4dll-R)

All animation-speed options live in the in-game **Game** menu and are implemented in
[`features/featuremenu.cpp`](../features/featuremenu.cpp). Settings persist to `C4menu.ini`
(`[menu]` section), separate from the game's own `Disciple.ini` / `settings.lua`. Addresses are for
the Russobit "slasher_mns_2_4" build.

## Two independent mechanisms

**A. Live virtual clock (global).** The game advances animation frames when the WINMM `timeGetTime`
clock passes the next frame deadline (the "slow ~66ms / fast ~33ms" update lists). We hook the
`timeGetTime` IAT slot (`0x6CE420`) with `timeGetTimeHook`, which multiplies elapsed time by
`factor/10`. `factor` is picked by `g_inBattle`: `g_battleFactor` in battle, `g_mapFactor` on the map.
`g_inBattle` is latched by the battle-viewer vftable discriminator (`installBattleDiscriminator`,
vftable `0x6F4294`). This scales **everything** in the active context, so it cannot tell the hero walk
from fog/buildings, nor an attack from idle. Factor table `kAnimFactor[]` = `{15,20,30,40,50,150}`
(x1.5/x2/x3/x4/x5/x15); `10` = identity.

**B. Native `GameSettings` fields.** The game already has per-category speed fields. `gameSettings()`
walks `CMidgardApi::instance()` (`0x401d35`, __cdecl) -> `CMidgard.data @+8` ->
`CMidgardData.settings @+60` (a `GameSettings**`). `setNativeSpeed()` writes the field + the matching
`Disciple.ini` key. Field offsets (confirmed from the `Disciple.ini` loader `sub_61A3AF`):

| Field | Offset | Disciple.ini key | Affects |
| --- | --- | --- | --- |
| `playerSpeed` | `+0x168` | `PlayerSpeed` | YOUR stack walk on the map only |
| `opponentSpeed` | `+0x16C` | `OpponentSpeed` | AI stack walk |
| `battleSpeed` | `+0x174` | `BattleSpeed` | whole battle (single value, no idle/attack split) |

## The menu controls

| Game-menu item | Mechanism | Code | Notes |
| --- | --- | --- | --- |
| Battle animation (live x1.5..x15) | A, `g_battleFactor` | `applyAnimSpeed(0,…)` | all battle anim |
| Map animation (live x1.5..x15) | A, `g_mapFactor` | `applyAnimSpeed(1,…)` | all map anim incl. hero |
| Battle speed (Slow/Normal/Fast/Instant) | B, `battleSpeed` | `setNativeSpeed(0,…)` | applies next battle |
| Map turn speed (Normal/Fast/Very fast) | B, `playerSpeed`+`opponentSpeed` | `setNativeSpeed(1,…)` | hero/AC walk only |
| Battle attack burst (off / 1.5x..5x) | A, gated | `updateBattleBurst()` | fast hits, calm idle |

To change only the hero's map walk without touching other map animation, use **Map turn speed**
(mechanism B), not Map animation (mechanism A).

## Battle attack burst (idle calm, hits fast)

Goal: keep idle units calm but speed up attacks. There is no native idle/attack split, so we drive
mechanism A from a live "attack playing" signal.

- **Signal.** Battle-viewer vftable `0x6F4294` slot `[2]` (`showAttackEffect`, `0x63203B`) calls
  `sub_639743(1)` when an attack effect begins; the per-frame update (slot `[1]`, `0x630DE3`) calls
  `sub_639743(0)` to reset. So slot `[2]` fires once per hit/effect. Our `batShowThunk` (already
  patched into that slot for `g_inBattle`) also sets `g_attackPulse = 1`.
- **Window + ramp.** The 32ms `WM_TIMER` pump calls `updateBattleBurst()`. Each pulse arms a
  `kAttackHoldMs` (1200ms) window at the attack factor; after it, the factor eases linearly back to
  the idle base over `kAttackRampMs` (700ms) instead of snapping. The idle base is the Battle animation
  setting (vanilla `10` unless set), so between attacks idle stays calm.
- **Limitation.** Because mechanism A is the global clock, idle units also speed up *during* the burst
  window. The split is "calm between attacks", not true per-unit isolation. See `BACKLOG.md`.

## Code map (`features/featuremenu.cpp`)

| Symbol | Role |
| --- | --- |
| `timeGetTimeHook`, `installTimeScaleHook` | virtual clock over IAT `0x6CE420` |
| `installBattleDiscriminator`, `batUpdateThunk`/`batShowThunk`/`batEndThunk`/`batDtorThunk` | latch `g_inBattle`; `batShowThunk` also sets `g_attackPulse` |
| `kAnimFactor[]`, `applyAnimSpeed` | speed 1..6 -> clock factor |
| `gameSettings`, `setNativeSpeed`, `readNativeSpeeds` | native `GameSettings` + `Disciple.ini` |
| `updateBattleBurst` | attack-burst window + ease-down, run from the `WM_TIMER` pump |
| `persist`, `loadSettings` (`C4menu.ini`), `onMenuCommand`, `refreshChecks` | menu state |

---

# Управление скоростью анимации (C4dll-R)

Все опции скорости анимации находятся во внутриигровом меню **Game** и реализованы в
[`features/featuremenu.cpp`](../features/featuremenu.cpp). Настройки сохраняются в `C4menu.ini`
(секция `[menu]`), отдельно от собственных `Disciple.ini` / `settings.lua` игры. Адреса даны для
сборки Russobit "slasher_mns_2_4".

## Два независимых механизма

**A. Живые виртуальные часы (глобально).** Игра продвигает кадры анимации, когда часы WINMM
`timeGetTime` проходят дедлайн следующего кадра (списки обновления "медленно ~66мс / быстро ~33мс").
Мы хукаем слот IAT `timeGetTime` (`0x6CE420`) через `timeGetTimeHook`, который умножает прошедшее
время на `factor/10`. `factor` выбирается по `g_inBattle`: `g_battleFactor` в бою, `g_mapFactor` на
карте. `g_inBattle` ставится дискриминатором vftable боевого вьюера (`installBattleDiscriminator`,
vftable `0x6F4294`). Это масштабирует **всё** в активном контексте, поэтому нельзя отличить шаг героя
от тумана/строений или атаку от idle. Таблица `kAnimFactor[]` = `{15,20,30,40,50,150}`
(x1.5/x2/x3/x4/x5/x15); `10` = без изменений.

**B. Нативные поля `GameSettings`.** У игры уже есть свои поля скорости по категориям. `gameSettings()`
идёт по `CMidgardApi::instance()` (`0x401d35`, __cdecl) -> `CMidgard.data @+8` ->
`CMidgardData.settings @+60` (это `GameSettings**`). `setNativeSpeed()` пишет поле + соответствующий
ключ `Disciple.ini`. Смещения полей (подтверждены из загрузчика `Disciple.ini` `sub_61A3AF`):

| Поле | Смещение | Ключ Disciple.ini | На что влияет |
| --- | --- | --- | --- |
| `playerSpeed` | `+0x168` | `PlayerSpeed` | только шаг ВАШИХ отрядов по карте |
| `opponentSpeed` | `+0x16C` | `OpponentSpeed` | шаг отрядов AI |
| `battleSpeed` | `+0x174` | `BattleSpeed` | весь бой (одно значение, без разделения idle/атака) |

## Пункты меню

| Пункт меню Game | Механизм | Код | Заметки |
| --- | --- | --- | --- |
| Battle animation (live x1.5..x15) | A, `g_battleFactor` | `applyAnimSpeed(0,…)` | вся боевая анимация |
| Map animation (live x1.5..x15) | A, `g_mapFactor` | `applyAnimSpeed(1,…)` | вся анимация карты, включая героя |
| Battle speed (Slow/Normal/Fast/Instant) | B, `battleSpeed` | `setNativeSpeed(0,…)` | применяется со след. боя |
| Map turn speed (Normal/Fast/Very fast) | B, `playerSpeed`+`opponentSpeed` | `setNativeSpeed(1,…)` | только шаг героя/AI |
| Battle attack burst (off / 1.5x..5x) | A, по сигналу | `updateBattleBurst()` | быстрые удары, спокойный idle |

Чтобы менять только шаг героя по карте, не задевая остальную анимацию карты, используйте **Map turn
speed** (механизм B), а не Map animation (механизм A).

## Battle attack burst (idle спокоен, удары быстрые)

Цель: оставить idle-юнитов спокойными, но ускорить атаки. Нативного разделения idle/атака нет, поэтому
мы управляем механизмом A по живому сигналу "идёт атака".

- **Сигнал.** Слот `[2]` vftable боевого вьюера `0x6F4294` (`showAttackEffect`, `0x63203B`) вызывает
  `sub_639743(1)` в начале эффекта атаки; покадровый апдейт (слот `[1]`, `0x630DE3`) вызывает
  `sub_639743(0)` для сброса. То есть слот `[2]` срабатывает раз на удар/эффект. Наш `batShowThunk`
  (уже стоит в этом слоте ради `g_inBattle`) дополнительно ставит `g_attackPulse = 1`.
- **Окно + плавность.** Пул `WM_TIMER` (32мс) зовёт `updateBattleBurst()`. Каждый пульс взводит окно
  `kAttackHoldMs` (1200мс) на факторе атаки; после него фактор линейно возвращается к базе idle за
  `kAttackRampMs` (700мс), без мгновенного скачка. База idle = настройка Battle animation (vanilla
  `10`, если не задано), поэтому между атаками idle остаётся спокойным.
- **Ограничение.** Так как механизм A это глобальные часы, idle-юниты тоже ускоряются *во время* окна
  burst. Это разделение "спокойно между атаками", а не настоящая пер-юнит изоляция. См. `BACKLOG.md`.

## Карта кода (`features/featuremenu.cpp`)

| Символ | Роль |
| --- | --- |
| `timeGetTimeHook`, `installTimeScaleHook` | виртуальные часы над IAT `0x6CE420` |
| `installBattleDiscriminator`, `batUpdateThunk`/`batShowThunk`/`batEndThunk`/`batDtorThunk` | ставят `g_inBattle`; `batShowThunk` также ставит `g_attackPulse` |
| `kAnimFactor[]`, `applyAnimSpeed` | скорость 1..6 -> фактор часов |
| `gameSettings`, `setNativeSpeed`, `readNativeSpeeds` | нативный `GameSettings` + `Disciple.ini` |
| `updateBattleBurst` | окно burst атаки + плавный спад, из пула `WM_TIMER` |
| `persist`, `loadSettings` (`C4menu.ini`), `onMenuCommand`, `refreshChecks` | состояние меню |
