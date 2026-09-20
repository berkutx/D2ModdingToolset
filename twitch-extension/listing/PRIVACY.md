# Disciples II Battle Info — Privacy notice / Конфиденциальность

Проект / Maintainer: **D2 Battle Info**. Дата документа / Document date: **20 сентября 2026 / 20 September 2026**. Относится к / Covers: Twitch extension **0.0.1**, Twitch Stat in **C4dll-R 2.0**.

## Русский

**Для чего нужны данные.** Disciples II Battle Info показывает зрителям карточки бойцов текущего боя. Плагин Twitch Stat читает игровые сведения на компьютере стримера. После нажатия «Подключить игру» панель расширения передаёт снимки через Twitch зрителям канала.

**Что передаётся.** Снимок содержит имена и описания бойцов, параметры, атаки, способности и эффекты, расположение рамок, размеры игрового изображения, время снимка, идентификатор боя и PID игрового процесса. Игровые тексты могут содержать имена, заданные игроком. Эти данные предназначены для показа зрителям и не являются приватными после передачи на канал. Сам плагин не захватывает видео, микрофон или изображение рабочего стола.

**Как данные проходят через систему.** Плагин обслуживает локальное соединение только по `127.0.0.1:8765`. Окно соединения передаёт снимок панели Twitch; панель использует Twitch Extension Helper и PubSub. Интерфейс расширения загружается с Twitch CDN. В браузере зрителя карточки и временный буфер снимков обрабатываются локально. Внешнего сервера или базы данных, куда автор расширения автоматически получает и сохраняет эти снимки, нет.

Для каждого подключения создаётся случайный идентификатор данного соединения (`nonce`). Он проверяет сообщения между локальным окном и панелью, присутствует в адресе локального окна и не включается в рассылку зрителям. PID — номер игрового процесса; он входит в снимок и помогает определить подключённую игру.

**Данные зрителей.** Расширение не запрашивает привязку личности Twitch и не использует имена или идентификаторы зрителей для своих функций. Собственный код расширения не отправляет автору события наведения, не включает аналитику и не создаёт cookies, localStorage или отдельные учётные записи. Twitch Helper обеспечивает авторизацию расширения; оверлей использует также сообщаемые Twitch размер видео и задержку трансляции. Работа самого Twitch, включая обработку сетевых и учётных данных, регулируется [политикой конфиденциальности Twitch](https://legal.twitch.com/en/legal/privacy-notice/). Это уведомление не утверждает, что Twitch не получает IP-адреса или другие данные.

**Что остаётся на компьютере стримера.** Плагин записывает последнее состояние в `TwitchStat-live-<PID>.json` рядом с игрой, используя временный файл с окончанием `.tmp`. Снимок перезаписывается, но эти файлы не удаляются автоматически при выходе. При включённой диагностике или профилировании записи плагина добавляются в `C4plugins.log` и вывод отладчика Windows. Настройки сохраняются в `C4Plugins.ini`. Эти файлы не отправляются автору автоматически; их можно удалить после закрытия игры. Удаление INI сбросит соответствующие настройки.

**Как остановить передачу.** Нажмите «Остановить» в панели Twitch или закройте окно соединения. Чтобы прекратить локальный сбор, выключите Twitch Stat в меню игры либо закройте игру. Уже полученные зрителями данные нельзя отозвать; автор не управляет сроками хранения данных на стороне Twitch. Чтобы удалить расширение с канала, используйте раздел «Мои расширения» Twitch.

## English

**Purpose.** Disciples II Battle Info shows viewers cards for units in the current battle. The Twitch Stat plugin reads game information on the broadcaster's computer. After the broadcaster selects “Connect game,” the extension's control panel sends snapshots through Twitch to the channel's viewers.

**Data sent.** A snapshot includes unit names and descriptions, statistics, attacks, abilities and effects, portrait positions, game image dimensions, timestamps, a battle identifier and the game process ID (PID). Game text may include player-defined names. This information is intended for viewers and is not private once broadcast to the channel. The plugin itself does not capture video, microphone audio or the desktop.

**Data flow.** The plugin serves a local connection only at `127.0.0.1:8765`. Its connection window passes snapshots to the Twitch control panel, which uses the Twitch Extension Helper and PubSub. Twitch CDN hosts the extension interface. Cards and a temporary snapshot buffer are processed in each viewer's browser. There is no external author-operated server or database that automatically receives and stores these snapshots.

Each connection generates a random session identifier (`nonce`) to check messages between the local window and control panel. It appears in the local window's URL and is not included in broadcasts to viewers. The PID identifies the game process; it is included in snapshots and helps identify the connected game.

**Viewer data.** The extension does not request Twitch identity linking or use viewer names or identifiers for its features. Its own code does not send hover activity to the author, include analytics, or create cookies, localStorage entries or separate accounts. Twitch Helper handles extension authorization; the overlay also uses Twitch-provided video dimensions and broadcast latency. Twitch's own processing of network and account information is governed by the [Twitch Privacy Notice](https://legal.twitch.com/en/legal/privacy-notice/). This notice does not claim that Twitch receives no IP addresses or other data.

**Local files.** The plugin writes the latest state to `TwitchStat-live-<PID>.json` beside the game, using a temporary `.tmp` file. The snapshot is replaced during operation, but these files are not automatically deleted on exit. When diagnostics or profiling is enabled, plugin messages are appended to `C4plugins.log` and Windows debugger output. Settings are saved in `C4Plugins.ini`. These files are not automatically uploaded to the author and can be removed after closing the game. Removing the INI resets the corresponding settings.

**Stopping.** Select “Stop” in the Twitch control panel or close the connection window. Disable Twitch Stat in the game menu or close the game to stop local collection. Data already received by viewers cannot be recalled; the author does not control Twitch's retention. To remove the extension from a channel, use Twitch's My Extensions page.
