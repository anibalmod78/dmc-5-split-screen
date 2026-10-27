# DMC5 Split-Screen - v4

## Lo que demostraron tus logs (v3)
| Evidencia | Conclusion |
|---|---|
| Banner con fecha de compilacion, `ping`->`pong`, 24 hooks de traza instalados, `players: 0->1->2`, P1 = `app.PlayerVergilPL` | El diagnostico YA es fiable. Cargas la DLL correcta. |
| El juego crea sus jugadores con `CharacterFactory.create(id=500/600, costume=null, parent=<GameObject>, mode=InGame(3), finishEvent=<delegado>)` y luego `create(prefab, parent, true)` por cada prefab; el registro llega solo (`addPlayerCore`, luego `addPlayer`) | **Ese es el flujo real de creacion en mision.** |
| `AdditionalPlayerLoadManager` no se llamo NI UNA VEZ en toda la mision | Esa via es secundaria (la mantengo como experimento `p2load`). |
| `set_manualPlayer` aparecia con `this=app.PlayerChain` / `ActivateMotionSynchronizer` | Es un setter trivial cuyo codigo nativo comparte el compilador con otros tipos: ruido. Quitado de la traza. |
| `app.Player` no declara `doUpdate` pero la cadena Character/GameModel/... si; hooks instalados sin problema | OK |
| Cero llamadas a `XInputGetState(0)` | O no habia mando conectado, o el juego no usa esa funcion. **Conecta un mando y usa la orden `pad`.** |
| `get_FOV=0`, `Aspect=1e-19` de la camara | Error mio: el objeto es nativo y lo invocaba por el tipo equivocado. Corregido. |
| `app.PadInput` tiene `update()`, `setAxisRawL(x,y)`, `set_buttonOnFlags(uint)`, y cada `Player` tiene el suyo | Punto de enganche para el mando del P2 (siguiente fase). |
| `via.render.layer.Scene` tiene `get/set_Camera`, `get_Size`, `get_Region`, `OutputRenderTarget`; `via.SceneView.set_Size` | Hay palanca para la pantalla dividida, en la capa de render (fase posterior). |

## Que hace la v4
- **Graba** la llamada real del juego a `CharacterFactory.create` (padre + delegado) y **describe el delegado** (a que objeto y metodo apunta) en `dmc5_ss_trace.log`.
- **`p2spawn [id]`**: repite esa llamada con otro id, usando el padre y el delegado reales. Detecta al nuevo `Player` en `PlayerManager.players` y le engancha el aislamiento de entrada.
- **`pad`**: sonda de mandos (XInput directo + `via.hid.GamePad` por usuario).
- Sonda de camara corregida. `recon` ahora vuelca tambien `FactoryID`, `CostumeData`, `CharacterFactoryData`, `via.UserIndex` y `PlayerControlType`.

## Instalacion
Sobrescribe los archivos del proyecto con los del zip (**hay cambios en**: `main.cpp`, `tracer.*`, `player_manager.*`, `input_isolation.*`, `camera_manager.cpp`, `diagnostics.hpp`, `reflect_util.hpp`). Compila en Release.

## Prueba (en este orden)
1. **Con el plugin ya cargado**, entra en una mision (idealmente conecta un mando). Es imprescindible: `p2spawn` necesita haber visto el `create` de arranque de la mision. Si ya estabas en la mision, reiniciala.
2. Mira `dmc5_ss.log`: debe haber lineas `GRABADO CharacterFactory.create(id=..., mode=3)`.
3. Ordenes (archivo `dmc5_ss_cmd.txt`): `recon`, `state`, `pad`.
4. **`p2spawn`** (sin argumento usa el primer id grabado). Si el juego no se rompe, mira `dmc5_ss.log`: `isFinished`, `PlayerManager.players: 2 -> 3`, `P2 DETECTADO`. Prueba despues `p2spawn 600` (o el otro id grabado).
5. Envia `dmc5_ss.log`, `dmc5_ss_trace.log` y `dmc5_ss_dump.log`.

> Reutilizar el delegado del juego es un experimento: su manejador puede estar pensado para un solo jugador (podria reemplazar a P1, o fallar). El log dira que paso. Hazlo offline; puede cerrar el juego.

## Ordenes
`ping` F5 - `state` F6 - `dump` F7 - `recon` F8 - **`p2spawn [id]` F9** - `p2load` F10 (experimento APL) - `pad` F11 - `p2activate`, `p2finish`, `trace on|off|sum`.
