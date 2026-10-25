# DMC5 Split-Screen - v3 (reconstruccion con diagnostico fiable)

## Que cambio y por que
| Hallazgo (con evidencia) | Consecuencia |
|---|---|
| `main.cpp` del repo era una copia de `common.hpp`: **no tenia `reframework_plugin_initialize` ni manejador de F7**. `ss::log` estaba definido dos veces con cuerpos distintos (ODR). | Reescrito: un solo logger (`common.hpp`), un `main.cpp` real. |
| Tu `dmc5_diagnostico.txt` empieza con "Plugin cargado (F7 ... F10 ...)" y trae un volcado completo: la version anterior SI funcionaba. | El repo actual es una regresion; la DLL instalada puede ser distinta del codigo. El banner de arranque lo demuestra (ver abajo). |
| `via.Camera` **no tiene setter de viewport** (solo FOV, planos, AspectRatio, LookAt, matrices). | Se elimina todo el codigo de viewport. La pantalla dividida va por render-layer / SceneView (fase posterior). |
| `app.Player` **no declara `doUpdate`**, solo `doLateUpdate`. | El hook de personaje engancha toda la cadena de tipos (doUpdate y doLateUpdate) y soporta anidamiento. |
| Existe el flujo nativo `AdditionalPlayerLoadManager` con firmas reales. `app.Player` tiene `get_playerID` y un `padInput` propio. | Se usa ese flujo (no el clonado) y se traza como lo usa el propio juego. |

## Estado honesto
- **Verificado:** sintaxis y uso de la API de todos los `.cpp` contra tu `deps/reframework/API.hpp` real (clang, cabeceras Windows simuladas).
- **NO verificado:** compilacion con MSVC y ejecucion dentro del juego. No hay P2 jugable todavia.
- **Lo desconocido** (se averigua con los logs): si el cargador crea el personaje solo o hay que llamar a `activatePlayerResource`; si se registra solo en `PlayerManager`; los valores de enum validos.

## Instalacion (Visual Studio)
1. Copia **todos** estos archivos a la raiz del proyecto, sobrescribiendo (incluye los NUEVOS `tracer.cpp/.hpp`).
2. *Proyecto -> Eliminar cache y volver a configurar CMake* (el `CMakeLists.txt` ahora usa `CONFIGURE_DEPENDS`).
3. Compila en **Release**. Opcional: define `DMC5_DIR` y la DLL se copia sola a `reframework\plugins`.
4. Copia `dmc5_ss_dump_request.txt` y `dmc5_ss_config.txt` a `<juego>\reframework\data\`.

## Prueba de observabilidad (haz esto PRIMERO)
Abre el juego y mira `<juego>\reframework\data\dmc5_ss.log`. La primera linea debe ser:
```
==== dmc5_splitscreen compilado <FECHA HORA> ====
```
- **Si la fecha/hora NO es la de tu ultima compilacion** -> esta cargando una DLL vieja (copia la nueva).
- **Si el archivo no existe** -> la DLL no carga o no se ejecuta; mira `re2_framework_log.txt`. El banner tambien indica ruta y codigo de error de Windows si no pudo abrir el archivo (probara `%TEMP%\dmc5_ss.log`).
- Crea `reframework\data\dmc5_ss_cmd.txt` con la linea `ping`: en ~0,3 s debe aparecer `pong` y el archivo se borra. (O pulsa F5.) Si sale `pong`, el canal de ordenes funciona.

## Ordenes (archivo `dmc5_ss_cmd.txt`, una por linea, o tecla)
| Orden | Tecla | Efecto |
|---|---|---|
| `ping` | F5 | prueba |
| `state` | F6 | jugadores, cargador adicional, sonda de camara |
| `dump` | F7 | vuelca `dmc5_ss_dump_request.txt` |
| `recon` | F8 | enums (con valores), cargador, CharacterFactory, PadInput, render-layer |
| `p2load` | F9 | pide al juego un jugador adicional (flujo nativo) |
| `p2activate` | F10 | `activatePlayerResource()` del cargador |
| `p2finish` | F11 | `requestFinishPlayerResource` |
| `trace on/off/sum` | - | trazado del flujo nativo |

## Secuencia de prueba
1. Entra en una **mision** y espera a controlar a tu personaje.
2. `recon` y `state`.
3. Reinicia una mision / cambia de personaje si el juego lo permite, para que el propio juego ejecute la carga (se registra en `dmc5_ss_trace.log`).
4. `p2load`, espera unos segundos, `state`. Si `isReady` pasa a TRUE y no aparece P2, prueba `p2activate`, luego `state`.
5. Envia `dmc5_ss.log`, `dmc5_ss_trace.log` y `dmc5_ss_dump.log`.

> `p2load`/`p2activate` llaman a funciones reales del juego con argumentos de enum validos, pero es un experimento: puede fallar o cerrar el juego. Pruebalo offline.

## Archivos
`main.cpp` entrada y ordenes - `common.hpp` logger unico - `reflect_util.hpp` reflexion - `diagnostics.hpp` volcados - `tracer.*` trazado del flujo nativo - `player_manager.*` jugadores y carga - `input_isolation.*` XInput (con contadores) - `camera_manager.*` sonda (solo lectura).
