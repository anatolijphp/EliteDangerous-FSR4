# ED-FSR — FSR 3.1 / FSR 4 для Elite Dangerous

Цель: заменить встроенный FSR 1 (EASU+RCAS, пространственный) на временной апскейлер
FSR 3.1, а на RDNA 4 — FSR 4 (через драйверную подмену DX12 FidelityFX DLL).

## Факты (проверено 2026-11-15)
- Игра: `D:\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64\EliteDangerous64.exe`,
  версия 2026.10.23.334925 (Epic).
- API: только `d3d11.dll` + `dxgi.dll` (d3d12 не импортируется).
- Режимы апскейла в exe: `UPSCALINGMODE_NORMAL / FFXFSR / FFXCAS`,
  множители `MULTIPLIER_FSR_{ULTRA_QUALITY,QUALITY,BALANCED,PERFORMANCE}`, cbuffer `c_ffxConst`.
- Экранных motion vectors / TAA не найдено (строки `*MotionVector` — flipbook-анимация текстур).
- GPU: AMD Radeon RX 9070 XT (RDNA 4) → FSR 4 возможен.

## Инструменты (D:\tools)
- `renderdoc\` — RenderDoc 1.47 portable (подпись проверена)
- `llvm-mingw-20261020-ucrt-x86_64\` — clang/mingw тулчейн
- `dxc\` — DirectX Shader Compiler
- `python\` — Python 3.12

## Структура
- `CAPTURE.md` — инструкция снятия кадров в игре
- `tools\analyze_capture.py` — анализатор .rdc (запуск внутри qrenderdoc --py)
- `tools\analyze.ps1` — прогон анализатора по всем `captures\*.rdc`
- `tools\testapp\` — тестовое DX11-приложение (проверка анализатора)
- `captures\` — снимки и результаты (`<name>_analysis\summary.txt`, `frame.json`, `img\`)

## План
1. **Карта кадра** (идёт): FSR1-проход, depth, матрицы камеры, порядок HUD/постэффектов. Go/no-go.
2. Каркас: аддон ReShade 6 (или proxy d3d11/dxgi). Проверка совместимости с EDHM.
3. Подмена FSR1: jitter в проекции, генерация MV из depth+матриц (камера), маски;
   сначала FSR 2.2 DX11, затем мост DX11→DX12 (shared NT handles + fences) → FSR 3.1 / FSR 4.
4. MV для движущихся объектов, reactive mask.
5. (Опц.) Frame Generation; до этого — AFMF 2 драйвера.
6. Конфиг, hotkey, фолбэк на родной FSR1 при несовпадении хешей.

## Карта кадра (снимки 2026-11-15, 2560x1440, SSAA 0.67 → рендер 1715x964, AAMode=4 SMAA)
Конец кадра (одинаков во всех сценах, хеши шейдеров стабильны):
1. Сцена + **HUD/голограммы кабины рисуются в рендер-разрешении** (до апскейла) в HDR R11G11B10.
2. Tonemap: PS `2c77020ebfb2210b` → RGBA8 (LDR) 1715x964.
3. SMAA: edge `a78b0b9294bca536` → weights `c506cba5af0ba614` → blend `7e65d0741cd1fa15`.
4. **FSR1 EASU: CS `2f7d47465d50053c`**, dispatch 160x90, 1715x964 RGBA8 → 2560x1440 RGBA8.
5. **FSR1 RCAS: CS `e1d2250284c19f31`**, 2560x1440 → 2560x1440.
6. Копия в backbuffer (PS `d7dc2158fc97dab9`) + 1 мелкий draw на полном разрешении.
- **Основной depth**: D32S8_TYPELESS 1715x964 (стабильный ResourceId 4488 в снимках), **reversed-Z, infinite far, near=0.025**.
- Отдельный depth 2560x1440 D32S8 — только для пост-апскейл draw.

Per-frame cbuffer (VS/PS slot **b1**, 4480 байт), смещения в байтах:
- `3664` — view rotation 3x3 (row-major), `3728` — view 3x4 с переносом.
- `3792` — inverse projection в UV-пространстве: `1/sx=0.94259`, `1/sy=0.53021` (FOV v≈56°), строка `0 0 40 0` (=1/near).
- `4320` — viewProj (camera-relative, column-major), z-столбец 0, w-строка `0 0 0.025 0` (reversed infinite).
- `656`, `720` — ещё два viewProj с переносом (разный масштаб FOV — вероятно кабина/мир), проверить.
- Життера нет (проекция центрирована).

Выводы для мода:
- Входы для FSR: color = вход EASU (LDR, после tonemap+SMAA; лучше брать HDR до tonemap), depth 4488, матрицы из b1.
- Життер: сдвигать проекцию в b1 (3792/4320/656/720) на субпиксель — нужно найти, откуда VS берут проекцию.
- MV: камера + отдельно кабина (кабина неподвижна относительно камеры, мир движется) — проверить stencil как маску кабины.
- HUD в сцене → нужен reactive/transparency mask на draw-вызовы HUD, иначе шлейфы.
- SMAA при временном апскейле отключать (AAMode).

## Статус
- [x] Тулчейн и RenderDoc на D:
- [x] Анализатор проверен на тестовом DX11 кадре
- [x] Кадры из игры: captures\station / space / supercruise / galmap .rdc
- [x] Разбор кадров — **GO**: FSR1-проход, depth и матрицы найдены, смещения в b1 одинаковы во всех 4 сценах
- [x] Этап 2 (код): `src\edfsr.cpp` → `build\dinput8.dll` (`sh build.sh`), конфиг `edfsr.ini`, лог `logs\edfsr.log`,
  установка `install.ps1` / `install.ps1 -Uninstall`. Ctrl+F8 — переключение ED-FSR ↔ оригинальный FSR1.
  Схема: IAT-хук D3D11CreateDevice → vtable-хуки CreateComputeShader(18) и CSSetShader(69);
  EASU опознаётся по FNV-64 байткода (0xf4335f6d4ecad949, 38208 байт) и подменяется своим CS с той же раскладкой.
  **Dispatch не хукать**: runtime сам переписывает слот Dispatch в vtable (fast/validated path) — проверено.
- [x] Этап 2 (проверка в игре): PoC-подмена работает в живой игре (~220 fps, Ctrl+F8 ок)
- [~] Этап 3: мост FSR (`src\fsr.cpp`): D3D11 → shared textures (создаются в D3D11 с SHARED_NTHANDLE, открываются в D3D12;
  обратное направление даёт E_INVALIDARG) + shared fence → `ffxDispatch` → обратно. FidelityFX SDK 2.4.0 в
  `D:\tools\fidelityfx-sdk-2.4.0`, DLL копируются в `D:\ed-fsr\bin`. **На RX 9070 XT провайдер = FSR 4.1.2.**
  Джиттер: сдвиг viewport пока привязан depth сцены. MV: из depth + viewProj (b1@4320) + camPos (b1@4400), кабина (z<cockpit_depth) = 0.
  EASU → no-op CS; FSR выполняется на следующем CSSetShader (RCAS) и пишет в выход EASU.
  Dev-сборки: `OUT=build/dev sh build.sh` (build\dinput8.dll может быть занят тестами).
- [~] Этап 5: Frame Generation (`src\fg.cpp`), ini `framegen=1`, Ctrl+F9. Хук CreateDXGIFactory1 → factory
  CreateSwapChain(10)/ForHwnd(15): swapchain игры уводится на скрытое окно, на настоящем окне — FFX proxy swapchain (D3D12).
  Present: копия backbuffer → shared → backbuffer прокси, FG prepare (depth/MV/камера из b1@3664/4400), Present прокси.
  Эксклюзивный fullscreen игнорируется — в игре нужен Borderless.
- [~] Оверлей настроек (`src\ui.cpp`, Dear ImGui, стиль ED): тост при запуске "Press CTRL+F8 to configure FSR",
  CTRL+F8 — панель (апскейлер FSR4/FSR3.1/игровой FSR1, резкость, FG, advanced, SAVE в edfsr.ini).
  Старые хоткеи Ctrl+F8/Ctrl+F9 (переключатели) удалены. Present хукается всегда (не только при FG). На RX 9070 XT провайдер FG = 4.0.2 (ML).
  Тестовый стенд: `tools\harness\dx11test.exe --edswap --show --vsync 600` с EDFSR_FRAMEGEN=1,
  EDFSR_EASU_FNV=0x8147ea2c8f57b26d, EDFSR_PROXY=D:\ed-fsr\build\dev\dinput8.dll.
- Нюансы окружения: Secure Boot → глобальный хук RenderDoc не работает; оверлей Epic (EOSOVH) падает с RenderDoc
  (прокси его блокирует); Radmin VPN ломает подключение к серверу (адрес 26.x).
