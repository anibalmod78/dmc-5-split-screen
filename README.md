# DMC5 Split-Screen - v8

## Resultado de la prueba v7 (mision de Dante) y que anade v8
| Evidencia (logs v7) | Conclusion |
|---|---|
| En la mision de Dante `PlayerManager.players` tiene **3** Player reales: Dante (P1), V (`pl0200_v`) y Nero (`pl0030_nero`), cada uno con su GO raiz (`app.Player`+`app.PlayerX`), sin padre | Los Player son GameObjects raiz de la escena. El juego tiene varios a la vez. |
| La traza muestra al propio juego haciendo `requestPreloadPlayerResource(Nero)`, `(Gilver)` y luego `requestLoadPlayerResource(Nero, Default, Dirty, reload=true)` / `(Gilver, Default, DirtyM, true)`; ~570 frames despues `addPlayer(V)` y `addPlayer(Nero)` solos. **No** se llama a `activatePlayerResource` | **`AdditionalPlayerLoadManager` ES la via nativa para tener otro personaje, y admite cualquier `PlayerID`** -> encaja con que P2 elija personaje. |
| Cada Player tiene su propio `PadInput` (Dante `...36C0`, V `...E9B0`, Nero `...58F0`); solo 2 PadInput globales reciben `update()` | Los PadInput de V y Nero no se actualizan solos (el juego solo actualiza el del jugador manual). |
| `PlayerControlType`: Manual=0, Auto=1. `Player` tiene `warp/requestWarp`, `isManuallyControlled`, `playAIMode`, `aiModeController` | Palancas para convertir un Player existente en P2. |
| `via.hid.GamePad` expone `getDevice(UserIndex[,..])`, `getMergedDevice(UserIndex)`, `createVirtualDevice(...)` | Posible via de entrada: que el PadInput de P2 lea el usuario 1. |
| `p2spawn` -> "NO hay ningun create InGame grabado" (cargaste el plugin con la mision ya empezada o el create de ESTA mision ocurrio antes del plugin) | Irrelevante ya: p2spawn queda obsoleto. |
| Tras "ORDEN: p2spawn" el log no muestra mas errores; el juego siguio estable | OK |

**Que hace v8 (primeros cambios de comportamiento, todos opcionales y por orden):**
- `p2load <PlayerID>`: ahora con `reload=true` y detecta al jugador nuevo (no a los que ya existian).
- `p2adopt <PlayerID>`: toma como P2 a un Player YA existente (p.ej. Nero o V en la mision de Dante).
- `p2warp [request]`: lo coloca junto a P1. `p2manual` / `p2auto`: `set_playerControlType` Manual/Auto.
- `p2pad on [usuario]` / `p2pad off` / `p2pad`: su `PadInput` lee `via.hid.GamePad` del usuario indicado (por defecto 1) y se le llama a `update()` si el juego no lo hace. **Siempre** (aunque no uses p2pad) se registra en el log con que `UserIndex` lee cada `PadInput`.
- `state` muestra, por jugador: `controlType`, `isManuallyControlled`, `isMasterManualPlayer`, `aiModeController`, y estado de su `PadInput`.

**Prueba sugerida (en la mision de Dante con Nero y V):**
1. `state` (mira las lineas `GamePad.... llamado DENTRO de PadInput.update`).
2. `p2adopt Nero`, `p2warp`, `p2manual`.
3. Sin segundo mando: `p2pad on 0` (Nero leera TU mando: si se mueve, el camino de control funciona). Con segundo mando: `p2pad on 1`. `p2pad off` para soltar.
4. `state` y envia los tres logs. Hazlo offline; puede cerrarse el juego.
5. En una mision con un solo personaje: `p2load Nero` (o `p2load Vergil`...) y luego `state`.

