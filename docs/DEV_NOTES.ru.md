# ED-FSR — заметки разработки

Цель: заменить встроенный в Elite Dangerous FSR 1 (EASU+RCAS, пространственный) на временной апскейлер
FSR 4 (ML, RDNA 4) / FSR 3.1 и добавить FSR Frame Generation. Публичное описание — `README.md` (EN).

## Факты об игре (проверено 2026-11-15)
- `EliteDangerous64.exe` (Odyssey, Epic), версия 2026.10.23.334925. API: только `d3d11.dll` (`D3D11CreateDevice`)
  и `dxgi.dll` (`CreateDXGIFactory1`).
- Режимы апскейла в exe: `UPSCALINGMODE_NORMAL / FFXFSR / FFXCAS`. TAA и экранных motion vectors в игре нет.
- Тестовый GPU: RX 9070 XT (RDNA 4) → провайдеры FSR 4.1.2 (апскейл) и FSR FG 4.0.2 (генерация кадров).

## Этапы
| # | Этап | Статус |
|---|------|--------|
| 1 | Карта кадра: FSR1-проход, depth, матрицы камеры (RenderDoc) | ✅ готово, GO |
| 2 | Каркас: прокси `dinput8.dll`, хуки D3D11, подмена EASU (PoC) | ✅ готово, проверено в игре |
| 3 | Временной апскейл: мост D3D11→D3D12, FidelityFX FSR 4 / 3.1, jitter, MV камеры | ✅ готово, проверено в игре |
| 4 | Качество: reactive mask для HUD/прозрачных (в работе), MV движущихся объектов | 🚧 в работе |
| 5 | FSR Frame Generation через FFX proxy swapchain | 🧪 работает на стенде, в игре не проверено |
| 6 | Оверлей настроек (CTRL+F8, стиль ED), сохранение в ini | ✅ работает в игре (SAVE пишет ini) |
| 7 | Репозиторий и CI (GitHub Actions) | ✅ github.com/anatolijphp/EliteDangerous-FSR4 |

### Этап 2 — каркас
- `dinput8.dll` — прокси системной DirectInput (игра импортирует `DINPUT8.dll`), грузится раньше создания устройства.
- IAT-хук `D3D11CreateDevice` в exe → vtable-хуки: `CreateComputeShader`(18), `CSSetShader`(69),
  `CSSetShaderResources`(67), `CSSetUnorderedAccessViews`(68), `OMSetRenderTargets`(33/34), `RSSetViewports`(44).
- EASU опознаётся по FNV-64 байткода (`0xf4335f6d4ecad949`, 38208 байт).
- **Dispatch/Draw не хукать**: runtime сам переписывает эти слоты vtable (fast/validated path) — проверено.

### Этап 3 — апскейл (`src/fsr.cpp`)
- EASU подменяется no-op шейдером; входы t0/u0 ловятся хуками SRV/UAV (игра привязывает их **после** `CSSetShader`;
  null-отвязки игнорируются). FSR выполняется на следующем `CSSetShader` (RCAS) и пишет в выход EASU.
- Общие текстуры создаются в D3D11 (`SHARED_NTHANDLE`) и открываются в D3D12 (обратное направление → E_INVALIDARG);
  синхронизация — общий fence (D3D11 Signal → D3D12 Wait/Signal → D3D11 Wait).
- Jitter — субпиксельный сдвиг viewport, пока привязан depth сцены. MV — из depth + viewProj (b1@4320) + позиции
  камеры (b1@4400); пиксели ближе `cockpit_depth` (кабина) = 0.
- Версия FFX выбирается через `ffxOverrideVersion` (FSR 4 / FSR 3.1), переключается на лету.

### Этап 4 — качество (в работе)
- Reactive mask: по шейдеру-триггеру (ini `reactive_trigger`, FNV-64 байткода VS/PS) снимается копия HDR-цели в момент
  конца непрозрачной геометрии; перед апскейлом маска = относительная разница «финальный HDR − снимок» → `reactive` в FFX.
  Ini: `reactive_mask`, `reactive_scale`. Хеши шейдеров в игре можно выписать с `EDFSR_LOG_SHADERS=1`.
- MV движущихся объектов: исследуется раскладка буфера объектов t33 (stride 336).

### Важно: не использовать `SwapDeviceContextState`
- Runtime D3D11 при каждом переключении состояния переписывает слоты vtable immediate-контекста (два набора реализаций),
  из-за чего хуки CSSetShader/OM/RS слетали каждый кадр и FSR не включался. Оверлей рисуется со стандартным
  бэкапом состояния ImGui; в `mod_verify_hooks` есть сторож, который перехукивает переписанные слоты.

