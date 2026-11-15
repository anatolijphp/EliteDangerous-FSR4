# Снятие кадров Elite Dangerous в RenderDoc (этап 1)

## Подготовка (один раз)
1. Запусти игру обычным образом (Epic → Play). В меню игры:
   - Options → Graphics → Display: режим **Fullscreen** или **Borderless**, VR выключен.
   - Options → Graphics → Quality: **Upscaling = AMD FidelityFX Super Resolution (FSR)**,
     **Supersampling = 0.77 (FSR Quality)**.
   - Выйди из игры полностью.

## АКТУАЛЬНЫЙ способ: прокси dinput8.dll (Secure Boot ломает глобальный хук, Epic под RenderDoc виснет)
- В папке игры лежит `dinput8.dll` (исходник `tools\rdproxy\`). Она грузит RenderDoc при старте игры.
- Запускай игру обычным способом через Epic. F12 → снимок сохраняется сам в `D:\ed-fsr\captures\ed_frameN.rdc`.
- Лог: `D:\ed-fsr\captures\rdproxy.log`. Временно выключить: создать файл `D:\ed-fsr\captures\rdproxy.disable`.
- Удалить совсем: `tools\rdproxy\uninstall.ps1`.
- Прокси блокирует оверлей Epic (EOSOVH) — он падает вместе с RenderDoc.
- **Radmin VPN держать выключенным**: иначе игра берёт его адрес 26.x.x.x и не подключается к серверу (SYNC).

## (устарело) Запуск под RenderDoc (глобальный хук)
1. Запусти `D:\tools\renderdoc\qrenderdoc.exe` **от имени администратора**.
2. Tools → Settings → General → включи **Allow global process hooking** → закрой Settings.
3. Вкладка **Launch Application**:
   - Executable Path: `D:\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64\EliteDangerous64.exe`
   - Capture Options: включи **Capture Child Processes**.
   - Нажми **Enable Global Hook** (кнопка внизу).
4. Запусти игру как обычно через Epic. В левом верхнем углу игры должна появиться
   плашка RenderDoc (`D3D11. Frame N. Captures saved: 0`). Если её нет — напиши мне.
5. Как только игра запустилась — в RenderDoc нажми **Disable Global Hook** (чтобы не цеплял лишнее).

## Какие кадры снять (клавиша F12 или PrtScn)
Для каждого — стой в сцене пару секунд, затем жми F12 один раз:
1. `station`  — в ангаре/на площадке станции, видна кабина и интерьер станции.
2. `space`    — обычный полёт у станции/планеты, в кадре другой корабль если есть.
3. `supercruise` — сверхкруиз, видны звёзды и HUD.
4. `planet`   — на поверхности или низко над ней.
5. `onfoot`   — пешком (Odyssey) в поселении/на станции, рядом NPC.
6. `galmap`   — открытая карта галактики.

После каждого F12 в оверлее счётчик `Captures saved` увеличится.

## Сохранение
В qrenderdoc появится окно с подключением к игре и списком снимков.
Сохрани каждый (ПКМ → Save) в `D:\ed-fsr\captures\` с именами
`station.rdc`, `space.rdc`, `supercruise.rdc`, `planet.rdc`, `onfoot.rdc`, `galmap.rdc`.

Потом скажи мне — я запущу `D:\ed-fsr\tools\analyze.ps1` и разберу результаты.

## Если глобальный хук не сработал
Пока игра запущена обычным способом, выполни в PowerShell:
```
(Get-CimInstance Win32_Process -Filter "Name='EliteDangerous64.exe'").CommandLine | Out-File D:\ed-fsr\captures\cmdline.txt
```
Я попробую запустить игру с этими аргументами напрямую через `renderdoccmd capture`.
