GMOD ESP + CHAMS x64 / INJECTED DLL / F11 MENU
=============================================

Файлы: GModESP.cpp, Config.hpp, CMakeLists.txt, .github/workflows/build.yml

СБОРКА ЧЕРЕЗ GITHUB ACTIONS
1. Распакуй архив.
2. Загрузи все файлы и папку .github в корень репозитория GitHub.
   Если там остался старый build.yml от 32-битной версии, замени его.
3. Открой Actions -> Build GMod ESP Chams x64 -> Run workflow.
4. Открой успешную сборку -> Summary -> Artifacts -> GMod-ESP-Chams-x64-F11.
5. В ZIP появится GModESP_Chams_x64.dll.

УПРАВЛЕНИЕ
F11 — открыть/скрыть меню в кадре Garry's Mod (ImGui).
F1 — переключить ESP.
F2 — переключить Chams.
F3 — фильтр по командам.
Меню: вкладки ESP / CHAMS / CONFIG. Клик мышью при доступном курсоре.

ФУНКЦИИ
- ESP: прямоугольники игроков, имена, полосы здоровья и HP.
- Chams: двухпроходная цветная отрисовка игроков; цвета через меню.
- CONFIG: текущий статус интерфейсов, найденные netvars, кнопка пересканирования.
- Смещения игроков считываются при запуске из RecvTable, а не берутся из x86 client.dll.

ТЕХНИЧЕСКОЕ
- Строго x64 Windows, D3D9 / D3D9Ex, Source Engine.
- Внутрипроцессный DirectX 9 hook (MinHook), интерфейсы Source Engine,
  Dear ImGui Win32/DX9. Загружается в 64-битный процесс Garry's Mod.
- DLL нельзя безопасно выгружать вручную во время работы процесса: завершай игру.
- Для работы интерактивного меню игра должна предоставлять курсор в окне;
  резервное управление ESP/CHAMS доступно через F1/F2/F3.
- Присланные x64 engine.dll/client(1).dll/materialsystem.dll/stdshader_dx9.dll
  проверены по PE-заголовкам и строкам интерфейсов. См. BINARY_CHECK.txt.
- x86 смещения из старого файла НЕ используются; игроки ищутся через RecvTable.
- На момент подготовки проекта индексы виртуальных функций движка
  не подтверждены запуском игры. Не гарантия работы ESP/Chams.
- Не содержит обхода античита; работа в Garry's Mod не проверена.
- Зависимости ImGui 1.91.9b и MinHook 1.3.4 скачиваются CMake при сборке.

ЛОКАЛЬНАЯ СБОРКА (Developer PowerShell for Visual Studio 2022):
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel

РЕЗУЛЬТАТ:
build/Release/GModESP_Chams_x64.dll

ОТЛАДКА
После инжекта проверь файл %%TEMP%%\GModESP_x64_F11.log: результат
CreateInterface, обнаруженные сетевые поля, установку перехватов DX9.