### Этап 5 — генерация кадров (`src/fg.cpp`)
- Хук `CreateDXGIFactory1` → `CreateSwapChain`(10) / `CreateSwapChainForHwnd`(15). Swapchain игры уводится на скрытое
  окно, на настоящем окне — FFX proxy swapchain (D3D12).
- На `Present`: копия backbuffer → shared → backbuffer прокси; FG prepare (depth, MV, камера b1@3664/4400); Present прокси.
- Эксклюзивный fullscreen игнорируется — в игре нужен Borderless. Пока открыта панель настроек, FG выключен.

### Этап 6 — оверлей (`src/ui.cpp`, Dear ImGui 1.91.8)
- Тост при запуске «Press CTRL+F8 to configure FSR». CTRL+F8 / ESC — панель: апскейлер (FSR 4 / FSR 3.1 / FSR1 игры),
  резкость, FG, advanced (jitter, MV, cockpit depth, debug view), статус, SAVE в `edfsr.ini`.
- Пока панель открыта, ввод в игру блокируется (WndProc, DirectInput клавиатура/мышь, SetCursorPos/ClipCursor).

## Карта кадра (2560x1440, SSAA 0.67 → рендер 1715x964)
Конец кадра (одинаков во всех сценах, хеши шейдеров стабильны):
1. Сцена + **HUD/голограммы кабины в рендер-разрешении** (до апскейла), HDR R11G11B10.
2. Tonemap PS `2c77020ebfb2210b` → RGBA8 1715x964.
3. SMAA: `a78b0b9294bca536` → `c506cba5af0ba614` → `7e65d0741cd1fa15`.
4. **FSR1 EASU** CS `2f7d47465d50053c`: 1715x964 → 2560x1440 (dispatch 160x90, 64 потока, тайл 16x16).
5. **FSR1 RCAS** CS `e1d2250284c19f31`.
6. Копия в backbuffer + 1 мелкий draw.

- **Depth сцены**: R32G8X24_TYPELESS (D32S8) в рендер-разрешении, BindFlags DEPTH_STENCIL|SHADER_RESOURCE,
  **reversed-Z, infinite far, near = 0.025**. Stencil — классы материалов, **не** маска кабины.
- **Per-frame cbuffer** (VS/PS b1, ≥4480 байт): `3664` — поворот камеры 3x3 (столбцы: right/up/forward),
  `4320` — viewProj (row-vector, camera-relative), `4400` — позиция камеры.
  Меш-шейдеры: `clip = mul(float4(pos - cb1[275], 1), cb1[270..273])`, позиции объектов из structured buffer t33.
- **Отдельной матрицы кабины нет** (смещения 656/720 никто не читает). Звёзды/скайбокс/HUD-голограммы берут матрицу
  из b0 (64..127) — поэтому jitter сделан сдвигом viewport, а не правкой матрицы.
- Полноэкранные проходы освещения тоже идут с depth сцены → получают тот же сдвиг jitter (возможный риск артефактов).

## Окружение
- Инструменты в `D:\tools`: RenderDoc 1.47, llvm-mingw 20261020, FidelityFX SDK 2.4.0, Dear ImGui 1.91.8, Python 3.12.
- Secure Boot → глобальный хук RenderDoc не работает; снимки делаются с `renderdoc=1` в ini (оверлей Epic при этом
  блокируется — он падает вместе с RenderDoc). Инструкция: `docs/CAPTURE.ru.md`.
- Radmin VPN ломает подключение ED к серверу (игра берёт адрес 26.x) — держать выключенным.

## Сборка и тесты
- `sh build.sh` → `build/dinput8.dll`; dev-сборка `OUT=build/dev sh build.sh`.
- Стенд: `tools/harness/dx11test.exe --edswap --show --vsync 600` с переменными
  `EDFSR_PROXY=<path>\dinput8.dll`, `EDFSR_EASU_FNV=0x8147ea2c8f57b26d`, `EDFSR_FRAMEGEN=1`;
  `EDFSR_UI_OPEN=1` — открыть панель, `EDFSR_UI_DUMP=<file.bmp>` — снимок кадра с оверлеем.
- Лог мода: `D:\ed-fsr\logs\edfsr.log`.
- CI: GitHub Actions (`.github/workflows/build.yml`), артефакт `ed-fsr`; теги `v*` → релиз.