## Resultado de la prueba v6 (y que anade v7)
| Evidencia (logs v6) | Conclusion |
|---|---|
| `FinishCreationEvent.Invoke` enganchado (hook #22) pero **0 disparos**, ni en el juego ni en `p2spawn` | El delegado no pasa por ese `Invoke` gestionado (llamada nativa/inlined). Callejon sin salida; se deja con limite bajo. |
| Los `PlayerV`/`PlayerVergilPL` viven en direcciones bajas (`0x1EF1C4A8E20`), asignados mucho antes de `create` | Los Player **ya existen en la escena de la mision**; `create(prefab, padre, true)` solo les cuelga cuerpo/cara. `addPlayerCore` llega ~2000 frames despues (al activarse). Por eso repetir `create` no crea jugadores. |
| El padre grabado tiene un componente `app.character.Character`; los GO instanciados no tienen `Player`/`Character` | Consistente con lo anterior (el padre es el GO logico del personaje). |
| `GO='<string?>'` | Mi lectura de `System.String` fallo. v7 anade lectura por offsets. |
| Dos `app.PadInput` distintos actualizandose desde el menu (`...346B0`, `...3EAF0`); todos los jugadores usan el mismo | Hay 2 PadInput globales. v7 los lista con su estado (flags, ejes). `PadInput` no tiene campo de indice de mando: lee `via.hid.GamePad` (mezcla de dispositivos). |
| `findPlayer` se dispara CADA frame desde antes de la mision | Limite de traza bajado a 3 (el contador sale con `trace sum`). |
| La mision de prueba es V+Vergil (ambos en la misma posicion: fusion) | **No es representativa.** Prueba v7 en una mision normal (Nero o Dante). |

**v7 solo observa**: nombres de GameObject (corregido), componentes con su tipo real (y los de cada Player para validar la sonda), hijos de cada GO, y todos los `PadInput` (estado y campos `padInput/enemyPadInput/tracePadInput/CachedPadInput` de P1).

## Resultado de la prueba v5 (y que anade v6)
| Evidencia (logs v5) | Conclusion |
|---|---|
| `PlayerManager cambio: 0 -> ...63220`; `state` vio 2 jugadores, P1=VergilPL, 2 grabaciones con su `request` | Correccion de v5 **verificada**. |
| `p2spawn` (id=500) x2: `create -> ok`, `isFinished -> TRUE` en ~20 frames, el juego NO se rompio, pero `players` siguio en 2 y no hubo `addPlayerCore/addPlayer` | **Repetir `create` con el delegado del juego NO crea un Player.** Solo se instancian 2 GameObjects (cuerpo + cara) colgados del padre grabado (posible basura en escena). |
| En el original: `create` en f5630, y `addPlayerCore(PlayerV)` ~2000 frames despues (f7587), `addPlayer` en f8012 | El registro NO ocurre dentro de `create`/del delegado inmediato: lo dispara otra cosa mas tarde (inicio de mision / `findPlayer` / inicializacion del componente). |
| `isFinished` y el delegado: `describe_delegate` da `target=null` | No se puede leer el manejador asi. |
| Fin de mision: `unload(RequestData)` -> las grabaciones se descartan solas | Ciclo de vida OK. |

**v6 solo observa** (no cambia el comportamiento): traza `FinishCreationEvent.Invoke`, `PlayerManager.findPlayer/updateMissionInitCheck/initCostumeInfo`; registra los GameObjects que devuelve `create(prefab,padre,true)` y, con `state` y 90 frames despues de `p2spawn`, imprime nombre, padre y si llevan componente `Player`/`Character`; `state` y las lineas `player[i]` ahora incluyen el nombre del GameObject y de su padre. El volcado `recon` incluye `via.GameObject`, `via.Transform` y metodos de arranque de `app.Player`.

## Lo que demostraron los logs de la prueba v4 (y que corrige v5)
| Evidencia | Conclusion |
|---|---|
| `state` dio `getPlayerNum=-1` y `p2spawn` dijo "no hay P1" **durante una mision** (frames 14512-18191, la 2a mision) | **Bug mio**: el plugin cacheaba el `PlayerManager` de la 1a mision (`...F6CF0`). El trazado muestra que el juego crea uno NUEVO por mision (`...83220`). v5 lo resuelve de nuevo cada 30 frames y antes de cada orden. |
| `P1 desaparecio ... se descartan grabaciones` a los 2 frames de grabar la 2a mision | **Bug mio**: al detectar el fin de la 1a mision borraba tambien las grabaciones recien hechas. v5 las ata al `RequestData` que devuelve `create` y solo las olvida cuando el juego llama a `CharacterFactory.unload(request)`. |
| `p2spawn`/`create` propio se grababa a si mismo | v5 lo evita. |
| En la mision, V (`PlayerV`) y Vergil (`PlayerVergilPL`, P1) comparten el MISMO `padInput` (`...17CB36C0`) | Dato clave para la fase de entrada: el `PadInput` no es por personaje, sino por pad/usuario. |
| `get_FOV=0` otra vez | Mi "arreglo" de v4 no era la causa: probablemente el float nativo vuelve por xmm0 y no en el buffer. v5 imprime los bytes crudos; no es critico. |
| XInput directo: mando 0 conectado, `XInputGetState` del juego = 0 llamadas; `via.hid.GamePad` cuenta 1 dispositivo | El juego NO usa XInputGetState: lee los mandos por `via.hid.GamePad`. El aislamiento por MinHook de XInput no sirve; hay que actuar sobre `PadInput`/`GamePad`. |

## Lo que sigue de v4

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

## Que hace (v4 + v5)
- **Graba** la llamada real del juego a `CharacterFactory.create` (padre + delegado) y **describe el delegado** (a que objeto y metodo apunta) en `dmc5_ss_trace.log`.
- **`p2spawn [id]`**: repite esa llamada con otro id, usando el padre y el delegado reales. Detecta al nuevo `Player` en `PlayerManager.players` y le engancha el aislamiento de entrada.
- **`pad`**: sonda de mandos (XInput directo + `via.hid.GamePad` por usuario).
- Sonda de camara corregida. `recon` ahora vuelca tambien `FactoryID`, `CostumeData`, `CharacterFactoryData`, `via.UserIndex` y `PlayerControlType`.

## Instalacion
Sobrescribe los archivos del proyecto con los del zip (**hay cambios en**: `main.cpp`, `tracer.*`, `player_manager.*`, `input_isolation.*`, `camera_manager.cpp`, `diagnostics.hpp`, `reflect_util.hpp`). Compila en Release.

## Prueba (en este orden)
1. **Con el plugin ya cargado**, entra en una mision (idealmente conecta un mando). Es imprescindible: `p2spawn` necesita haber visto el `create` de arranque de la mision. Si ya estabas en la mision, reiniciala. (Ahora tambien sirve la 2a, 3a... mision.)
2. Mira `dmc5_ss.log`: debe haber lineas `GRABADO CharacterFactory.create(id=..., mode=3)`.
3. Ordenes (archivo `dmc5_ss_cmd.txt`): `recon`, `state`, `pad`.
4. **`p2spawn`** (sin argumento usa el primer id grabado). Si el juego no se rompe, mira `dmc5_ss.log`: `isFinished`, `PlayerManager.players: 2 -> 3`, `P2 DETECTADO`. Prueba despues `p2spawn 600` (o el otro id grabado).
5. Envia `dmc5_ss.log`, `dmc5_ss_trace.log` y `dmc5_ss_dump.log`.

> Reutilizar el delegado del juego es un experimento: su manejador puede estar pensado para un solo jugador (podria reemplazar a P1, o fallar). El log dira que paso. Hazlo offline; puede cerrar el juego.

## Ordenes
`ping` F5 - `state` F6 - `dump` F7 - `recon` F8 - **`p2spawn [id]` F9** - `p2load` F10 (experimento APL) - `pad` F11 - `p2activate`, `p2finish`, `trace on|off|sum`.

## v9 - Teclas para P2 (ventana del juego en primer plano)

| Tecla | Orden |
|---|---|
| RePag / AvPag | elegir personaje de P2 (Nero, Dante, Vergil, Gilver) |
| F9 | `p2adopt <pj>` |
| F10 | `p2load <pj>` |
| F12 / Ctrl+F12 | `p2warp` / `p2warp request` |
| Ctrl+F5 / Ctrl+F6 | `p2manual` / `p2auto` |
| Ctrl+F7 / Ctrl+F8 / Ctrl+F9 | `p2pad on 1` / `p2pad off` / `p2pad status` |
| Ctrl+F10 / Ctrl+F11 | `p2activate` / `p2finish` |

F5 ping, F6 state, F7 dump, F8 recon, F11 pad siguen igual. El archivo `dmc5_ss_cmd.txt` sigue funcionando.
