# To add

Limitaciones reales del compilador/lenguaje encontradas de pasada mientras se trabajaba
en otra cosa — no bloquean lo que se estaba haciendo en el momento (siempre hay un
workaround razonable), pero merece la pena arreglarlas más adelante. Cada entrada dice
qué falla, por qué, el workaround actual, y dónde mirar para arreglarlo de verdad.

---

## `type_of()` no conoce el tipo de retorno de una llamada a función de usuario — ARREGLADO

**Encontrado:** 2026-09-16, mientras se arreglaba que un `fn` suelto no podía usar
constructores de clase (ver el commit "Standalone functions can now use classes").

**Arreglado:** mismo día, commit "Resolve chained method calls on function/constructor
results". `FnSig` ganó un campo `devuelve` (el tipo de retorno declarado), rellenado por
`make_sig()`, y `type_of()`/`check_call` lo consultan en el caso `ExprKind::Call` (tanto
para una función suelta como para un método de clase) en vez de devolver `Type::unknown()`.
`check_call`'s propia resolución del receptor de un `.método()` (antes un chequeo a mano
solo para `Ident`/`this`) pasó a usar `type_of()` también, que es lo que de verdad
resuelve el ejemplo de abajo. Verificado con 60 peticiones seguidas contra un proceso
limpio (sin nada más escuchando en el puerto) — ver la nota al final sobre el
falso-positivo de no-determinismo que salió al verificarlo.

**Nota sobre un no-determinismo que resultó ser falso:** durante la verificación, el mismo
repro daba 204 en vez de `{"r":25}" en una fracción de las peticiones, de forma
aparentemente aleatoria, incluso con concurrencia y con ThreadSanitizer sin quejarse.
Resultó ser un artefacto del entorno de pruebas, no un bug: `SO_REUSEPORT` permite que
más de un proceso escuche el mismo puerto a la vez, y un proceso de la extensión de
VSCode de otro repo (`.../Github/Lux/build/lux .`) llevaba un rato escuchando por
casualidad en el mismo puerto (8098) usado para este repro de prueba en `/tmp`. El
kernel repartía las conexiones nuevas entre los dos procesos (pegajoso por conexión,
de ahí que pareciera "a veces sí, a veces no" pero siempre igual dentro de la misma
conexión) — el proceso ajeno no tiene esa ruta y devolvía 204 sin más. Confirmado
matando ambos procesos y repitiendo la prueba en un puerto verificado como libre:
60/60 peticiones correctas. Moraleja para la próxima vez que algo parezca no
determinista en un puerto reusado entre pruebas: `ss -tlnp | grep <puerto>` primero,
no asumir que solo el proceso propio está escuchando ahí.

**Qué falla:**

```lux
class Punto:
    int x
    int y

    fn int cuadrado():
        return this.x * this.x + this.y * this.y

fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    return { "r": hacer_punto(3, 4).cuadrado() }
```

Compila (`lux --check` no se queja), pero falla en tiempo de ejecución:

```
{"error":"Dicts have no method 'cuadrado'"}
```

**Por qué:** `Emitter::type_of()` (`src/lux_script/emitter.cpp`) solo sabe el tipo de una
expresión `Call` cuando es una llamada a MÉTODO sobre un receptor de tipo ya conocido
(`case ExprKind::Call: { if (!e.object || e.object->kind != ExprKind::Member) return
Type::unknown(); ... }`). Para una llamada a una función de usuario suelta
(`hacer_punto(3, 4)`), no hay ninguna rama que consulte `FunctionSigs`/el tipo de retorno
declarado de la función — cae directo a `Type::unknown()`. Sin saber que el resultado es
un `Punto`, el checker no puede resolver `.cuadrado()` sobre él, y en tiempo de
ejecución el valor (un `Value::Dict` normal y corriente) se despacha por la vía dinámica
genérica, que solo conoce los métodos de `Dict` (`has`/`keys`), de ahí el mensaje.

Confirmado que es **independiente** de si la llamada ocurre dentro de una ruta o dentro
de otro `fn` — falla igual en los dos sitios, así que no tiene relación con el fix de
`build_function_signatures()`/`emit_function_bodies()` que sí se hizo.

**Workaround actual (funciona perfectamente):** asignar el resultado a una variable
local con el tipo declarado antes de encadenar el método:

```lux
fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    Punto p = hacer_punto(3, 4)
    return { "r": p.cuadrado() }
```

Esto funciona porque `type_of(Ident)` sí devuelve el tipo declarado de la variable
(`local_type(e.text)`), a diferencia de `type_of(Call)`.

**Dónde mirar para arreglarlo:** `Emitter::type_of()`, caso `ExprKind::Call`
(`src/lux_script/emitter.cpp`, cerca de la línea 538 en el commit donde se encontró esto).
Necesitaría una rama nueva: si `e.object->kind == ExprKind::Ident` y ese nombre resuelve
en `functions_` (la `FunctionSigs` que el `Emitter` ya recibe), devolver el tipo de
retorno declarado de esa función — análogo a como ya se resuelve el retorno de un
método (`m.devuelve ? Type::from_legacy_name(m.devuelve) : recv`), pero mirando
`FnSig`/lo que sea que guarde el tipo de retorno de una función suelta en vez de un
método. Comprobar también si `check_call`/`emit_call` tienen el mismo tipo de hueco
para otras formas encadenadas (`fn_que_devuelve_list()[0].campo`, etc.) una vez se
toque esto, no solo el caso de un método.

---

## `:param.ext` en un patrón de ruta no liga el parámetro — y `{param}.ext` "compila" pero liga el valor equivocado

**Encontrado:** 2026-09-17, reportado desde Homeflix al intentar `get endpoint("/hls/:n.ts", int n)`
para servir segmentos HLS con la extensión incrustada en la URL.

**Qué falla — dos capas, no solo una:**

1. `:n.ts` da un error de compilación confuso:
   ```
   error: the pattern declares ':n.ts' but no parameter binds it
   ```
   Esto viene de `pattern_params()` (`src/lux_script/project.cpp`), que para la sintaxis `:nombre`
   busca el siguiente `/` como delimitador de cierre (`char close = (pattern[i] == '{') ? '}' : '/';`)
   — para `:n.ts`, el "nombre" que extrae es literalmente `n.ts` (todo hasta la barra o el final),
   no `n`. El checker exige entonces un parámetro llamado `n.ts`, que nadie declara nunca.

2. Cambiar a `{n}.ts` (delimitador `}`, distinto en `pattern_params()`) hace que el checker SÍ
   extraiga `n` correctamente y compile limpio — pero **el valor que llega en tiempo de
   ejecución es el equivocado**, no un error: `GET /segment/42.ts` contra `get
   endpoint("/segment/{n}.ts", int n)` devuelve `{"n":0}`, no `{"n":42}`. Esto es un bug
   **distinto y más profundo**, en el router de C++ (`src/router.cpp`), no en el checker de
   Lux Script:
   - `Router::normalize_pattern()` convierte `{n}.ts` a `:n.ts` ANTES de registrar la ruta
     (sustituye `{` por `:` y quita el `}`, dejando todo lo demás igual) — así que en tiempo de
     ejecución `{n}.ts` y `:n.ts` son exactamente el mismo patrón interno.
   - `Router::add_internal()` (línea `name = seg.substr(1);`) trata el segmento entero
     `n.ts` (todo lo que sigue a `:`) como el NOMBRE del parámetro — liga
     `params["n.ts"] = "42.ts"` (el valor CRUDO del segmento completo), nunca `params["n"]`.
   - El binding de Lux Script busca `req.params["n"]` (porque el checker, vía la sintaxis
     `{n}`, cree que el parámetro se llama `n`) — no lo encuentra, y el `int n` cae a su valor
     por defecto (`0`), sin error ni aviso.

   Conclusión: **el router no soporta en absoluto un parámetro combinado con texto literal
   dentro del mismo segmento** (`:id.json`, `file-:id`, `{id}.ts`, lo que sea) — no es un hueco
   del checker nada más, es una limitación real de emparejamiento de segmentos. Un segmento de
   patrón solo puede ser enteramente estático, enteramente un parámetro, o `*`.

**Workaround actual (funciona perfectamente):** usar un segmento de ruta limpio para el
parámetro, sin incrustar la extensión (`/segment/:n` en vez de `/segment/:n.ts`) — HLS no exige
que la URL termine literalmente en `.ts`, basta con el `Content-Type` de la respuesta.

**Dónde mirar para arreglarlo:**
- `pattern_params()` (`src/lux_script/project.cpp`, cerca de la línea 373): tendría que saber
  parsear un segmento MIXTO (`:nombre` seguido de texto literal antes del `/`), no solo
  `:nombre-hasta-la-barra` o `{nombre}`.
- `Router::normalize_pattern()`/`Router::add_internal()`/`Router::match_recursive()`
  (`src/router.cpp`): el `Node` de tipo `PARAM` tendría que poder llevar un sufijo/prefijo
  literal, y `match_recursive()` tendría que comprobar ese sufijo contra el segmento real
  ANTES de aceptar el match y extraer solo la parte variable como valor — hoy asume que un
  segmento `PARAM` consume el segmento completo, sin más.
- Los dos ficheros tienen que quedar consistentes entre sí (el nombre que el checker de Lux
  Script cree que tiene el parámetro tiene que ser EXACTAMENTE la clave que el router real usa
  al ligar `params[...]`) — el bug de `{n}.ts` de arriba es precisamente esa inconsistencia.

---

## `/*` no cubre la ruta raíz vacía `/`

**Encontrado:** 2026-09-17, reportado desde Homeflix al montar un fallback SPA con
`any endpoint("/*")`.

**Qué pasa:** `get endpoint("/*")` (o `any`) coincide con `/algo`, `/a/b/c`, etc., pero NO con
`/` a secas — hace falta al menos un carácter después de la barra. No está confirmado si es
intencional (un wildcard normalmente implica "uno o más segmentos", no "cero o más"), pero
sorprende si no se dice en ningún sitio, así que hay que documentarlo bien visible aunque se
decida no cambiar el comportamiento.

**Workaround actual (funciona perfectamente):** declarar también un `get endpoint("/")`
explícito junto al comodín `/*`.

**Dónde mirar:** `Router::match_recursive()`/`split_path()` (`src/router.cpp`) — `/` produce
una lista de segmentos VACÍA (`split_path` descarta segmentos vacíos), así que el nodo
`WILDCARD` nunca llega a probarse para ella (el bucle `for (const auto& seg : segments)` en
`match_recursive` no itera nada, y el caso terminal en `index == segments.size()` solo mira
`node->handlers`, no los hijos wildcard del nodo raíz). Si se decide soportarlo, sería en ese
caso terminal: comprobar también `node->find_wildcard_child()` cuando `index == segments.size()`
antes de darse por vencido. Si se decide NO soportarlo (razonable: es coherente con que un
wildcard siempre capture "el resto de la ruta", nunca "nada"), basta con una nota en GUIDE.md
junto a la sección de rutas.
